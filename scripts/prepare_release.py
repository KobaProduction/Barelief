"""Validate immutable package identities and prepare GitHub release metadata."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import git_version


def checksum(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def prepare(source: Path, packages: Path, expected_commit: str) -> dict:
    identity = git_version.resolve(source)["identity"]
    if identity["dirty"] or identity["commit"] != expected_commit:
        raise ValueError("Release requires clean source at the CI commit")
    tag = identity["releaseTag"]
    existing = subprocess.run(["git", "-C", str(source), "rev-parse", "--verify", "--quiet", f"refs/tags/{tag}^{{commit}}"],
                              capture_output=True, text=True)
    if existing.returncode == 0 and existing.stdout.strip() != expected_commit:
        raise ValueError("Release tag already identifies another commit")
    if existing.returncode not in (0, 1):
        raise ValueError("Could not verify the release tag")
    files = sorted(path for path in packages.iterdir() if path.is_file() and path.name not in ("SHA256SUMS.txt", "release-notes.txt"))
    archives = [path for path in files if path.suffix in (".zip", ".exe", ".deb", ".tgz") or path.name.endswith(".tar.gz")]
    if not {".zip", ".exe", ".deb"}.issubset({path.suffix for path in archives}):
        raise ValueError("Release is missing Windows ZIP/installer or Linux DEB packages")
    expected_names = {path.name for path in archives} | {path.name + ".identity.json" for path in archives}
    if {path.name for path in files} != expected_names:
        raise ValueError("Release packages require exactly one identity sidecar per archive")
    for archive in archives:
        if not archive.name.startswith(f"Barelief-{identity['version']}-"):
            raise ValueError(f"Package filename has another version: {archive.name}")
        manifest = json.loads((packages / (archive.name + ".identity.json")).read_text(encoding="utf-8-sig"))
        if not isinstance(manifest, dict) or type(manifest.get("schemaVersion")) is not int or manifest["schemaVersion"] != 1:
            raise ValueError(f"Unsupported package identity: {archive.name}")
        if manifest.get("dirty") is not False:
            raise ValueError(f"Dirty package cannot be published: {archive.name}")
        for key in ("version", "build", "commit", "releaseTag", "channel"):
            if manifest.get(key) != identity[key]:
                raise ValueError(f"Package identity mismatch ({key}): {archive.name}")
    (packages / "SHA256SUMS.txt").write_text("".join(f"{checksum(path)}  {path.name}\n" for path in files), encoding="utf-8")
    notes = (f"Barelief {identity['version']}\n\nUnsigned build.\n\n"
             f"Source commit: {expected_commit}\nBuild identity: {identity['build']}\n")
    (packages / "release-notes.txt").write_text(notes, encoding="utf-8")
    return identity


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--packages", type=Path, required=True)
    parser.add_argument("--expected-commit", required=True)
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    try:
        identity = prepare(args.source, args.packages, args.expected_commit)
        if args.github_output:
            with args.github_output.open("a", encoding="utf-8") as output:
                output.write(f"version={identity['version']}\nreleaseTag={identity['releaseTag']}\n")
        print(json.dumps(identity))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Release validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
