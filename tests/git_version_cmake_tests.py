"""Verify automatic version refresh through the actual CMake/Ninja dependency graph."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CMAKE, NINJA = sys.argv[1:3]
del sys.argv[1:3]


class ConfigureVersionTests(unittest.TestCase):
    def test_build_refreshes_commits_dirty_bytes_new_files_and_tags(self):
        with tempfile.TemporaryDirectory(prefix="barelief version cmake ") as directory:
            source, build = Path(directory) / "source", Path(directory) / "build"
            (source / "scripts").mkdir(parents=True)
            (source / "cmake").mkdir()
            shutil.copy(ROOT / "scripts/git_version.py", source / "scripts")
            shutil.copy(ROOT / "cmake/GitVersion.cmake", source / "cmake")
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.30)\ninclude(cmake/GitVersion.cmake)\n'
                'project(VersionProbe VERSION ${BARELIEF_VERSION} LANGUAGES NONE)\nadd_custom_target(probe ALL)\n')
            self.command(source, "git", "init", "-q")
            for key, value in (("user.name", "Version Test"), ("user.email", "version-test@example.invalid"),
                               ("core.autocrlf", "false"), ("core.hooksPath", str(source / ".git/no-hooks"))):
                self.command(source, "git", "config", key, value)
            self.command(source, "git", "add", ".")
            self.command(source, "git", "commit", "-qm", "initial")
            self.command(source, "git", "tag", "v1.2.3")
            self.command(source, CMAKE, "-S", str(source), "-B", str(build), "-G", "Ninja",
                         f"-DCMAKE_MAKE_PROGRAM={NINJA}", f"-DPython3_EXECUTABLE={sys.executable}")
            initial = self.rebuild(source, build)
            self.assertEqual(initial["version"], "1.2.3")
            self.assertEqual(initial, self.rebuild(source, build))
            (source / "new.cpp").write_text("new", encoding="utf-8")
            added = self.rebuild(source, build)
            self.assertTrue(added["dirty"])
            (source / "new.cpp").write_text("changed", encoding="utf-8")
            edited = self.rebuild(source, build)
            self.assertNotEqual(added["build"], edited["build"])
            self.command(source, "git", "add", ".")
            self.assertEqual(edited, self.rebuild(source, build))
            self.command(source, "git", "commit", "-qm", "next")
            advanced = self.rebuild(source, build)
            self.assertEqual(advanced["version"], "1.2.4")
            self.assertFalse(advanced["dirty"])
            self.command(source, "git", "tag", "v2.0.0")
            self.assertEqual(self.rebuild(source, build)["version"], "2.0.0")
            self.command(source, "git", "pack-refs", "--all")
            self.assertEqual(self.rebuild(source, build)["version"], "2.0.0")

    def command(self, cwd, *arguments):
        result = subprocess.run(arguments, cwd=cwd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def rebuild(self, source, build):
        self.command(source, CMAKE, "--build", str(build))
        return json.loads((build / "build-identity.json").read_text())


if __name__ == "__main__":
    unittest.main()
