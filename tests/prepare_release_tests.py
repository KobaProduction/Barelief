"""Check release admission using local packages and isolated Git repositories."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import git_version
import prepare_release


class PrepareReleaseTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="barelief-release-")
        self.addCleanup(self.directory.cleanup)
        self.source, self.packages = Path(self.directory.name) / "source", Path(self.directory.name) / "packages"
        self.source.mkdir()
        self.packages.mkdir()
        self.git("init", "-q", "-b", "main")
        for key, value in (("user.name", "Release Test"), ("user.email", "release-test@example.invalid"),
                           ("core.autocrlf", "false"), ("core.hooksPath", str(self.source / ".git/no-hooks"))):
            self.git("config", key, value)
        (self.source / "source.txt").write_text("source", encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "-qm", "initial")
        self.git("tag", "v1.2.3")
        self.identity = git_version.resolve(self.source)["identity"]
        for suffix, platform in ((".zip", "Windows-AMD64"), (".exe", "Windows-AMD64"), (".deb", "Linux-x86_64")):
            path = self.packages / f"Barelief-1.2.3-{platform}{suffix}"
            path.write_bytes(b"isolated package fixture")
            self.manifest(path).write_text(json.dumps(self.identity), encoding="utf-8")

    def git(self, *arguments):
        return subprocess.check_output(["git", "-C", str(self.source), *arguments], stderr=subprocess.PIPE).decode().strip()

    def manifest(self, path):
        return path.with_name(path.name + ".identity.json")

    def prepare(self, commit=None):
        return prepare_release.prepare(self.source, self.packages, commit or self.identity["commit"])

    def test_verified_packages_checksums_and_repeat_are_immutable(self):
        self.assertEqual(self.prepare(), self.identity)
        sums = (self.packages / "SHA256SUMS.txt").read_text()
        self.assertEqual(len(sums.splitlines()), 6)
        for line in sums.splitlines():
            value, name = line.split("  ", 1)
            self.assertEqual(value, hashlib.sha256((self.packages / name).read_bytes()).hexdigest())
        self.assertIn(self.identity["commit"], (self.packages / "release-notes.txt").read_text())
        self.prepare()
        self.assertEqual((self.packages / "SHA256SUMS.txt").read_text(), sums)

    def test_next_commit_prepares_a_release_without_creating_its_tag(self):
        self.git("commit", "--allow-empty", "-qm", "next release")
        self.identity = git_version.resolve(self.source)["identity"]
        for archive in list(self.packages.iterdir()):
            if archive.name.endswith(".identity.json"):
                continue
            self.manifest(archive).unlink()
            renamed = archive.with_name(archive.name.replace("1.2.3", "1.2.4", 1))
            archive.rename(renamed)
            self.manifest(renamed).write_text(json.dumps(self.identity), encoding="utf-8")
        self.assertEqual(self.prepare()["releaseTag"], "v1.2.4")
        self.assertNotIn("refs/tags/v1.2.4", self.git("show-ref", "--tags"))

    def test_dirty_source_and_another_ci_commit_are_rejected(self):
        with self.assertRaises(ValueError):
            self.prepare("0" * 40)
        (self.source / "source.txt").write_text("edited", encoding="utf-8")
        with self.assertRaises(ValueError):
            self.prepare()

    def test_mixed_dirty_future_and_mismatched_package_identities_are_rejected(self):
        path = next(self.packages.glob("*.zip"))
        for key, value in (("dirty", True), ("schemaVersion", 2), ("version", "9.0.0"),
                           ("build", "another-build"), ("commit", "0" * 40)):
            with self.subTest(key=key):
                self.manifest(path).write_text(json.dumps(dict(self.identity, **{key: value})), encoding="utf-8")
                with self.assertRaises(ValueError):
                    self.prepare()

    def test_missing_platform_and_sidecar_are_rejected(self):
        path = next(self.packages.glob("*.exe"))
        path.unlink()
        with self.assertRaises(ValueError):
            self.prepare()
        path.write_bytes(b"restored")
        self.manifest(path).unlink()
        with self.assertRaises(ValueError):
            self.prepare()

    def test_existing_release_tag_cannot_point_to_another_commit(self):
        self.git("checkout", "-qb", "feature")
        self.git("commit", "--allow-empty", "-qm", "feature")
        self.git("tag", "v1.2.4")
        self.git("checkout", "-q", "main")
        self.git("commit", "--allow-empty", "-qm", "main advance")
        current = self.git("rev-parse", "HEAD")
        with self.assertRaisesRegex(ValueError, "another commit"):
            self.prepare(current)


if __name__ == "__main__":
    unittest.main()
