"""Portable worker budget with performance-core affinity when topology is known."""

import ctypes
from dataclasses import dataclass
from functools import lru_cache
import os
from pathlib import Path
import struct
import sys


@dataclass(frozen=True)
class CpuBudget:
    workers: int
    mode: str
    cpus: tuple[int, ...] = ()
    windows_mask: int = 0

    def apply(self, process):
        if not self.cpus:
            return
        if os.name == "nt":
            _apply_windows(self, process)
        else:
            os.sched_setaffinity(process.pid, self.cpus)


@lru_cache(maxsize=1)
def _windows_api():
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    u32, handle = ctypes.c_uint32, ctypes.c_void_p
    signatures = {
        "GetCurrentProcess": (handle, []),
        "GetSystemCpuSetInformation": (ctypes.c_int, [handle, u32, ctypes.POINTER(u32), handle, u32]),
        "GetProcessAffinityMask": (ctypes.c_int, [handle, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]),
        "OpenProcess": (handle, [u32, ctypes.c_int, u32]),
        "CloseHandle": (ctypes.c_int, [handle]),
        "SetProcessDefaultCpuSets": (ctypes.c_int, [handle, ctypes.POINTER(u32), u32]),
        "SetProcessAffinityMask": (ctypes.c_int, [handle, ctypes.c_size_t]),
    }
    for name, (result, arguments) in signatures.items():
        function = getattr(api, name)
        function.restype, function.argtypes = result, arguments
    return api


def parse_windows_cpu_sets(data):
    """Read variable-size CPU Set records; skip future, unknown record types."""
    records, offset = [], 0
    while offset < len(data):
        if len(data) - offset < 8:
            raise ValueError("Truncated CPU Set header")
        size, kind = struct.unpack_from("<II", data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError("Invalid CPU Set size")
        if kind == 0:
            if size < 20:
                raise ValueError("Truncated CPU Set record")
            identifier, group, logical, core, _, _, efficiency, flags = struct.unpack_from("<IHBBBBBB", data, offset + 8)
            if not flags & 2 or flags & 4:  # Exclude CPUs reserved for another process, not parked CPUs.
                records.append((identifier, group, logical, core, efficiency))
        offset += size
    return records


def _windows_budget():
    api, size = _windows_api(), ctypes.c_uint32()
    current = api.GetCurrentProcess()
    api.GetSystemCpuSetInformation(None, 0, ctypes.byref(size), current, 0)
    if not size.value:
        return None
    buffer = ctypes.create_string_buffer(size.value)
    if not api.GetSystemCpuSetInformation(buffer, len(buffer), ctypes.byref(size), current, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    records = parse_windows_cpu_sets(buffer.raw[:size.value])
    classes = {row[4] for row in records}
    if len(classes) < 2:
        return None  # A single class does not establish a P/E distinction.
    groups = {row[1] for row in records}
    allowed, system = ctypes.c_size_t(), ctypes.c_size_t()
    if len(groups) == 1 and api.GetProcessAffinityMask(current, ctypes.byref(allowed), ctypes.byref(system)):
        records = [row for row in records if allowed.value & (1 << row[2])]
    performance = [row for row in records if row[4] == max(classes)]
    if not performance:
        return None
    mask = sum(1 << row[2] for row in performance) if len(groups) == 1 else 0
    return CpuBudget(len(performance), "performance-cores", tuple(row[0] for row in performance), mask)


def _apply_windows(budget, process):
    api = _windows_api()
    handle = api.OpenProcess(0x2200, False, process.pid)  # SET_INFORMATION | SET_LIMITED_INFORMATION
    if not handle:
        if process.poll() is not None:
            return
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        identifiers = (ctypes.c_uint32 * len(budget.cpus))(*budget.cpus)
        success = api.SetProcessDefaultCpuSets(handle, identifiers, len(identifiers))
        if success and budget.windows_mask:
            success = api.SetProcessAffinityMask(handle, budget.windows_mask)
        if not success and process.poll() is None:
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        api.CloseHandle(handle)


def parse_cpu_list(text):
    cpus = set()
    for part in text.strip().split(","):
        first, separator, last = part.partition("-")
        start, end = int(first), int(last) if separator else int(first)
        if start < 0 or end < start:
            raise ValueError("Invalid CPU range")
        cpus.update(range(start, end + 1))
    return cpus


def _linux_budget(root=Path("/sys/devices")):
    allowed = set(os.sched_getaffinity(0))
    performance_list = root / "cpu_core/cpus"
    if performance_list.is_file():
        performance = parse_cpu_list(performance_list.read_text()) & allowed
    else:
        capacities = {cpu: int((root / f"system/cpu/cpu{cpu}/cpu_capacity").read_text()) for cpu in allowed}
        if len(set(capacities.values())) < 2:
            return None
        performance = {cpu for cpu, capacity in capacities.items() if capacity == max(capacities.values())}
    if not performance:
        return None
    return CpuBudget(len(performance), "performance-cores", tuple(sorted(performance)))


def available_cpu_count():
    if hasattr(os, "sched_getaffinity"):
        try:
            return len(os.sched_getaffinity(0)) or 1
        except OSError:
            pass
    if os.name == "nt":
        try:
            api, allowed, system = _windows_api(), ctypes.c_size_t(), ctypes.c_size_t()
            if api.GetProcessAffinityMask(api.GetCurrentProcess(), ctypes.byref(allowed), ctypes.byref(system)):
                return allowed.value.bit_count() or 1
        except (AttributeError, OSError):
            pass
    return getattr(os, "process_cpu_count", os.cpu_count)() or 1


def detect_budget(cpu_count=None):
    if cpu_count is None:
        try:
            budget = _windows_budget() if os.name == "nt" else _linux_budget() if sys.platform.startswith("linux") else None
            if budget:
                return budget
        except (AttributeError, OSError, ValueError):
            pass
        cpu_count = available_cpu_count()
    return CpuBudget(max(1, cpu_count * 3 // 4), "fallback-75-percent")
