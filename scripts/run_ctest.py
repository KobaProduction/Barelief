"""Run the local-push CTest preset against a prepared Release build."""

from pathlib import Path
import shutil
import subprocess
import sys


def ctest_executable(root: Path) -> str:
    build = root / "cmake-build-release"
    cache = build / "CMakeCache.txt"
    if not cache.is_file() or not (build / "CTestTestfile.cmake").is_file():
        raise ValueError("Prepare the Release tests first: cmake --preset release; cmake --build --preset release")
    for line in cache.read_text(encoding="utf-8-sig").splitlines():
        if line.startswith("CMAKE_CTEST_COMMAND:INTERNAL="):
            candidate = Path(line.split("=", 1)[1])
            if candidate.is_file():
                return str(candidate)
    candidate = shutil.which("ctest")
    if candidate:
        return candidate
    raise ValueError("CTest was not found in the configured Release build or PATH")


def main(root: Path | None = None) -> int:
    root = root or Path(__file__).resolve().parent.parent
    try:
        executable = ctest_executable(root)
    except ValueError as error:
        print(f"CTest quick gate: {error}", file=sys.stderr)
        return 1
    print("CTest quick gate: local-push preset; integration/audit/native/long tests excluded", flush=True)
    return subprocess.run([executable, "--preset", "local-push", "--no-tests=error"], cwd=root).returncode


if __name__ == "__main__":
    sys.exit(main())
