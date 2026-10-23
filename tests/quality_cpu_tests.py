import ctypes
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import quality_cpu as cpu


def record(identifier, logical, efficiency, *, group=0, flags=0, size=32):
    fields = struct.pack("<IIIHBBBBBB", size, 0, identifier, group, logical, logical, 0, 0, efficiency, flags)
    return fields + bytes(size - len(fields))


def windows_api(data, mask=0xffff):
    api = Mock()
    def information(buffer, length, returned, process, flags):
        ctypes.cast(returned, ctypes.POINTER(ctypes.c_uint32)).contents.value = len(data)
        if buffer is not None:
            ctypes.memmove(buffer, data, len(data))
        return bool(buffer is not None)
    def affinity(process, allowed, system):
        ctypes.cast(allowed, ctypes.POINTER(ctypes.c_size_t)).contents.value = mask
        return True
    api.GetSystemCpuSetInformation.side_effect = information
    api.GetProcessAffinityMask.side_effect = affinity
    return api


class WindowsTopologyTests(unittest.TestCase):
    def test_variable_records_keep_parked_but_exclude_reserved_cpus(self):
        data = record(1, 0, 1, flags=1, size=40) + struct.pack("<II", 8, 77)
        data += record(2, 1, 1, flags=2) + record(3, 2, 1, flags=6)
        self.assertEqual([row[0] for row in cpu.parse_windows_cpu_sets(data)], [1, 3])

    def test_truncated_and_zero_size_records_are_rejected(self):
        for data in [b"x", struct.pack("<II", 0, 0), record(1, 0, 1)[:-1], struct.pack("<II", 8, 0)]:
            with self.subTest(data=data), self.assertRaises(ValueError):
                cpu.parse_windows_cpu_sets(data)

    def test_highest_class_keeps_smt_threads_and_honors_process_affinity(self):
        data = record(100, 0, 1) + record(101, 1, 1) + record(102, 2, 0) + record(103, 3, 1)
        with patch.object(cpu, "_windows_api", return_value=windows_api(data, mask=7)):
            budget = cpu._windows_budget()
        self.assertEqual((budget.workers, budget.cpus, budget.windows_mask), (2, (100, 101), 3))

    def test_multiple_processor_groups_use_cpu_sets_without_wrong_group_mask(self):
        data = record(100, 0, 1) + record(101, 0, 1, group=1) + record(102, 1, 0)
        with patch.object(cpu, "_windows_api", return_value=windows_api(data)):
            budget = cpu._windows_budget()
        self.assertEqual((budget.cpus, budget.windows_mask), ((100, 101), 0))

    def test_homogeneous_cpu_does_not_claim_performance_core_detection(self):
        with patch.object(cpu, "_windows_api", return_value=windows_api(record(100, 0, 0))):
            self.assertIsNone(cpu._windows_budget())

    def test_child_receives_cpu_sets_and_hard_mask(self):
        api, process = Mock(), Mock(pid=123)
        process.poll.return_value = None
        with patch.object(cpu, "_windows_api", return_value=api):
            cpu._apply_windows(cpu.CpuBudget(2, "performance-cores", (100, 101), 3), process)
        self.assertEqual(tuple(api.SetProcessDefaultCpuSets.call_args.args[1]), (100, 101))
        self.assertEqual(api.SetProcessAffinityMask.call_args.args[1], 3)
        api.CloseHandle.assert_called_once_with(api.OpenProcess.return_value)


class PortableTopologyTests(unittest.TestCase):
    def test_linux_performance_pmu_intersects_allowed_cpus(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "cpu_core").mkdir()
            (root / "cpu_core/cpus").write_text("0-3,8\n")
            with patch.object(cpu.os, "sched_getaffinity", return_value={0, 2, 4}, create=True):
                budget = cpu._linux_budget(root)
            self.assertEqual((budget.workers, budget.cpus), (2, (0, 2)))

    def test_linux_capacity_selects_fast_class_and_homogeneous_falls_back(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, capacity in enumerate([1024, 512, 1024]):
                path = root / f"system/cpu/cpu{index}/cpu_capacity"
                path.parent.mkdir(parents=True)
                path.write_text(str(capacity))
            with patch.object(cpu.os, "sched_getaffinity", return_value={0, 1, 2}, create=True):
                self.assertEqual(cpu._linux_budget(root).cpus, (0, 2))
                (root / "system/cpu/cpu1/cpu_capacity").write_text("1024")
                self.assertIsNone(cpu._linux_budget(root))

    def test_unknown_topology_uses_75_percent_without_failure(self):
        with patch.object(cpu, "_windows_budget", side_effect=OSError("unavailable")), \
             patch.object(cpu, "_linux_budget", side_effect=OSError("unavailable")), \
             patch.object(cpu, "available_cpu_count", return_value=8):
            self.assertEqual(cpu.detect_budget(), cpu.CpuBudget(6, "fallback-75-percent"))
        self.assertEqual(cpu.detect_budget(1).workers, 1)
        self.assertEqual(cpu.detect_budget(22).workers, 16)

    def test_fallback_does_not_apply_affinity(self):
        process = Mock()
        cpu.CpuBudget(6, "fallback-75-percent").apply(process)
        self.assertEqual(process.mock_calls, [])


if __name__ == "__main__":
    unittest.main()
