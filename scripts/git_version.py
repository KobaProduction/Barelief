"""Resolve build identity from Git tags and first-parent history, without mutating Git."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def git(source: Path, *arguments: str, binary: bool = False):
    result = subprocess.run(["git", "-C", str(source), *arguments], capture_output=True, check=True).stdout
    return result if binary else result.decode("utf-8").strip()


def working_identity(source: Path, revision: str) -> tuple[str, bool, str]:
    diff = git(source, "diff", "--no-ext-diff", "--no-renames", "--diff-filter=CDMRTUXB", "--binary", "HEAD", binary=True)
    added = git(source, "diff", "--no-ext-diff", "--no-renames", "--diff-filter=A", "--name-only", "-z", "HEAD", binary=True)
    untracked = git(source, "ls-files", "--others", "--exclude-standard", "-z", binary=True)
    new_files = (set(added.split(b"\0")) | set(untracked.split(b"\0"))) - {b""}
    dirty = bool(diff or new_files)
    digest = hashlib.sha256(diff)
    for name in sorted(new_files):
        path = source / name.decode("utf-8")
        content = str(path.readlink()).encode("utf-8") if path.is_symlink() else path.read_bytes()
        digest.update(name + b"\0" + hashlib.sha256(content).digest())
    build = f"g{revision}" + (f".dirty.{digest.hexdigest()[:32]}" if dirty else "")
    suffix = f"-dirty-{hashlib.sha256(build.encode('ascii')).hexdigest()[:12]}" if dirty else ""
    return build, dirty, suffix


def configure_inputs(source: Path, tag: str) -> tuple[list[str], list[str]]:
    files = git(source, "ls-files", "--cached", "--others", "--exclude-standard", "-z", binary=True)
    inputs = [(source / name.decode("utf-8")).as_posix() for name in files.split(b"\0")
              if name and (source / name.decode("utf-8")).exists()]
    names = ["HEAD", "index", "packed-refs", f"refs/tags/{tag}"]
    branch = subprocess.run(["git", "-C", str(source), "symbolic-ref", "-q", "HEAD"], capture_output=True)
    if branch.returncode == 0:
        names.append(branch.stdout.decode("utf-8").strip())
    for name in names:
        path = source / git(source, "rev-parse", "--git-path", name)
        if path.exists():
            inputs.append(path.as_posix())
    directories = [(source / git(source, "rev-parse", "--git-path", "packed-refs")).parent.as_posix(),
                   (source / git(source, "rev-parse", "--git-path", "refs/tags")).as_posix()]
    return inputs, directories


def resolve(source: Path) -> dict:
    source = source.resolve()
    if git(source, "rev-parse", "--is-shallow-repository") != "false":
        raise ValueError("Versioning requires complete Git history and tags (fetch-depth: 0)")
    tag = git(source, "describe", "--first-parent", "--tags", "--match", "v[0-9]*.[0-9]*.[0-9]*", "--abbrev=0")
    match = re.fullmatch(r"v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", tag)
    if not match:
        raise ValueError(f"Unsupported stable Git version tag: {tag}")
    revision = git(source, "rev-parse", "HEAD")
    distance = int(git(source, "rev-list", "--count", "--first-parent", f"{tag}..HEAD"))
    major, minor, patch = map(int, match.groups())
    version = f"{major}.{minor}.{patch + distance}"
    if max(major, minor, patch + distance) > 65535:
        raise ValueError("Git version exceeds the Windows VERSIONINFO component limit")
    build, dirty, suffix = working_identity(source, revision)
    inputs, directories = configure_inputs(source, tag)
    return {"identity": {"schemaVersion": 1, "version": version, "displayVersion": version,
                         "build": build, "commit": revision, "baseTag": tag, "distance": distance,
                         "dirty": dirty, "channel": "stable", "releaseTag": f"v{version}", "packageSuffix": suffix},
            "inputs": inputs, "gitDirectories": directories}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--require-clean", action="store_true")
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    try:
        result = resolve(args.source)
        identity = result["identity"]
        if args.require_clean and identity["dirty"]:
            raise ValueError("Release source must be clean")
        if args.github_output:
            with args.github_output.open("a", encoding="utf-8") as output:
                for key in ("version", "releaseTag", "commit", "build"):
                    output.write(f"{key}={identity[key]}\n")
        print(json.dumps(result, ensure_ascii=True))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Git version resolution failed: {error}. Fetch complete version tags; no manual fallback is used.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
