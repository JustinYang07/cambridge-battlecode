#!/usr/bin/env bash
# Run lab A/B tests on a Google Cloud spot VM (96 vCPUs), then stop it.
#
#   lab/cloud/gcab.sh ab <abtest.py args...>   # start VM, sync code, run, copy results back, stop VM
#   lab/cloud/gcab.sh up | sync | ssh | down | delete | status
#
# The VM is stopped (not deleted) after each run: a stopped VM costs only its disk (~$5/month),
# and the next start skips setup. lab/data (experiments, platform cookie) is never uploaded;
# only the new experiment folders come back.
set -euo pipefail

PROJECT=${GCAB_PROJECT:-project-c1e605ac-d0f1-4500-85e}
NAME=${GCAB_NAME:-cambc-ab}
MACHINE=${GCAB_MACHINE:-n2-highcpu-96}
ZONES=${GCAB_ZONES:-"europe-west2-b europe-west2-c europe-west2-a"}
CAMBC_VERSION=3.0.1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KEY=$HOME/.ssh/google_compute_engine
REMOTE=cb
G="gcloud --project $PROJECT --quiet"

zone_of() { $G compute instances list --filter="name=$NAME" --format="value(zone.basename())" 2>/dev/null; }
ip_of() { $G compute instances describe "$NAME" --zone "$1" --format="value(networkInterfaces[0].accessConfigs[0].natIP)"; }
ssh_opts() { echo "-i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ServerAliveInterval=30"; }

create() {
    for z in $ZONES; do
        echo "creating $NAME ($MACHINE, spot) in $z..." >&2
        if $G compute instances create "$NAME" --zone "$z" --machine-type "$MACHINE" \
            --provisioning-model=SPOT --instance-termination-action=STOP \
            --image-family=ubuntu-2404-lts-amd64 --image-project=ubuntu-os-cloud \
            --boot-disk-size=30GB --boot-disk-type=pd-balanced >&2; then
            echo "$z"; return 0
        fi
    done
    echo "no zone had capacity" >&2; return 1
}

up() {
    local z; z=$(zone_of)
    if [ -z "$z" ]; then
        z=$(create)
    else
        local st; st=$($G compute instances describe "$NAME" --zone "$z" --format="value(status)")
        [ "$st" = RUNNING ] || { echo "starting $NAME in $z..." >&2; $G compute instances start "$NAME" --zone "$z" >&2; }
    fi
    # First ssh through gcloud puts our key on the VM (and makes ~/.ssh/google_compute_engine).
    for i in $(seq 1 30); do
        $G compute ssh "$NAME" --zone "$z" --command true >/dev/null 2>&1 && break
        sleep 5
    done
    local ip; ip=$(ip_of "$z")
    # One-time setup: Python venv with the same cambc as here.
    ssh $(ssh_opts) "$USER@$ip" "test -x $REMOTE/.venv/bin/cambc || (sudo apt-get update -qq && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq python3-venv rsync git >/dev/null && mkdir -p $REMOTE/lab/data/experiments && python3 -m venv $REMOTE/.venv && $REMOTE/.venv/bin/pip install -q cambc==$CAMBC_VERSION)" >&2
    # Same cambc (engine) as here: upgrade when the pinned version changes (balance patches).
    ssh $(ssh_opts) "$USER@$ip" "test \"\$($REMOTE/.venv/bin/cambc --version)\" = 'cambc $CAMBC_VERSION' || $REMOTE/.venv/bin/pip install -q --upgrade cambc==$CAMBC_VERSION" >&2
    echo "$ip"
}

sync_code() {
    local ip=$1
    rsync -az --delete -e "ssh $(ssh_opts)" \
        --exclude .git --exclude .venv --exclude '.cambc' --exclude 'build/' --exclude 'target/' \
        --exclude 'lab/data' --exclude 'lab/nn' --exclude 'replays' --exclude 'matches' \
        --exclude '*.zip' --exclude '.DS_Store' --exclude 'compile_commands.json' --exclude '__pycache__' \
        --exclude 'platform.json' \
        "$ROOT/bots" "$ROOT/lab" "$ROOT/maps" "$ROOT/sdk" "$ROOT/cmake" "$ROOT/cambc.toml" "$USER@$ip:$REMOTE/"
}

down() {
    local z; z=$(zone_of)
    [ -n "$z" ] && $G compute instances stop "$NAME" --zone "$z" >&2 && echo "stopped $NAME" >&2 || true
}

cmd=${1:-}; shift || true
case "$cmd" in
up) up ;;
sync) ip=$(up); sync_code "$ip" ;;
ssh) z=$(zone_of); ssh $(ssh_opts) "$USER@$(ip_of "$z")" ;;
down) down ;;
delete) z=$(zone_of); [ -n "$z" ] && $G compute instances delete "$NAME" --zone "$z" ;;
status) $G compute instances list --filter="name=$NAME" ;;
ab)
    trap down EXIT  # stop the VM however the run ends
    ip=$(up)
    sync_code "$ip"
    args=$(printf '%q ' "$@")
    ssh $(ssh_opts) "$USER@$ip" "cd $REMOTE && python3 lab/abtest.py $args --jobs \$(nproc)"
    rsync -az -e "ssh $(ssh_opts)" "$USER@$ip:$REMOTE/lab/data/experiments/" "$ROOT/lab/data/experiments/"
    ;;
*) sed -n 2,9p "$0"; exit 1 ;;
esac
