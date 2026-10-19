"""Run the established clang-tidy function policy with bounded parallelism."""

import argparse
import os
from pathlib import Path
import shutil
import sys

from quality_execution import CommandTask, run_jobs
from quality_scope import partition_scope, select_scope


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", default=os.environ.get("CLANG_TIDY"))
    parser.add_argument("-p", "--build-path", default="cmake-build-release")
    parser.add_argument("--jobs", type=int)
    parser.add_argument("--full", action="store_true")
    parser.add_argument("--scope", choices=["auto", "full"], default="auto")
    parser.add_argument("--code-scope", choices=["production", "tests", "all"], default="production")
    parser.add_argument("--from-ref")
    parser.add_argument("--to-ref")
    parser.add_argument("--extra-arg", action="append", default=[])
    parser.add_argument("--extra-arg-before", action="append", default=[])
    parser.add_argument("files", nargs="*")
    return parser.parse_args()


def selected_files(args: argparse.Namespace, root: Path) -> tuple[str, ...]:
    category = args.code_scope
    if args.files:
        explicit = tuple(sorted({str(Path(name).resolve()) for name in args.files}))
        partition_scope(root, explicit, category, strict=True)
    full = args.full or args.scope == "full" or os.environ.get("QUALITY_FULL") == "1"
    if args.files and not full:
        files = tuple(sorted({str(Path(name).resolve()) for name in args.files}))
        print(f"clang-tidy scope: explicit focused files ({len(files)})", flush=True)
        return partition_scope(root, files, category, strict=True)
    scope = select_scope(
        root, Path(args.build_path), full=full,
        from_ref=args.from_ref, to_ref=args.to_ref,
    )
    print(f"clang-tidy scope: {scope.reason} ({len(scope.files)} files)", flush=True)
    return partition_scope(root, scope.files, category)


def run(args: argparse.Namespace) -> int:
    root = Path(__file__).resolve().parent.parent
    build_path = Path(args.build_path).resolve()
    if not (build_path / "compile_commands.json").is_file():
        raise ValueError("clang-tidy requires a configured compile_commands.json: " + str(build_path))
    executable = shutil.which(args.executable or "clang-tidy")
    if not executable:
        raise ValueError("Install clang-tidy or set CLANG_TIDY to its executable")
    files = selected_files(args, root)
    for name in files:
        if not Path(name).is_file():
            raise ValueError("clang-tidy source is missing: " + name)
    command = [executable, "-p=" + str(build_path),
               *("--extra-arg=" + value for value in args.extra_arg),
               *("--extra-arg-before=" + value for value in args.extra_arg_before)]
    return run_jobs(
        [CommandTask(name, [*command, name], cwd=str(root)) for name in files],
        jobs=args.jobs,
    )


def main() -> None:
    try:
        result = run(arguments())
    except (OSError, ValueError) as error:
        print("clang-tidy gate: " + str(error), file=sys.stderr)
        result = 2
    sys.exit(result)


if __name__ == "__main__":
    main()
