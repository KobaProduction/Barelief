"""Cross-build environment checks without installation or compilation."""
import importlib.util
import os
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("runner", Path(__file__).resolve().parents[1] / "scripts/build-esp32-sdk.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class EnvironmentTests(unittest.TestCase):
    def test_caller_paths_and_no_platform_dependency(self):
        original = {"PATH": "existing", "IDF_PATH": "old", "PYTHONPATH": "foreign"}
        env = runner.build_environment(original, Path("idf"), Path("compiler"), None)
        self.assertEqual(original["IDF_PATH"], "old")
        self.assertEqual(env["IDF_PATH"], str(Path("idf").resolve()))
        self.assertEqual(env["ESP32_TOOLCHAIN_DIR"], str(Path("compiler").resolve()))
        self.assertEqual(env["PATH"], str(Path("compiler/bin").resolve()) + os.pathsep + "existing")
        self.assertNotIn("PYTHONPATH", env)
        self.assertNotIn("IDF_TOOLS_PATH", env)

    def test_python_cache_only_when_requested(self):
        env = runner.build_environment({}, Path("idf"), Path("compiler"), Path("cache"))
        self.assertEqual(env["IDF_PYTHON_ENV_PATH"], str(Path("cache/python").resolve()))
        self.assertEqual(env["IDF_TOOLS_PATH"], str(Path("cache/idf-tools").resolve()))


if __name__ == "__main__":
    unittest.main()
