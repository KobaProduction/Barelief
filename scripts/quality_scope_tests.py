"""Git scope regressions use isolated source trees and a deterministic Git adapter.

No branch or commit is created in the working repository or in the fixtures.
"""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quality_scope as scope


class ScopeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.build = self.root / "cmake-build-release"
        self.build.mkdir()
        self.tracked = []
        self.committed = b""
        self.worktree = b""
        self.staged = b""
        self.untracked = []
        self.missing_git = False
        self.target = b"target"
        self.head = b"target"
        self.add("src/a.cpp", '#include "common.h"\n')
        self.add("src/b.cpp", "int b;\n")
        self.add("include/common.h", "int common;\n")
        self.database()
        self.environment = patch.dict(os.environ, {"QUALITY_FULL": "", "PRE_COMMIT_FROM_REF": "", "PRE_COMMIT_TO_REF": ""})
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.git_patch = patch.object(scope, "_git", side_effect=self.git)
        self.git_patch.start()
        self.addCleanup(self.git_patch.stop)

    def add(self, name, content, tracked=True):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        (self.tracked if tracked else self.untracked).append(name)
        return path

    def database(self, entries=None):
        if entries is None:
            entries = [{"file": name, "directory": str(self.root),
                        "arguments": ["c++", "-I", str(self.root / "include"), "-c", name]}
                       for name in ("src/a.cpp", "src/b.cpp")]
        (self.build / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")

    def git(self, root, *args):
        if self.missing_git:
            raise scope.ScopeError("Git unavailable")
        if args == ("rev-parse", "--show-toplevel"):
            return os.fsencode(self.root)
        if args[:2] == ("rev-parse", "--verify"):
            return {"base^{commit}": b"base", "target^{commit}": self.target,
                    "HEAD^{commit}": self.head}[args[2]]
        if args[0] == "diff":
            return self.staged if "--cached" in args else self.committed if "base" in args else self.worktree
        if args[0] == "ls-files":
            names = self.tracked + self.untracked if "--cached" in args else self.untracked
            return b"\0".join(os.fsencode(name) for name in names) + b"\0"
        raise AssertionError(args)

    def select(self, **kwargs):
        return scope.select_scope(self.root, self.build, from_ref="base", to_ref="target", **kwargs)

    def assert_names(self, selection, expected):
        actual = [Path(name).relative_to(self.root).as_posix() for name in selection.files]
        self.assertEqual(actual, sorted(expected))

    def test_staged_and_unstaged_are_independent_unions(self):
        # A staged edit can be reversed in the worktree; both diffs still belong
        # to the gate scope rather than disappearing in a net diff against HEAD.
        self.staged = b"M\0src/a.cpp\0"
        self.worktree = b"M\0src/b.cpp\0"
        self.assert_names(self.select(), ["src/a.cpp", "src/b.cpp"])

    def test_missing_changed_cpp_falls_back(self):
        self.committed = b"M\0src/missing.cpp\0"
        self.assertTrue(self.select().full)

    def test_cpp_header_extensions_and_c_exclusion(self):
        for suffix in (".h", ".hpp", ".hh", ".hxx", ".cpp", ".cc", ".cxx", ".c"):
            self.add("extra/file" + suffix, "int extra;\n")
        result = self.select(full=True)
        self.assertNotIn(str(self.root / "extra/file.c"), result.files)
        self.assertEqual(len(result.files), 10)

    def test_unsupported_exclusion_fails_closed(self):
        self.add(".pre-commit-config.yaml", "exclude: |\n  ^vendor/\n")
        with self.assertRaises(scope.ScopeError):
            self.select(full=True)

    def test_precommit_identify_cpp_extensions_preserved(self):
        names = ["file" + suffix for suffix in (
            ".c++", ".c++m", ".cc", ".ccm", ".cpp", ".cppm", ".cxx", ".cxxm",
            ".h", ".hh", ".hpp", ".hxx", ".inl", ".ino", ".ipp", ".ixx", ".mm", ".tpp")]
        names.extend(["capital_header.H", "capital_cpp.CPP"])
        for name in names:
            self.add("extra/" + name, "int extension;\n")
        self.add("extra/capital_c.C", "int ordinary_c;\n")
        self.add("extra/file.c", "int ordinary_c;\n")
        result = self.select(full=True)
        expected = {str(self.root / "extra" / name) for name in names}
        self.assertTrue(expected.issubset(set(result.files)))
        self.assertNotIn(str(self.root / "extra/capital_c.C"), result.files)
        self.assertNotIn(str(self.root / "extra/file.c"), result.files)

    def test_inline_template_changes_add_owners(self):
        for suffix in (".ipp", ".tpp", ".inl"):
            with self.subTest(suffix=suffix):
                self.add("include/inline" + suffix, "int inline_value;\n")
                (self.root / "src/a.cpp").write_text('#include "inline' + suffix + '"\n')
                self.committed = b"M\0" + ("include/inline" + suffix).encode() + b"\0"
                self.assert_names(self.select(), ["include/inline" + suffix, "src/a.cpp"])

    def test_installed_identify_cpp_extension_parity(self):
        try:
            from identify import extensions
        except ImportError:
            self.skipTest("identify is installed in the project's quality environment")
        expected = {"." + suffix for suffix, tags in extensions.EXTENSIONS.items() if "c++" in tags}
        self.assertEqual(scope.SOURCE_SUFFIXES, expected)

    def test_partition_registered_test_root_includes_fixtures(self):
        self.add("tests/helper.hpp", "int fixture;\n")
        self.add("tests/support/loose.cpp", "int loose;\n")
        selected = scope.partition_scope(self.root, self.select(full=True).files, "tests")
        self.assert_names(scope.ScopeSelection(True, selected, "partition"), ["tests/helper.hpp", "tests/support/loose.cpp"])

    def test_partition_default_is_production(self):
        self.add("tests/probe.cpp", "int probe;\n")
        selected = scope.partition_scope(self.root, self.select(full=True).files)
        self.assert_names(scope.ScopeSelection(True, selected, "partition"), ["include/common.h", "src/a.cpp", "src/b.cpp"])

    def test_partition_does_not_use_filename_or_target_heuristics(self):
        names = ["src/device_tests.cpp", "libs/dsp_kernels/src/backend_esp_dsp.cpp",
                 "adapters/esp_idf/persistence/src/nvs_persistence.cpp", "examples/benchmark.cpp",
                 "libs/unregistered/tests/production_helper.h"]
        files = [str(self.add(name, "int production;\n")) for name in names]
        self.assertEqual(scope.partition_scope(self.root, files), tuple(sorted(files)))
        self.assertEqual(scope.partition_scope(self.root, files, "tests"), ())

    def test_partition_is_disjoint_and_union_preserves_full_scope(self):
        self.add("tests/fixture.inl", "int fixture;\n")
        files = self.select(full=True).files
        production = set(scope.partition_scope(self.root, files, "production"))
        tests = set(scope.partition_scope(self.root, files, "tests"))
        self.assertFalse(production & tests)
        self.assertEqual(production | tests, set(scope.partition_scope(self.root, files, "all")))

    def test_partition_explicit_mismatch_is_error(self):
        test_file = str(self.add("tests/probe.cpp", "int probe;\n"))
        with self.assertRaisesRegex(scope.ScopeError, "does not belong"):
            scope.partition_scope(self.root, [test_file], "production", strict=True)
        with self.assertRaisesRegex(scope.ScopeError, "does not belong"):
            scope.partition_scope(self.root, [str(self.root / "src/a.cpp")], "tests", strict=True)

    def test_partition_all_accepts_explicit_outside_fixture(self):
        outside = str(self.root.parent / "outside.cpp")
        self.assertEqual(scope.partition_scope(self.root, [outside], "all", strict=True), (outside,))
        with self.assertRaisesRegex(scope.ScopeError, "outside"):
            scope.partition_scope(self.root, [outside], "production")

    def test_partition_unknown_category_is_error_even_for_empty_input(self):
        with self.assertRaises(scope.ScopeError):
            scope.partition_scope(self.root, [], "unknown")

    def test_partition_full_override_does_not_switch_code_category(self):
        self.add("tests/not_in_cdb.hpp", "int loose_fixture;\n", tracked=False)
        self.committed = b"M\0src/a.cpp\0"
        with patch.dict(os.environ, {"QUALITY_FULL": "1"}):
            files = self.select().files
        tests = scope.partition_scope(self.root, files, "tests")
        self.assert_names(scope.ScopeSelection(True, tests, "partition"), ["tests/not_in_cdb.hpp"])
        self.assertNotIn(str(self.root / "tests/not_in_cdb.hpp"), scope.partition_scope(self.root, files))

    def test_partition_registered_boundary_is_not_a_prefix_guess(self):
        names = ["tests_backup/fixture.h", "tests.cpp", "src/tests.cpp"]
        files = [str(self.add(name, "int production;\n")) for name in names]
        self.assertEqual(scope.partition_scope(self.root, files), tuple(sorted(files)))

    def test_committed_source(self):
        self.committed = b"M\0src/a.cpp\0"
        result = self.select()
        self.assertFalse(result.full)
        self.assert_names(result, ["src/a.cpp"])

    def test_worktree_and_untracked_union(self):
        self.committed = b"M\0src/a.cpp\0"
        self.worktree = b"M\0src/b.cpp\0"
        self.add("src/new.cpp", "int untracked;", tracked=False)
        self.assert_names(self.select(), ["src/a.cpp", "src/b.cpp", "src/new.cpp"])

    def test_header_and_owning_tu(self):
        self.committed = b"M\0include/common.h\0"
        result = self.select()
        self.assertFalse(result.full)
        self.assert_names(result, ["include/common.h", "src/a.cpp"])

    def test_transitive_header_and_cycle(self):
        self.add("include/deep.hpp", '#include "common.h"\n')
        (self.root / "include/common.h").write_text('#include "deep.hpp"\n')
        self.committed = b"M\0include/deep.hpp\0"
        self.assert_names(self.select(), ["include/deep.hpp", "src/a.cpp"])

    def test_angle_include_does_not_search_source_directory(self):
        self.add("src/common.h", "int source_local;\n")
        (self.root / "src/b.cpp").write_text('#include <common.h>\n')
        self.committed = b"M\0include/common.h\0"
        self.assert_names(self.select(), ["include/common.h", "src/b.cpp"])

    def test_header_shared_by_two_tus(self):
        (self.root / "src/b.cpp").write_text('#include "common.h"\n')
        self.worktree = b"M\0include/common.h\0"
        self.assert_names(self.select(), ["include/common.h", "src/a.cpp", "src/b.cpp"])

    def test_header_macro_falls_back(self):
        (self.root / "src/b.cpp").write_text('#include CHOSEN_HEADER\n')
        self.committed = b"M\0include/common.h\0"
        self.assertTrue(self.select().full)

    def test_unresolved_system_include_falls_back(self):
        (self.root / "src/b.cpp").write_text('#include <unknown_implicit_library>\n')
        self.committed = b"M\0include/common.h\0"
        self.assertTrue(self.select().full)

    def test_unknown_header_owner_falls_back(self):
        self.add("include/orphan.hh", "int orphan;\n")
        self.committed = b"M\0include/orphan.hh\0"
        self.assertTrue(self.select().full)

    def test_forced_include_dependency(self):
        self.database([{"file": "src/b.cpp", "directory": str(self.root),
                        "arguments": ["c++", "-Iinclude", "-include", "common.h", "src/b.cpp"]}])
        self.committed = b"M\0include/common.h\0"
        self.assert_names(self.select(), ["include/common.h", "src/b.cpp"])

    def test_unsupported_quote_search_option_is_full(self):
        self.database([{"file": "src/a.cpp", "directory": str(self.root),
                        "arguments": ["c++", "-iquote", "include", "src/a.cpp"]}])
        self.committed = b"M\0include/common.h\0"
        self.assertTrue(self.select().full)

    def test_forced_include_searches_compiler_directory(self):
        self.add("common.h", "int cwd_header;\n")
        self.add("src/common.h", "int wrong_source_header;\n")
        self.database([{"file": "src/b.cpp", "directory": str(self.root),
                        "arguments": ["c++", "-include", "common.h", "src/b.cpp"]}])
        self.committed = b"M\0common.h\0"
        self.assert_names(self.select(), ["common.h", "src/b.cpp"])

    def test_response_file_falls_back(self):
        self.database([{"file": "src/a.cpp", "directory": str(self.root), "arguments": ["c++", "@flags.rsp"]}])
        self.committed = b"M\0include/common.h\0"
        self.assertTrue(self.select().full)

    def test_source_without_cdb_entry_preserves_coverage(self):
        self.add("src/standalone.cc", "int standalone;\n")
        self.committed = b"M\0src/standalone.cc\0"
        self.assert_names(self.select(), ["src/standalone.cc"])

    def test_missing_git_falls_back_to_full_filesystem(self):
        self.missing_git = True
        self.add("cmake-build-release/generated.cpp", "int ignored;\n")
        self.add(".agent-workspace/fixture.cpp", "int ignored;\n")
        result = self.select()
        self.assertTrue(result.full)
        self.assert_names(result, ["include/common.h", "src/a.cpp", "src/b.cpp"])

    def test_missing_range_is_full(self):
        self.committed = b"M\0src/a.cpp\0"
        self.assertTrue(scope.select_scope(self.root, self.build).full)

    def test_explicit_full_overrides_range(self):
        self.committed = b"M\0src/a.cpp\0"
        self.assertTrue(self.select(full=True).full)
        with patch.dict(os.environ, {"QUALITY_FULL": "1"}):
            self.assertTrue(self.select().full)

    def test_precommit_range_environment(self):
        self.committed = b"M\0src/b.cpp\0"
        with patch.dict(os.environ, {"PRE_COMMIT_FROM_REF": "base", "PRE_COMMIT_TO_REF": "target"}):
            result = scope.select_scope(self.root, self.build)
        self.assertFalse(result.full)
        self.assert_names(result, ["src/b.cpp"])

    def test_checkout_must_match_push_target(self):
        self.head = b"another"
        self.assertTrue(self.select().full)

    def test_rename_delete_and_conflict_are_full(self):
        for diff in (b"R100\0include/common.h\0include/renamed.h\0",
                     b"D\0include/common.h\0", b"U\0src/a.cpp\0"):
            with self.subTest(diff=diff):
                self.committed = diff
                self.assertTrue(self.select().full)

    def test_shared_configuration_and_scripts_force_full(self):
        for name in ("DEPS", "nested/DEPS", "CMakeLists.txt", "cmake/foo.cmake", "scripts/check.py",
                     ".pre-commit-config.yaml", ".clang-tidy", ".github/workflows/ci.yml", "version.h.in"):
            with self.subTest(name=name):
                self.committed = b"M\0" + name.encode() + b"\0"
                self.assertTrue(self.select().full)

    def test_existing_exclude_policy_preserved(self):
        self.add(".pre-commit-config.yaml", "exclude: '^(vendor|output)/'\n")
        self.add("vendor/foreign.hpp", "int foreign;\n")
        self.add("output/generated.cpp", "int generated;\n")
        self.assert_names(self.select(full=True), ["include/common.h", "src/a.cpp", "src/b.cpp"])
        self.committed = b"M\0vendor/foreign.hpp\0"
        self.assertTrue(self.select().full)
        self.assert_names(self.select(), ["include/common.h", "src/a.cpp", "src/b.cpp"])

    def test_missing_or_invalid_cdb_forces_full(self):
        self.committed = b"M\0src/a.cpp\0"
        for payload in ("[]", "bad json", '[{"file":"src/a.cpp"}]'):
            with self.subTest(payload=payload):
                (self.build / "compile_commands.json").write_text(payload)
                self.assertTrue(self.select().full)
        (self.build / "compile_commands.json").unlink()
        self.assertTrue(self.select().full)

    def test_dedup_and_space_paths(self):
        self.add("src/with space.cxx", "int x;")
        self.committed = b"M\0src/with space.cxx\0M\0src/with space.cxx\0"
        self.worktree = self.committed
        self.assert_names(self.select(), ["src/with space.cxx"])

    def test_empty_and_docs_only_ranges_skip_cpp(self):
        self.assertEqual(self.select().files, ())
        self.committed = b"M\0README.md\0"
        self.assertEqual(self.select().files, ())

    def test_invalid_status_and_path_escape_fall_back(self):
        for diff in (b"R100\0old.h\0", b"M\0../escape.cpp\0", b"?\0x.cpp\0"):
            with self.subTest(diff=diff):
                self.committed = diff
                self.assertTrue(self.select().full)

    def test_no_result_cache_after_source_include_changes(self):
        self.committed = b"M\0include/common.h\0"
        first = self.select()
        (self.root / "src/b.cpp").write_text('#include "common.h"\n')
        second = self.select()
        self.assertNotEqual(first.files, second.files)
        self.assert_names(second, ["include/common.h", "src/a.cpp", "src/b.cpp"])


if __name__ == "__main__":
    unittest.main()
