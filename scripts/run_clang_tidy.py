"""Run the established clang-tidy function policy with bounded parallelism."""

import argparse
import json
import os
from pathlib import Path
import shutil
import sys

from quality_execution import CommandTask, run_jobs
from quality_compile_context import exact_header_filter, contexts_for, embedded_target, include_roots, is_fragment, load_contexts, materialize
from quality_scope import partition_scope, select_scope


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", default=os.environ.get("CLANG_TIDY"))
    parser.add_argument("-p", "--build-path", default="cmake-build-release")
    parser.add_argument("--jobs", type=int)
    parser.add_argument("--context-build-path", action="append",
                        default=[path for path in os.environ.get("QUALITY_CONTEXT_BUILD_PATHS", "").split(os.pathsep) if path])
    parser.add_argument("--embedded-executable", default=os.environ.get("ESP_CLANG_TIDY"))
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


SCENARIO_DRIVERS = frozenset({
    "tests/application_startup_trace.cpp", "tests/block_ui_native_probe.cpp",
    "tests/fft_size_benchmark.cpp", "tests/graph_performance_compare.cpp",
    "tests/graph_visual_compare.cpp", "tests/numeric_loopback_probe.cpp",
    "tests/numeric_preview_cadence_probe.cpp", "tests/render_topology_probe.cpp",
    "tests/resampler_quality_benchmark.cpp", "tests/ui_audit_application.cpp",
    "tests/ui_audit_graphs.cpp", "tests/ui_audit_workspace.cpp", "tests/ui_transition_audit.cpp",
})


def test_policy(source: Path, root: Path) -> tuple[list[str], str]:
    """Keep scenario allowances out of builders, oracles and support sources."""
    if not source.is_relative_to(root / "tests"):
        return [], "production 20/15"
    scenario = source.name.endswith("_tests.cpp") or source.relative_to(root).as_posix() in SCENARIO_DRIVERS
    config = root / "tests/.clang-tidy" if scenario else root / ".clang-tidy"
    if not config.is_file():
        raise ValueError("clang-tidy policy configuration is missing: " + str(config))
    category = "test scenario 60/25" if scenario else "test support 20/15"
    return ["--config-file=" + str(config)], category


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
    contexts = load_contexts(root, [build_path, *[Path(path) for path in args.context_build_path]])
    inventory = select_scope(root, build_path, full=True).files
    owner_inventory = set(partition_scope(root, inventory, args.code_scope))
    test_sources = frozenset(Path(name) for name in partition_scope(root, inventory, "tests")) if args.code_scope == "production" else frozenset()
    owner_inventory.update(map(str, test_sources))
    owner_inventory.update(files)
    if args.code_scope in {"tests", "all"}:
        # Platform fixture headers can be included by production sources compiled
        # in a real test target. This adds owner candidates, never selected jobs.
        owner_inventory.update(str(context.source) for context in contexts
                               if any(path.is_relative_to(root / "tests") for path in include_roots(context)))
    contexts = tuple(context for context in contexts if str(context.source) in owner_inventory)
    tasks = []
    command = [executable,
               *("--extra-arg=" + value for value in args.extra_arg),
               *("--extra-arg-before=" + value for value in args.extra_arg_before)]
    for name in files:
        source = Path(name)
        policy_options, policy_category = test_policy(source, root)
        for context in contexts_for(source, contexts, root, test_sources=test_sources):
            target = embedded_target(context)
            tool = executable
            if target is not None:
                tool = shutil.which(args.embedded_executable or "")
                if not tool:
                    raise ValueError("Real embedded context requires ESP_CLANG_TIDY/--embedded-executable for " + target)
            fragment = is_fragment(source, root)
            profile = materialize(source, context, root / ".agent-workspace/quality/compile-contexts", fragment=fragment)
            analysis_main = str(context.source) if fragment else name
            header_options = ["--header-filter=" + exact_header_filter(source)] if fragment else []
            if fragment and source.is_relative_to(root / "tests"):
                # Header-filter is exact; this additional intersection removes the
                # scenario main's diagnostics from the strict support-header task.
                # Basename matching tolerates Clang's mixed Windows separators.
                header_options.append("--line-filter=" + json.dumps([{"name": source.name}]))
            tasks.append(CommandTask(name + " [owner " + str(context.source) + "] [policy " + policy_category + "]",
                                     [tool, *command[1:], *policy_options, *header_options,
                                      "-p=" + str(profile), analysis_main], cwd=str(root)))
    return run_jobs(tasks, jobs=args.jobs)


def main() -> None:
    try:
        result = run(arguments())
    except (OSError, ValueError) as error:
        print("clang-tidy gate: " + str(error), file=sys.stderr)
        result = 2
    sys.exit(result)


if __name__ == "__main__":
    main()
