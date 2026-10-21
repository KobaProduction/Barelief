"""Preserve real compilation contexts instead of borrowing clang's nearest file."""

from dataclasses import dataclass
import ctypes
import hashlib
from functools import lru_cache
import json
import os
from pathlib import Path
import re
import shlex


@dataclass(frozen=True)
class CompileContext:
    source: Path
    directory: Path
    arguments: tuple[str, ...]
    database: Path
    response_files: tuple[tuple[str, str], ...] = ()


def command_arguments(entry: dict) -> tuple[str, ...]:
    if isinstance(entry.get("arguments"), list):
        return tuple(entry["arguments"])
    command = entry.get("command")
    if not isinstance(command, str):
        raise ValueError("Compilation entry has neither arguments nor command")
    return split_command(command)


def split_command(command: str) -> tuple[str, ...]:
    if os.name != "nt":
        return tuple(shlex.split(command))
    count = ctypes.c_int()
    shell = ctypes.WinDLL("shell32", use_last_error=True)
    shell.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
    values = shell.CommandLineToArgvW(command, ctypes.byref(count))
    if not values:
        raise ValueError("Cannot parse Windows compilation command")
    try:
        return tuple(values[index] for index in range(count.value))
    finally:
        kernel = ctypes.WinDLL("kernel32")
        kernel.LocalFree.argtypes = [ctypes.c_void_p]
        kernel.LocalFree(ctypes.cast(values, ctypes.c_void_p))


def split_gnu_response(contents: str) -> tuple[str, ...]:
    """Tokenize the configured GNU drivers' rsp files, separately from Windows argv."""
    if "\0" in contents:
        raise ValueError("NUL in compilation response file")
    if os.name == "nt":
        contents = contents.replace("\r\n", "\n")
    result, token = [], []
    quote = None
    started = False
    index = 0
    while index < len(contents):
        value = contents[index]
        following = contents[index + 1] if index + 1 < len(contents) else ""
        if value == "\\" and quote != "'" and (quote != '"' or following in '$`"\\\n'):
            index += 1
            if index == len(contents):
                raise ValueError("Incomplete escape in compilation response file")
            if contents[index] != "\n":
                token.append(contents[index])
            started = True
        elif quote:
            if value == quote:
                quote = None
            else:
                token.append(value)
        elif value in "\"'":
            quote, started = value, True
        elif value in " \t\r\n\v\f":
            if started:
                result.append("".join(token))
                token, started = [], False
        else:
            token.append(value)
            started = True
        index += 1
    if quote:
        raise ValueError("Unterminated quote in compilation response file")
    if started:
        result.append("".join(token))
    return tuple(result)


@lru_cache(maxsize=None)
def _response_contents(path: Path) -> tuple[str, str]:
    contents = path.read_bytes()
    return contents.decode("utf-8-sig"), hashlib.sha256(contents).hexdigest()


def expand_responses(arguments: tuple[str, ...], directory: Path, stack: frozenset[Path] = frozenset()
                     ) -> tuple[tuple[str, ...], tuple[tuple[str, str], ...]]:
    expanded = []
    provenance = []
    for argument in arguments:
        if not argument.startswith("@"):
            expanded.append(argument)
            continue
        path = (directory / argument[1:]).resolve()
        if path in stack:
            raise ValueError("Compilation response-file cycle: " + str(path))
        if not path.is_file():
            raise ValueError("Configured compilation response file is missing: " + str(path))
        contents, digest = _response_contents(path)
        values = split_gnu_response(contents)
        nested, nested_provenance = expand_responses(values, directory, stack | {path})
        expanded.extend(nested)
        provenance.extend([(str(path), digest), *nested_provenance])
    return tuple(expanded), tuple(provenance)


def load_contexts(root: Path, build_paths: list[Path]) -> tuple[CompileContext, ...]:
    result = []
    for build_path in build_paths:
        database = (root / build_path / "compile_commands.json").resolve()
        if not database.is_file():
            raise ValueError("Configured compilation profile is missing: " + str(database))
        entries = json.loads(database.read_text(encoding="utf-8"))
        if not isinstance(entries, list) or not entries:
            raise ValueError("Configured compilation profile is empty: " + str(database))
        for entry in entries:
            directory = Path(entry["directory"]).resolve()
            source = (directory / entry["file"]).resolve()
            if source.is_relative_to(root) and source.is_file():
                arguments, response_files = expand_responses(command_arguments(entry), directory)
                result.append(CompileContext(source, directory, arguments, database, response_files))
    return tuple(result)


@lru_cache(maxsize=None)
def include_roots(context: CompileContext) -> tuple[Path, ...]:
    roots = []
    arguments = iter(context.arguments)
    for value in arguments:
        if value in ("-I", "-isystem", "-iquote"):
            roots.append((context.directory / next(arguments)).resolve())
        elif value.startswith("-I"):
            roots.append((context.directory / value[2:]).resolve())
        elif value.startswith("-isystem"):
            roots.append((context.directory / value[len("-isystem"):]).resolve())
    return tuple(roots)


@lru_cache(maxsize=None)
def include_sources(context: CompileContext) -> tuple[Path, ...]:
    sources = [context.source]
    values = iter(context.arguments)
    roots = include_roots(context)
    for value in values:
        if value in {"-include", "-imacros"}:
            name = next(values)
            candidates = [context.directory / name, *[root / name for root in roots]]
            target = next((candidate.resolve() for candidate in candidates if candidate.is_file()), None)
            if target is None:
                raise ValueError("Configured forced include is missing: " + name)
            sources.append(target)
    return tuple(sources)


_INCLUDES = re.compile(r'^\s*#\s*include\s*([<"])([^>"\r\n]+)[>"]', re.MULTILINE)


@lru_cache(maxsize=None)
def _literal_includes(source: Path) -> tuple[tuple[str, str], ...]:
    text = source.read_text(encoding="utf-8-sig", errors="replace")
    text = re.sub(r"/\*.*?\*/|//[^\r\n]*", "", text, flags=re.DOTALL)
    return tuple(_INCLUDES.findall(text))


@lru_cache(maxsize=None)
def _resolve_include(name: str, parent: Path | None, roots: tuple[Path, ...]) -> Path | None:
    candidates = ([parent / name] if parent is not None else []) + [p / name for p in roots]
    return next((p.resolve() for p in candidates if p.is_file()), None)


@lru_cache(maxsize=None)
def _resolved_includes(source: Path, roots: tuple[Path, ...], root: Path) -> tuple[Path, ...]:
    targets = []
    for delimiter, name in _literal_includes(source):
        target = _resolve_include(name, source.parent if delimiter == '"' else None, roots)
        if target is not None and target.is_relative_to(root):
            targets.append(target)
    return tuple(targets)


@lru_cache(maxsize=None)
def _dependencies(sources: tuple[Path, ...], roots: tuple[Path, ...], root: Path) -> frozenset[Path]:
    pending = list(sources)
    seen = set()
    while pending:
        source = pending.pop()
        if source in seen:
            continue
        seen.add(source)
        pending.extend(_resolved_includes(source, roots, root))
    return frozenset(seen)


def owns_header(context: CompileContext, header: Path, root: Path) -> bool:
    roots = tuple(path for path in include_roots(context) if path.is_relative_to(root))
    return header in _dependencies(include_sources(context), roots, root)


FRAGMENT_HEADERS = frozenset({
    "tests/numeric_dsp_stage_oracle.h",
    "tests/aligned_numerical_acceptance.h",
    "examples/esp32_fft_benchmark/main/correctness.h",
    "examples/esp32_fft_benchmark/main/memory_checks.h",
    "examples/esp32_resampler_benchmark/main/legacy_double_fir.h",
})


def is_fragment(source: Path, root: Path) -> bool:
    return source.is_relative_to(root) and source.relative_to(root).as_posix() in FRAGMENT_HEADERS


@lru_cache(maxsize=None)
def semantic_arguments(context: CompileContext) -> tuple[str, ...]:
    """Normalize emission-only arguments, preserving semantic order and defines."""
    result = []
    values = iter(context.arguments)
    for value in values:
        if value.startswith("@"):
            raise ValueError("Response-file compilation profile requires expanded arguments")
        if value in {"-o", "-MF", "-MT", "-MQ", "-MJ"}:
            next(values, None)
        elif value in {"-c", "-MD", "-MMD", "-MP"}:
            continue
        elif value in {"-I", "-isystem", "-iquote", "-include", "-imacros"}:
            result.extend([value, str((context.directory / next(values)).resolve())])
        elif value.startswith("-I"):
            result.append("-I" + str((context.directory / value[2:]).resolve()))
        elif value.startswith("-isystem"):
            result.append("-isystem" + str((context.directory / value[len("-isystem"):]).resolve()))
        elif not value.startswith("-") and (context.directory / value).resolve() == context.source:
            continue
        else:
            result.append(value)
    return tuple(result)


def contexts_for(source: Path, contexts: tuple[CompileContext, ...], root: Path, *,
                 test_sources: frozenset[Path] = frozenset()) -> tuple[CompileContext, ...]:
    if not contexts:
        raise ValueError("No eligible compilation profiles are configured")
    primary = contexts[0].database
    tiers = ([context for context in contexts if context.database == primary and context.source not in test_sources],
             [context for context in contexts if context.database != primary and context.source not in test_sources],
             [context for context in contexts if context.database == primary and context.source in test_sources])
    for tier in tiers:
        exact = [context for context in tier if context.source == source]
        if not exact and source.suffix.lower() in {".cpp", ".cc", ".cxx", ".c++"}:
            continue
        owner_suffixes = {".cpp", ".cc", ".cxx", ".c++"}
        if source.is_relative_to(root / "tests"):
            owner_suffixes.add(".c")
        candidates = exact or [context for context in tier
                               if context.source.suffix.lower() in owner_suffixes
                               and owns_header(context, source, root)]
        if not candidates:
            continue
        # Keep all semantic configurations in the selected primary/fallback tier.
        unique = {}
        for context in candidates:
            key = (context.source if is_fragment(source, root) else None, semantic_arguments(context))
            unique[key] = context
        return tuple(unique.values())
    raise ValueError("No proven compilation owner/profile for " + str(source))


def _driver_option_values(context: CompileContext, names: tuple[str, ...]) -> tuple[str, ...]:
    values = []
    for index, argument in enumerate(context.arguments[1:], 1):
        for name in names:
            if argument == name:
                if index + 1 == len(context.arguments) or context.arguments[index + 1].startswith("-"):
                    raise ValueError("Missing embedded driver option value: " + name)
                values.append(context.arguments[index + 1])
            elif argument.startswith(name + "="):
                value = argument[len(name) + 1:]
                if not value:
                    raise ValueError("Missing embedded driver option value: " + name)
                values.append(value)
    return tuple(values)


def embedded_target(context: CompileContext) -> str | None:
    name = Path(context.arguments[0]).name.lower()
    match = re.match(r"(xtensa-(?:esp32s3|esp32s2|esp32)-elf)-g(?:cc|\+\+)(?:\.exe)?$", name)
    if match:
        return match.group(1)
    if name.startswith("xtensa-"):
        raise ValueError("ESP policy analysis requires a real IDF_TOOLCHAIN=clang build profile; "
                         "unsupported native compiler: " + name)
    triples = _driver_option_values(context, ("--target", "-target"))
    if not any(triple.startswith("xtensa") for triple in triples):
        return None
    if name not in {"clang", "clang++", "clang.exe", "clang++.exe"} or set(triples) != {"xtensa-esp-elf"}:
        raise ValueError("Unsupported embedded compiler/target command: " + str(context.source))
    cpus = _driver_option_values(context, ("-mcpu",))
    if len(set(cpus)) != 1 or cpus[0] not in {"esp32", "esp32s2", "esp32s3"}:
        raise ValueError("Official Xtensa Clang context requires its configured CPU: " + str(context.source))
    return "xtensa-" + cpus[0] + "-elf"


def frontend_arguments(context: CompileContext) -> list[str]:
    target = embedded_target(context)
    name = Path(context.arguments[0]).name.lower()
    if target is not None and name not in {"clang", "clang++", "clang.exe", "clang++.exe"}:
        raise ValueError("ESP policy analysis requires a real IDF_TOOLCHAIN=clang build profile; "
                         "keep native GCC object compilation separate: " + str(context.database))
    return list(context.arguments)


def exact_header_filter(source: Path) -> str:
    # Clang's FileEntryName may retain either Windows separator spelling.
    return "^" + re.escape(source.as_posix()).replace("/", r"[/\\]") + "$"


def materialize(source: Path, context: CompileContext, output: Path, *, fragment: bool = False) -> Path:
    arguments = frontend_arguments(context)
    analysis_main = context.source if fragment else source
    if source != context.source and not fragment:
        arguments = [str(source) if not arg.startswith("-") and (context.directory / arg).resolve() == context.source else arg
                     for arg in arguments]
        # Header main files use the compiler's header language, before the input.
        index = next(index for index, value in enumerate(arguments)
                     if not value.startswith("-") and (context.directory / value).resolve() == source)
        arguments[index:index] = ["-x", "c++-header"]
    header_filter = exact_header_filter(source) if fragment else None
    identity = hashlib.sha256((str(source) + repr(context) + str(fragment) + repr(arguments) +
                               str(analysis_main) + repr(header_filter)).encode()).hexdigest()
    directory = output / identity
    directory.mkdir(parents=True, exist_ok=True)
    entry = {"directory": str(context.directory), "file": str(analysis_main), "arguments": arguments}
    (directory / "compile_commands.json").write_text(json.dumps([entry]), encoding="utf-8")
    (directory / "provenance.json").write_text(json.dumps({
        "source": str(source), "owner": str(context.source), "analysis_main": str(analysis_main),
        "header_filter": header_filter,
        "database": str(context.database),
        "database_sha256": hashlib.sha256(context.database.read_bytes()).hexdigest(),
        "response_files": dict(context.response_files),
    }), encoding="utf-8")
    return directory
