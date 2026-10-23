"""Run installed KDE Clazy on compilation-database C++ translation units.

No compiler replacement, fixes, source analysis or QML processing is performed
here. Set CLAZY_STANDALONE to pin the executable used by hooks and CI.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from collections.abc import Mapping, Sequence

from quality_execution import CommandTask, run_jobs
from quality_scope import partition_scope, select_scope


def arguments(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", default=os.environ.get("CLAZY_STANDALONE"))
    parser.add_argument("-p", "--build-path", default="cmake-build-release")
    parser.add_argument("--checks", default="level1")
    parser.add_argument("--extra-arg", action="append", default=[],
                        help="Pass a compiler argument to Clazy, for example --target for a MinGW database")
    parser.add_argument("--jobs", type=int, default=None,
                        help="Worker count override; default uses QUALITY_JOBS or half the logical CPUs")
    parser.add_argument("--full", action="store_true", help="Check all applicable current-tree Qt translation units")
    parser.add_argument("--scope", choices=("auto", "full"), default="auto",
                        help="Select Git-aware affected scope or force the complete current tree")
    parser.add_argument("--code-scope", choices=("production", "tests", "all"), default="production",
                        help="Choose production code, separately checked tests, or both")
    parser.add_argument("--from-ref", help="Git base ref; default uses PRE_COMMIT_FROM_REF")
    parser.add_argument("--to-ref", help="Git target ref; default uses PRE_COMMIT_TO_REF")
    parser.add_argument("files", nargs="*", help="Explicit owning translation units for focused manual checks")
    return parser.parse_args(argv)


def installed_candidates():
    found = shutil.which("clazy-standalone")
    if found:
        yield Path(found)
    creator = shutil.which("qtcreator")
    if creator:
        yield Path(creator).parent / "clang/bin/clazy-standalone.exe"
    if os.environ.get("QT_CREATOR_ROOT"):
        yield Path(os.environ["QT_CREATOR_ROOT"]) / "bin/clang/bin/clazy-standalone.exe"
    root = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "JetBrains"
    for candidate in sorted(root.glob("CLion*/bin/clang/win/x64/bin/clazy-standalone.exe"), reverse=True):
        if list(candidate.parent.parent.glob("lib/clang/*/include/stddef.h")):
            yield candidate


def executable_path(requested):
    configuration = Path(__file__).resolve().parent.parent / ".agent-workspace/clazy-executable.txt"
    if not requested and configuration.is_file():
        requested = configuration.read_text(encoding="utf-8-sig").strip()
        if not requested or not Path(requested).is_absolute():
            raise ValueError("Clazy local configuration must contain one absolute executable path")
    if requested:
        candidate = shutil.which(requested) or requested
        if Path(candidate).is_file():
            return str(Path(candidate).resolve())
        raise ValueError("Configured Clazy executable does not exist: " + requested)
    for candidate in installed_candidates():
        if candidate.is_file():
            return str(candidate.resolve())
    raise ValueError("Install clazy-standalone or set CLAZY_STANDALONE to its executable")


def database_file(entry):
    path = Path(entry["file"])
    if not path.is_absolute():
        path = Path(entry["directory"]) / path
    return os.path.normcase(str(path.resolve()))


def translation_units(build_path: str | Path, files: Sequence[str]) -> list[str]:
    database = Path(build_path) / "compile_commands.json"
    entries = json.loads(database.read_text(encoding="utf-8-sig"))
    if not isinstance(entries, list):
        raise ValueError("Compilation database must contain a list of compile commands")
    known = {database_file(entry) for entry in entries}
    result = {}
    for name in files:
        path = Path(name).resolve()
        if path.suffix.lower() not in {".cpp", ".cc", ".cxx", ".c++"}:
            raise ValueError("Pass owning C++ translation units, not headers or QML: " + name)
        if not path.is_file() or os.path.normcase(str(path)) not in known:
            raise ValueError("Translation unit absent from compile_commands.json; reconfigure the build: " + name)
        result.setdefault(os.path.normcase(str(path)), str(path))
    return list(result.values())


def enabled_checks(executable: str, checks: str,
                   environment: Mapping[str, str] | None = None) -> list[str]:
    completed = subprocess.run([executable, "--checks=" + checks, "--list-checks"],
                               capture_output=True, text=True, check=True, env=environment)
    if completed.stderr.strip():
        raise ValueError("Clazy check selection failed: " + completed.stderr.strip())
    names = re.findall(r"^\s+([a-z][a-z0-9-]+)\s*$", completed.stdout, re.MULTILINE)
    if "Enabled checks:" not in completed.stdout or not names:
        raise ValueError("Clazy did not report enabled checks")
    return names


def selected_files(args: argparse.Namespace) -> list[str]:
    root = Path(__file__).resolve().parents[1]
    full = args.full or args.scope == "full" or os.environ.get("QUALITY_FULL") == "1"
    if args.files:
        focused = partition_scope(root, args.files, args.code_scope, strict=True)
        if not full:
            return list(focused)
        # A broader request must not hide an invalid explicit category, missing
        # file or stale CDB entry. Full priority only broadens a valid request.
        translation_units(args.build_path, focused)
    selection = select_scope(root, Path(args.build_path).resolve(), full=full,
                             from_ref=args.from_ref, to_ref=args.to_ref)
    print("Clazy scope: " + selection.reason + "; code=" + args.code_scope, flush=True)
    files = partition_scope(root, selection.files, args.code_scope)
    tests = set(partition_scope(root, files, "tests")) if args.code_scope == "all" else set()
    result = []
    for name in files:
        path = Path(name).resolve()
        relative = path.relative_to(root).as_posix()
        eligible = args.code_scope == "tests" or str(path) in tests or relative.startswith(
            ("presentation/qt/", "adapters/desktop/", "apps/"))
        if path.suffix in {".cpp", ".cc", ".cxx"} and eligible:
            result.append(str(path))
    return result


def sanitized_environment() -> dict[str, str]:
    environment = os.environ.copy()
    for key in ("CLAZY_IGNORE_DIRS", "CLAZY_HEADER_FILTER", "CLAZY_CHECKS", "CLAZY_NO_WERROR"):
        environment.pop(key, None)
    return environment


def report_progress(message: str) -> None:
    print("Clazy " + message, flush=True)


def run(args: argparse.Namespace) -> int:
    selected = selected_files(args)
    if not selected:
        print("Clazy: no affected Qt translation units", flush=True)
        return 0
    files = translation_units(args.build_path, selected)
    executable = executable_path(args.executable)
    environment = sanitized_environment()
    checks = enabled_checks(executable, args.checks, environment)
    # Clazy's own severity promotion avoids promoting unrelated Clang warnings.
    environment["CLAZY_CHECKS_AS_ERRORS"] = ",".join(checks)
    command = [executable, "--checks=" + args.checks, "--only-qt",
               "-p", str(Path(args.build_path).resolve()),
               *("--extra-arg=" + value for value in args.extra_arg)]
    tasks = [CommandTask(label=name, command=[*command, name], env=environment) for name in files]
    return run_jobs(tasks, jobs=args.jobs,
                    progress=report_progress)


def main():
    try:
        return run(arguments())
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print("Clazy gate: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
