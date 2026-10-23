"""Bounded process scheduling shared by the C++ quality tools."""

import codecs
from concurrent.futures import Future, ThreadPoolExecutor, as_completed
from dataclasses import dataclass
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
from typing import Callable, Mapping, Sequence

from quality_cpu import CpuBudget, detect_budget


@dataclass(frozen=True)
class CommandTask:
    label: str
    command: Sequence[str]
    cwd: str | None = None
    env: Mapping[str, str] | None = None


def resolve_jobs(
    requested: int | None = None,
    *,
    cpu_count: int | None = None,
    budget: CpuBudget | None = None,
) -> int:
    if requested is None:
        configured = os.environ.get("QUALITY_JOBS", "auto").strip()
        if configured and configured != "auto":
            try:
                requested = int(configured)
            except ValueError as error:
                raise ValueError("QUALITY_JOBS must be auto or a positive integer") from error
    if requested is not None:
        if requested <= 0:
            raise ValueError("quality jobs must be a positive integer")
        return requested
    return (budget or detect_budget(cpu_count)).workers


class _JobOutput:
    def __init__(self, total: int, workers: int, tool: str, progress: Callable[[str], None] | None, budget: CpuBudget):
        self.total = total
        self.budget = budget
        self.completed = 0
        self.failed = 0
        self.running = 0
        self.workers = workers
        self.tool = tool
        self.progress = progress or (lambda message: print(message, flush=True))
        self.lock = threading.Lock()
        self.started = time.monotonic()
        self.started_at = time.time()
        configured = os.environ.get("QUALITY_PROGRESS_FILE")
        self.path = Path(configured) if configured else Path(
            f".agent-workspace/quality/progress-{tool}-{os.getpid()}.jsonl"
        )
        self.status_path = self.path.with_suffix(".status.json")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.journal = self.path.open("w", encoding="utf-8")
        self.emit("schedule", workers=workers)

    def emit(self, event: str, **details: object) -> None:
        record = {
            "event": event, "total": self.total,
            "completed": self.completed, "failed": self.failed,
            "running": self.running, "jobs": self.workers, "tool": self.tool,
            "cpu_mode": self.budget.mode, "cpu_sets": self.budget.cpus,
            "started_at": self.started_at,
            "elapsed_seconds": round(time.monotonic() - self.started, 3),
            **details,
        }
        self.journal.write(json.dumps(record, ensure_ascii=False) + "\n")
        self.journal.flush()
        temporary = self.status_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(record, ensure_ascii=False), encoding="utf-8")
        for attempt in range(10):
            try:
                temporary.replace(self.status_path)
                break
            except PermissionError:
                if attempt == 9:
                    raise
                time.sleep(0.01)  # A Windows status reader may briefly hold it open.
        label = details.get("label", "")
        self.progress(
            f"[quality] {event} {self.completed}/{self.total} "
            f"failed={self.failed} elapsed={record['elapsed_seconds']}s "
            f"{('workers=' + str(details['workers'])) if 'workers' in details else label}"
        )

    def close(self) -> None:
        self.journal.close()


def _execute_task(
    task: CommandTask,
    output: _JobOutput,
    processes: set[subprocess.Popen[bytes]],
    process_lock: threading.Lock,
    cancelled: threading.Event,
    budget: CpuBudget,
) -> int:
    if cancelled.is_set():
        return 130
    with output.lock:
        output.running += 1
        output.emit("start", label=task.label)
    with tempfile.TemporaryFile() as diagnostics:
        try:
            with process_lock:
                if cancelled.is_set():
                    process = None
                else:
                    process = subprocess.Popen(
                        task.command, cwd=task.cwd, env=task.env,
                        stdout=diagnostics, stderr=subprocess.STDOUT,
                        start_new_session=os.name != "nt",
                    )
                    processes.add(process)
                    try:
                        budget.apply(process)
                    except OSError:
                        process.kill()
                        process.wait()
                        processes.discard(process)
                        raise
            result = process.wait() if process else 130
            if process:
                with process_lock:
                    processes.discard(process)
        except OSError as error:
            diagnostics.write((f"Cannot run {task.label}: {error}\n").encode("utf-8"))
            result = 1
        with output.lock:
            diagnostics.seek(0)
            decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
            try:
                while chunk := diagnostics.read(65536):
                    sys.stdout.write(decoder.decode(chunk))
                sys.stdout.write(decoder.decode(b"", final=True))
                sys.stdout.flush()
            except Exception:
                result = 1
                raise
            finally:
                output.completed += 1
                output.running -= 1
                output.failed += int(result != 0)
                output.emit("failed" if result else "passed", label=task.label, exit_code=result)
        return result


def run_jobs(
    tasks: Sequence[CommandTask],
    jobs: int | None = None,
    progress: Callable[[str], None] | None = None,
) -> int:
    # Redirected Windows Python streams otherwise use the ANSI code page, which
    # cannot publish valid UTF-8 compiler diagnostics such as multiplication signs.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    budget = detect_budget()
    workers = resolve_jobs(jobs, budget=budget)
    if not tasks:
        return 0
    workers = min(workers, len(tasks))
    tool = Path(tasks[0].command[0]).stem
    print(f"[quality] CPU mode={budget.mode}; workers={workers}; selected CPUs={budget.cpus or 'OS scheduler'}", flush=True)
    output = _JobOutput(len(tasks), workers, tool, progress, budget)
    processes: set[subprocess.Popen[bytes]] = set()
    process_lock = threading.Lock()
    cancelled = threading.Event()
    executor = ThreadPoolExecutor(max_workers=workers)
    futures: list[Future[int]] = []
    failed = False
    try:
        futures = [executor.submit(
            _execute_task, task, output, processes, process_lock, cancelled, budget
        ) for task in tasks]
        for future in as_completed(futures):
            try:
                failed |= future.result() != 0
            except Exception as error:
                failed = True
                print(f"Quality worker publication failed: {error}", file=sys.stderr, flush=True)
        exit_code = int(failed)
        with output.lock:
            output.emit("complete", exit_code=exit_code,
                        verdict="failed" if failed else "passed")
        return exit_code
    except KeyboardInterrupt:
        cancelled.set()
        with process_lock:
            for process in processes:
                try:
                    if os.name == "nt":
                        subprocess.run(
                            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            check=False,
                        )
                    else:
                        os.killpg(process.pid, signal.SIGTERM)
                except OSError:
                    pass  # The owned compiler may already have exited.
        for future in futures:
            future.cancel()
        return 130
    finally:
        executor.shutdown(wait=True, cancel_futures=cancelled.is_set())
        if cancelled.is_set():
            with output.lock:
                output.emit("interrupted")
        output.close()
