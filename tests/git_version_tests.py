"""Exercise version identity against isolated real Git histories."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import git_version


class GitVersionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="barelief-version-")
        self.addCleanup(self.directory.cleanup)
        self.source = Path(self.directory.name) / "source"
        self.source.mkdir()
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.name", "Version Test")
        self.git("config", "user.email", "version-test@example.invalid")
        self.git("config", "core.autocrlf", "false")
        self.git("config", "core.hooksPath", str(self.source / ".git/no-hooks"))
        (self.source / "source.txt").write_text("initial\n", encoding="utf-8")
        (self.source / ".gitignore").write_text("build/\npackages/\n", encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "-qm", "initial")
        self.git("tag", "v1.2.3")

    def git(self, *arguments):
        return subprocess.check_output(["git", "-C", str(self.source), *arguments], stderr=subprocess.PIPE).decode().strip()

    def identity(self):
        return git_version.resolve(self.source)["identity"]

    def advance(self, text="next\n"):
        (self.source / "source.txt").write_text(text, encoding="utf-8")
        self.git("commit", "-qam", "advance")

    def test_clean_identity_repeats_and_ignored_outputs_do_not_advance(self):
        before = self.identity()
        self.assertEqual(before["version"], "1.2.3")
        self.assertEqual(before["build"], "g" + self.git("rev-parse", "HEAD"))
        output = self.source / "build"
        output.mkdir()
        (output / "binary").write_bytes(b"generated")
        self.assertEqual(before, self.identity())

    def test_commit_advances_patch_and_release_tag_preserves_identity(self):
        self.advance()
        before = self.identity()
        self.assertEqual(before["version"], "1.2.4")
        self.git("tag", before["releaseTag"])
        after = self.identity()
        for key in ("version", "displayVersion", "build", "commit", "releaseTag"):
            self.assertEqual(before[key], after[key])

    def test_ci_build_logs_and_packages_do_not_mark_sources_dirty(self):
        ignore = Path(__file__).resolve().parents[1] / ".gitignore"
        (self.source / ".gitignore").write_text(ignore.read_text(), encoding="utf-8")
        self.git("commit", "-qam", "ignore generated CI outputs")
        before = self.identity()
        for name in ("linux-configure.log", "linux-build.log", "linux-package.log", "linux-test.log"):
            (self.source / name).write_text("diagnostic output", encoding="utf-8")
        (self.source / "build").mkdir()
        (self.source / "build/generated.txt").write_text("generated", encoding="utf-8")
        self.assertEqual(before, self.identity())

    def test_dirty_bytes_staging_added_files_and_rename(self):
        clean = self.identity()
        (self.source / "source.txt").write_text("edited\n", encoding="utf-8")
        edited = self.identity()
        self.assertEqual(clean["version"], edited["version"])
        self.assertNotEqual(clean["build"], edited["build"])
        self.assertRegex(edited["packageSuffix"], r"^-dirty-[0-9a-f]{12}$")
        self.git("add", ".")
        self.assertEqual(edited, self.identity())
        (self.source / "new.cpp").write_text("new", encoding="utf-8")
        before = self.identity()
        self.git("add", ".")
        self.assertEqual(before, self.identity())
        (self.source / "source.txt").rename(self.source / "renamed.txt")
        renamed = self.identity()
        self.git("add", "-A")
        self.assertEqual(renamed, self.identity())
        (self.source / "new.cpp").write_text("changed", encoding="utf-8")
        self.assertNotEqual(renamed["build"], self.identity()["build"])

    def test_first_parent_ignores_feature_tags_and_counts_merge_once(self):
        self.git("checkout", "-qb", "feature")
        (self.source / "feature.txt").write_text("feature", encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "-qm", "feature one")
        self.git("commit", "--allow-empty", "-qm", "feature two")
        self.git("tag", "v9.0.0")
        self.git("checkout", "-q", "main")
        self.advance()
        self.git("merge", "--no-ff", "feature", "-m", "merge feature")
        self.assertEqual(self.identity()["version"], "1.2.5")

    def test_worktree_and_packed_refs(self):
        worktree = Path(self.directory.name) / "worktree"
        self.git("worktree", "add", "--detach", str(worktree), "HEAD")
        self.git("pack-refs", "--all")
        result = git_version.resolve(worktree)
        self.assertEqual(result["identity"], self.identity())
        self.assertTrue(any(path.endswith("HEAD") for path in result["inputs"]))

    def test_missing_and_unsupported_tags_fail(self):
        self.git("tag", "-d", "v1.2.3")
        with self.assertRaises(subprocess.CalledProcessError):
            self.identity()
        self.git("tag", "v1.2.3-beta.1")
        with self.assertRaises(ValueError):
            self.identity()

    def test_shallow_history_fails(self):
        clone = Path(self.directory.name) / "shallow"
        self.git("config", "--unset", "core.hooksPath")
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--no-local", str(self.source), str(clone)], check=True)
        with self.assertRaises(ValueError):
            git_version.resolve(clone)

    def test_windows_resource_overflow_fails(self):
        self.git("tag", "-d", "v1.2.3")
        self.git("tag", "v1.2.65535")
        self.advance()
        with self.assertRaises(ValueError):
            self.identity()


if __name__ == "__main__":
    unittest.main()
