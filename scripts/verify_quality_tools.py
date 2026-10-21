"""Keep changes to the gate's own selection and scheduling covered by regression."""

import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    sys.path.insert(0, str(root / "tests"))
    sys.path.insert(0, str(root / "scripts"))
    with tempfile.TemporaryDirectory(prefix="quality-regressions-") as directory:
        environment = {
            "QUALITY_FULL": "0", "QUALITY_JOBS": "auto",
            "PRE_COMMIT_FROM_REF": "", "PRE_COMMIT_TO_REF": "",
            "QUALITY_PROGRESS_FILE": str(Path(directory) / "progress.jsonl"),
        }
        with patch.dict(os.environ, environment):
            suite = unittest.defaultTestLoader.loadTestsFromNames([
                "quality_scope_tests", "quality_execution_tests", "quality_compile_context_tests",
                "run_clang_tidy_tests", "run_clazy_tests",
            ])
            if not suite.countTestCases():
                print("Quality-tool regressions were not discovered", file=sys.stderr)
                return 1
            result = unittest.TextTestRunner(verbosity=2).run(suite)
            if result.skipped:
                print("Required quality-tool regressions were skipped", file=sys.stderr)
                return 1
            return int(not result.wasSuccessful())


if __name__ == "__main__":
    sys.exit(main())
