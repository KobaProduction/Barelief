"""Select a conservative Git-aware C++ check scope without caching verdicts."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from collections.abc import Sequence
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

# Preserve identify's C++ main-file eligibility used by the old pre-commit
# types:[c++] hook, including inline/templates, modules, Arduino and ObjC++.
# identify normalizes extension case: .C is tagged C, whereas .H is C/C++.
CPP_SUFFIXES = frozenset({".cpp", ".cc", ".cxx", ".c++", ".c++m", ".ccm",
                          ".cppm", ".cxxm", ".ino", ".ixx", ".mm"})
HEADER_SUFFIXES = frozenset({".h", ".hpp", ".hh", ".hxx", ".inl", ".ipp", ".tpp"})
SOURCE_SUFFIXES = CPP_SUFFIXES | HEADER_SUFFIXES
# Registered repository test-code roots. Names of files/targets do not classify
# production code: shared SDK/adapters/examples remain in the production gate.
TEST_ROOTS = ("tests",)


@dataclass(frozen=True)
class ScopeSelection:
    full: bool
    files: tuple[str, ...]
    reason: str


class ScopeError(ValueError):
    """The affected set cannot be established safely."""


def partition_scope(root: Path, files: Sequence[str], code_scope: str = "production",
                    *, strict: bool = False) -> tuple[str, ...]:
    """Separate production and registered test code after Git scope selection.

    `all` preserves every selected main file, including explicit focused files
    outside the repository. Production/tests require repository-local paths.
    An explicit request uses strict=True to reject a mismatched category rather
    than silently reporting success after filtering every requested file away.
    """
    if code_scope not in {"production", "tests", "all"}:
        raise ScopeError("Unknown code scope: " + code_scope)
    root = root.resolve()
    selected = set()
    for name in files:
        path = (root / name).resolve()
        if code_scope == "all":
            selected.add(str(path))
            continue
        if not path.is_relative_to(root):
            raise ScopeError("Partitioned source is outside the repository: " + str(path))
        is_test = any(path.is_relative_to(root / test_root) for test_root in TEST_ROOTS)
        matches = is_test == (code_scope == "tests")
        if strict and not matches:
            raise ScopeError("Explicit source does not belong to " + code_scope + ": " + str(path))
        if matches:
            selected.add(str(path))
    return tuple(sorted(selected))


def _git(root: Path, *arguments: str) -> bytes:
    try:
        result = subprocess.run(["git", "-C", str(root), *arguments],
                                capture_output=True, timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ScopeError("Git unavailable") from error
    if result.returncode:
        raise ScopeError("Git range or worktree query failed")
    return result.stdout


def _paths(data: bytes) -> list[str]:
    return [os.fsdecode(value) for value in data.split(b"\0") if value]


def _canonical(root: Path, name: str) -> Path:
    path = (root / name).resolve()
    if not path.is_relative_to(root):
        raise ScopeError("Path escapes the repository")
    return path


def _exclusion(root: Path) -> re.Pattern[str]:
    configuration = root / ".pre-commit-config.yaml"
    if not configuration.is_file():
        return re.compile(r"(?!)")
    text = configuration.read_text(encoding="utf-8-sig")
    match = re.search(r"^exclude:\s*(.*?)\s*$", text, re.MULTILINE)
    if not match:
        return re.compile(r"(?!)")
    value = match.group(1)
    if not value:
        raise ScopeError("Empty hook exclusion is not a valid policy")
    if value.startswith("'") and value.endswith("'"):
        value = value[1:-1].replace("''", "'")
    elif value.startswith('"') and value.endswith('"'):
        value = json.loads(value)
    elif value.startswith(("|", ">")):
        raise ScopeError("Unsupported multiline hook exclusion; refusing to narrow scope")
    return re.compile(value)


def _inventory(root: Path) -> tuple[str, ...]:
    try:
        names = _paths(_git(root, "ls-files", "-z", "--cached", "--others", "--exclude-standard"))
        paths = [_canonical(root, name) for name in names]
    except ScopeError:
        excluded = {".git", ".agent-workspace", ".idea", "output", "node_modules", ".venv"}
        paths = []
        for directory, subdirs, filenames in os.walk(root):
            subdirs[:] = [name for name in subdirs if name not in excluded and not name.startswith("cmake-build-")]
            paths.extend(Path(directory) / name for name in filenames)
    exclusion = _exclusion(root)
    return tuple(sorted({str(path.resolve()) for path in paths
                         if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES
                         and not exclusion.search(path.relative_to(root).as_posix())}))


def _full(root: Path, reason: str) -> ScopeSelection:
    return ScopeSelection(True, _inventory(root), reason)


def _changed_names(data: bytes) -> tuple[set[str], bool]:
    values = _paths(data)
    names: set[str] = set()
    removed_cpp = False
    position = 0
    while position < len(values):
        status = values[position]
        count = 2 if status.startswith(("R", "C")) else 1
        if not status or status[0] not in "ACDMRTUXB" or position + count >= len(values):
            raise ScopeError("Malformed Git diff status")
        entries = values[position + 1:position + 1 + count]
        names.update(entries)
        if status[0] in "DRTUXB" and any(Path(name).suffix.lower() in SOURCE_SUFFIXES for name in entries):
            removed_cpp = True
        position += count + 1
    return names, removed_cpp


def _shared_change(name: str) -> bool:
    path = Path(name)
    return (path.name in {"CMakeLists.txt", "DEPS", "AGENTS.md", ".clang-tidy", ".clang-format",
                          ".pre-commit-config.yaml", "CMakePresets.json", "CMakeUserPresets.json"}
            or path.suffix.lower() in {".cmake", ".in"}
            or name.replace("\\", "/").startswith(("scripts/", ".github/"))
            or path.name.startswith(("requirements", "compile_commands"))
            or path.name in {".gitmodules", "pyproject.toml", "uv.lock"})


def _compile_commands(root: Path, build_path: Path) -> list[dict]:
    try:
        entries = json.loads((build_path / "compile_commands.json").read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as error:
        raise ScopeError("Compilation database unavailable") from error
    if not isinstance(entries, list) or not entries:
        raise ScopeError("Compilation database is empty or malformed")
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str) or not isinstance(entry.get("directory"), str):
            raise ScopeError("Malformed compilation command")
    return entries


def _command_arguments(entry: dict) -> list[str]:
    if isinstance(entry.get("arguments"), list) and all(isinstance(value, str) for value in entry["arguments"]):
        return entry["arguments"]
    command = entry.get("command")
    if not isinstance(command, str):
        raise ScopeError("Compilation arguments unavailable")
    # CMake emits quoted forward-slash paths on Windows. Unquoted backslashes
    # cannot be parsed with POSIX escaping without changing the include roots.
    if "\\" in command:
        raise ScopeError("Ambiguous compilation command quoting")
    try:
        return shlex.split(command)
    except ValueError as error:
        raise ScopeError("Malformed compilation command quoting") from error


def _include_roots(entry: dict) -> tuple[list[Path], list[str]]:
    directory = Path(entry["directory"])
    arguments = _command_arguments(entry)
    roots: list[Path] = []
    system_roots: list[Path] = []
    forced: list[str] = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument.startswith("@"):
            raise ScopeError("Response-file include dependencies are unknown")
        if ((argument.startswith(("-include", "-imacros")) and argument not in {"-include", "-imacros"})
                or argument.startswith(("-iquote", "-idirafter", "-iprefix", "-iwithprefix", "-iframework", "-ivfsoverlay", "-Wp,", "/FI", "-F", "-fmodule"))
                or argument == "-I-"):
            raise ScopeError("Unsupported compiler include-search option")
        if argument in {"-I", "-isystem", "-iquote", "/I", "-include", "-imacros"}:
            index += 1
            if index >= len(arguments):
                raise ScopeError("Incomplete include argument")
            if argument in {"-include", "-imacros"}:
                forced.append(arguments[index])
            else:
                destination = system_roots if argument == "-isystem" else roots
                destination.append((directory / arguments[index]).resolve())
        elif argument.startswith(("-I", "/I")) and len(argument) > 2:
            roots.append((directory / argument[2:]).resolve())
        elif argument.startswith(("-isystem", "-iquote")):
            prefix = "-isystem" if argument.startswith("-isystem") else "-iquote"
            system_roots.append((directory / argument[len(prefix):]).resolve())
        index += 1
    roots.extend(system_roots)
    resolved_forced = []
    for name in forced:
        candidates = [directory / name] + [path / name for path in roots]
        target = next((path.resolve() for path in candidates if path.is_file()), None)
        if target is None:
            raise ScopeError("Forced include cannot be resolved")
        resolved_forced.append(str(target))
    return roots, resolved_forced


_INCLUDE = re.compile(r"^\s*#\s*(include|include_next|import)\s+(.+)$", re.MULTILINE)
_LITERAL = re.compile(r'^([<"])([^>"\r\n]+)[>"]\s*$')


def _includes(path: Path) -> list[tuple[str, bool]]:
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (OSError, UnicodeError) as error:
        raise ScopeError("Include source unavailable") from error
    text = re.sub(r"\\\r?\n", "", text)
    text = re.sub(r"/\*.*?\*/", lambda match: "\n" * match.group().count("\n"), text, flags=re.DOTALL)
    text = re.sub(r"//[^\r\n]*", "", text)
    if re.search(r"^\s*(?:export\s+)?import\s", text, re.MULTILINE):
        raise ScopeError("Module import dependencies are unknown")
    includes = []
    for kind, spelling in _INCLUDE.findall(text):
        match = _LITERAL.fullmatch(spelling.strip())
        if kind != "include" or not match:
            raise ScopeError("Macro or include-next dependencies are unknown")
        includes.append((match.group(2), match.group(1) == '"'))
    return includes


def _dependencies(source: Path, roots: list[Path], forced: list[str]) -> set[Path]:
    visited: set[Path] = set()
    pending = [(source, [(name, True) for name in forced])]
    while pending:
        path, extra = pending.pop()
        if path in visited:
            continue
        visited.add(path)
        for name, quoted in _includes(path) + extra:
            candidates = ([path.parent / name] if quoted else []) + [directory / name for directory in roots]
            target = next((candidate.resolve() for candidate in candidates if candidate.is_file()), None)
            if target is None:
                raise ScopeError("Unresolved include dependency")
            pending.append((target, []))
    return visited


def _header_owners(root: Path, entries: list[dict], headers: set[Path], inventory: set[str]) -> set[str]:
    owners: set[str] = set()
    covered: set[Path] = set()
    for entry in entries:
        source = (Path(entry["directory"]) / entry["file"]).resolve()
        if str(source) not in inventory or source.suffix.lower() not in CPP_SUFFIXES:
            continue
        roots, forced = _include_roots(entry)
        affected = _dependencies(source, roots, forced) & headers
        if affected:
            owners.add(str(source))
            covered.update(affected)
    if covered != headers:
        raise ScopeError("Changed header has no proven owning translation unit")
    return owners


def select_scope(root: Path, build_path: Path, *, full: bool = False,
                 from_ref: str | None = None, to_ref: str | None = None) -> ScopeSelection:
    """Return absolute main files, preserving changed headers and CDB owners.

    Missing range/Git/CDB or uncertain dependencies always selects full scope.
    Full scope preserves non-CDB sources and headers for clang-tidy's fallback.
    Callers retain their tool-specific eligibility rules; no verdict is cached.
    """
    root = root.resolve()
    build_path = (root / build_path).resolve()
    if full or os.environ.get("QUALITY_FULL") == "1":
        return _full(root, "Explicit full check")
    from_ref = from_ref or os.environ.get("PRE_COMMIT_FROM_REF")
    to_ref = to_ref or os.environ.get("PRE_COMMIT_TO_REF")
    if not from_ref or not to_ref:
        return _full(root, "No explicit Git range; full check required")
    try:
        top = Path(os.fsdecode(_git(root, "rev-parse", "--show-toplevel")).strip()).resolve()
        if top != root:
            raise ScopeError("Scope root differs from the Git repository")
        base = _git(root, "rev-parse", "--verify", from_ref + "^{commit}").strip()
        target = _git(root, "rev-parse", "--verify", to_ref + "^{commit}").strip()
        head = _git(root, "rev-parse", "--verify", "HEAD^{commit}").strip()
        if not base or not target or target != head:
            raise ScopeError("Checkout does not match the requested push target")
        names, removed = _changed_names(_git(root, "diff", "--ignore-submodules=none", "--name-status", "-z", "--find-renames", from_ref, to_ref, "--"))
        staged, staged_removed = _changed_names(_git(root, "diff", "--ignore-submodules=none", "--cached", "--name-status", "-z", "--find-renames", "HEAD", "--"))
        local, local_removed = _changed_names(_git(root, "diff", "--ignore-submodules=none", "--name-status", "-z", "--find-renames", "--"))
        names.update(staged)
        names.update(local)
        names.update(_paths(_git(root, "ls-files", "--others", "--exclude-standard", "-z")))
        if removed or staged_removed or local_removed or any(_shared_change(name) for name in names):
            return _full(root, "Shared policy/toolchain change or C++ rename/delete")
        entries = _compile_commands(root, build_path)
        paths = {_canonical(root, name) for name in names}
        if any(path.is_dir() for path in paths):
            raise ScopeError("Submodule or directory dependency changed")
        if any(path.suffix.lower() in SOURCE_SUFFIXES and not path.is_file() for path in paths):
            raise ScopeError("Changed C++ source is missing from the checkout")
        inventory = set(_inventory(root))
        selected = {str(path) for path in paths if str(path) in inventory}
        headers = {path for path in paths if path.is_file() and path.suffix.lower() in HEADER_SUFFIXES}
        if headers:
            selected.update(_header_owners(root, entries, headers, inventory))
        return ScopeSelection(False, tuple(sorted(selected)), "Git range plus staged/unstaged/untracked changes")
    except (ScopeError, OSError, ValueError) as error:
        return _full(root, str(error))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("-p", "--build-path", type=Path, default=Path("cmake-build-release"))
    parser.add_argument("--full", action="store_true")
    parser.add_argument("--from-ref")
    parser.add_argument("--to-ref")
    arguments = parser.parse_args()
    scope = select_scope(arguments.root, arguments.build_path, full=arguments.full,
                         from_ref=arguments.from_ref, to_ref=arguments.to_ref)
    print(json.dumps({"full": scope.full, "files": scope.files, "reason": scope.reason}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
