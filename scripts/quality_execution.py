"""Bounded process scheduling shared by the C++ quality tools."""

import codecs
from concurrent.futures import Future, ThreadPoolExecutor, as_completed
import ctypes
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


@dataclass(frozen=True)
class CommandTask:
    label: str
    command: Sequence[str]
    cwd: str | None = None
    env: Mapping[str, str] | None = None


def available_memory_bytes() -> int | None:
    if os.name == "nt":
        class MemoryStatus(ctypes.Structure):
            _fields_ = [
                ("length", ctypes.c_ulong), ("load", ctypes.c_ulong),
                ("total_physical", ctypes.c_ulonglong),
                ("available_physical", ctypes.c_ulonglong),
                ("total_page", ctypes.c_ulonglong),
                ("available_page", ctypes.c_ulonglong),
                ("total_virtual", ctypes.c_ulonglong),
                ("available_virtual", ctypes.c_ulonglong),
                ("available_extended", ctypes.c_ulonglong),
            ]

        status = MemoryStatus()
        status.length = ctypes.sizeof(status)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return int(status.available_physical)
        return None
    try:
        return int(os.sysconf("SC_AVPHYS_PAGES") * os.sysconf("SC_PAGE_SIZE"))
    except (AttributeError, OSError, ValueError):
        return None


def resolve_jobs(
    requested: int | None = None,
    memory_per_worker_bytes: int = 768 * 1024 * 1024,
    *,
    cpu_count: int | None = None,
    memory_bytes: int | None = None,
) -> int:
    if memory_per_worker_bytes <= 0:
        raise ValueError("memory_per_worker_bytes must be positive")
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
    available_cpus = cpu_count
    if available_cpus is None:
        available_cpus = getattr(os, "process_cpu_count", os.cpu_count)() or 1
    memory = memory_bytes if memory_bytes is not None else available_memory_bytes()
    if memory is None:
        return max(1, available_cpus)
    # Leave headroom for the GUI, IDE and operating system. Explicit --jobs or
    # QUALITY_JOBS overrides this estimate when the operator knows the workload.
    memory_workers = memory * 3 // 4 // memory_per_worker_bytes
    return max(1, min(available_cpus, memory_workers))


class _JobOutput:
    def __init__(self, total: int, workers: int, tool: str, progress: Callable[[str], None] | None):
        self.total = total
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
    memory_per_worker_bytes: int = 768 * 1024 * 1024,
    progress: Callable[[str], None] | None = None,
) -> int:
    # Redirected Windows Python streams otherwise use the ANSI code page, which
    # cannot publish valid UTF-8 compiler diagnostics such as multiplication signs.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    workers = resolve_jobs(jobs, memory_per_worker_bytes)
    if not tasks:
        return 0
    workers = min(workers, len(tasks))
    tool = Path(tasks[0].command[0]).stem
    output = _JobOutput(len(tasks), workers, tool, progress)
    processes: set[subprocess.Popen[bytes]] = set()
    process_lock = threading.Lock()
    cancelled = threading.Event()
    executor = ThreadPoolExecutor(max_workers=workers)
    futures: list[Future[int]] = []
    failed = False
    try:
        futures = [executor.submit(
            _execute_task, task, output, processes, process_lock, cancelled
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
