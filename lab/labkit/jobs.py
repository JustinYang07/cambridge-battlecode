"""Background queue for the web lab: experiments run one at a time (each uses all cores)."""
from __future__ import annotations

import queue
import threading
import time
import traceback

from . import runner


class JobQueue:
    def __init__(self, store: runner.ExperimentStore) -> None:
        self.store = store
        self._queue: "queue.Queue[dict]" = queue.Queue()
        self._cancel: set[str] = set()
        self.current: str | None = None
        self.log: list[str] = []
        threading.Thread(target=self._worker, name="lab-jobs", daemon=True).start()

    def submit(self, spec: runner.ExperimentSpec) -> dict:
        exp = runner.plan(spec)
        self.store.save(exp)
        self._queue.put(exp)
        self._log(f"queued {exp['id']} ({len(exp['games'])} games)")
        return exp

    def cancel(self, exp_id: str) -> None:
        self._cancel.add(exp_id)
        self._log(f"cancel requested for {exp_id}")

    def status(self) -> dict:
        return {"current": self.current, "queued": self._queue.qsize(), "log": self.log[-40:]}

    def _log(self, line: str) -> None:
        self.log.append(time.strftime("%H:%M:%S ") + line)
        del self.log[:-400]

    def _worker(self) -> None:
        while True:
            exp = self._queue.get()
            if exp["id"] in self._cancel:
                exp["status"] = "cancelled"
                self.store.save(exp)
                continue
            self.current = exp["id"]
            self._log(f"running {exp['id']}")
            try:
                runner.execute(exp, self.store, should_stop=lambda: exp["id"] in self._cancel)
                self._log(f"finished {exp['id']}: {exp['status']}")
            except Exception:  # noqa: BLE001 - keep the worker alive and surface the error
                exp["status"], exp["error"] = "failed", traceback.format_exc()[-3000:]
                self.store.save(exp)
                self._log(f"failed {exp['id']}")
            finally:
                self.current = None
