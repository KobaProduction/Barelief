import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from quality_compile_context import exact_header_filter, contexts_for, expand_responses, load_contexts, materialize

from quality_compile_context import CompileContext, embedded_target, frontend_arguments, split_gnu_response


class CompilationContextTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.header = self.root / "fragment.hpp"
        self.owner = self.root / "owner.cpp"
        self.build = self.root / "build"
        self.build.mkdir()
        self.header.write_text("#pragma once\ninline OwnerType value() { return OwnerType(1); }\n")
        self.owner.write_text('using OwnerType = int;\n#include "fragment.hpp"\n')
        self.entries = [{"directory": str(self.root), "file": str(self.owner),
                         "arguments": ["clang++", "-std=c++23", "-c", str(self.owner)]}]
        self.save()

    def save(self):
        (self.build / "compile_commands.json").write_text(json.dumps(self.entries))

    def test_exact_and_header_owners_are_proven(self):
        contexts = load_contexts(self.root, [self.build])
        self.assertEqual(contexts_for(self.owner, contexts, self.root), contexts)
        self.assertEqual(contexts_for(self.header, contexts, self.root), contexts)
        with self.assertRaisesRegex(ValueError, "No proven"):
            contexts_for(self.root / "unknown.hpp", contexts, self.root)
        fixture = self.root / "tests/fixture.h"
        fixture.parent.mkdir()
        fixture.write_text("#define FIXTURE_ATTRIBUTE\n")
        production = self.root / "production.h"
        production.write_text("#define PRODUCTION_ATTRIBUTE\n")
        c_owner = self.root / "vendor.c"
        c_owner.write_text('#include "tests/fixture.h"\n#include "production.h"\n')
        c_context = CompileContext(c_owner,self.root,("gcc", "-I" + str(fixture.parent), "-c", str(c_owner)),
                                   self.build / "compile_commands.json")
        self.assertEqual(contexts_for(fixture,(c_context,),self.root),(c_context,))
        with self.assertRaisesRegex(ValueError,"No proven"):
            contexts_for(production,(c_context,),self.root)

    def test_all_command_variants_are_retained(self):
        variant = dict(self.entries[0])
        variant["arguments"] = [*variant["arguments"], "-DFEATURE=1"]
        self.entries += [variant, self.entries[0]]
        self.save()
        owners = contexts_for(self.header, load_contexts(self.root, [self.build]), self.root)
        self.assertEqual(len(owners), 2)

    def test_equivalent_owner_commands_deduplicate_emission_only_differences(self):
        other = self.root / "other.cpp"
        other.write_text(self.owner.read_text())
        self.entries[0]["arguments"] += ["-o", "first.obj", "-MMD", "-MF", "first.d"]
        self.entries.append({"directory": str(self.root), "file": str(other),
            "arguments": ["clang++", "-std=c++23", "-c", str(other), "-o", "other.obj", "-MF", "other.d"]})
        self.save()
        owners = contexts_for(self.header, load_contexts(self.root, [self.build]), self.root)
        self.assertEqual(len(owners), 1)

    def test_fragment_owners_keep_their_distinct_preludes(self):
        fragment = self.root / "examples/esp32_fft_benchmark/main/correctness.h"
        fragment.parent.mkdir(parents=True)
        fragment.write_text("#pragma once\ninline OwnerType value() { return 1; }")
        self.owner.write_text('using OwnerType = int;\n#include "examples/esp32_fft_benchmark/main/correctness.h"')
        other = self.root / "other.cpp"
        other.write_text('using OwnerType = long;\n#include "examples/esp32_fft_benchmark/main/correctness.h"')
        self.entries.append({"directory": str(self.root), "file": str(other),
                            "arguments": ["clang++", "-std=c++23", "-c", str(other)]})
        self.save()
        owners = contexts_for(fragment, load_contexts(self.root, [self.build]), self.root)
        self.assertEqual(len(owners), 2)

    def test_define_undefine_order_is_not_normalized_away(self):
        first = [*self.entries[0]["arguments"], "-DFEATURE=1", "-UFEATURE"]
        second = [*self.entries[0]["arguments"], "-UFEATURE", "-DFEATURE=1"]
        self.entries[0]["arguments"] = first
        self.entries.append({**self.entries[0], "arguments": second})
        self.save()
        self.assertEqual(len(contexts_for(self.header, load_contexts(self.root, [self.build]), self.root)), 2)

    def test_primary_owner_precedes_additional_platform_profiles(self):
        secondary = self.root / "native"
        secondary.mkdir()
        other = self.root / "native.cpp"
        other.write_text(self.owner.read_text())
        entry = {"directory": str(self.root), "file": str(other),
                 "arguments": ["clang++", "-DPLATFORM=1", "-c", str(other)]}
        (secondary / "compile_commands.json").write_text(json.dumps([entry]))
        contexts = load_contexts(self.root, [self.build, secondary])
        owners = contexts_for(self.header, contexts, self.root)
        self.assertEqual([owner.database for owner in owners], [self.build / "compile_commands.json"])
        fallback = contexts_for(other, contexts, self.root)
        self.assertEqual([owner.database for owner in fallback], [secondary / "compile_commands.json"])

    def test_response_files_expand_in_place_with_nested_provenance(self):
        inner = self.root / "inner flags.rsp"
        inner.write_text("-DFEATURE=1 -UFEATURE")
        outer = self.root / "outer.rsp"
        outer.write_text('@"' + str(inner) + '" -std=c++23')
        values, provenance = expand_responses(("clang++", "@" + str(outer), "-DFEATURE=2"), self.root)
        self.assertEqual(values, ("clang++", "-DFEATURE=1", "-UFEATURE", "-std=c++23", "-DFEATURE=2"))
        self.assertEqual({path for path, digest in provenance}, {str(inner), str(outer)})
        self.assertTrue(all(len(digest) == 64 for path, digest in provenance))

    def test_response_file_cycle_and_missing_input_fail_closed(self):
        cyclic = self.root / "cycle.rsp"
        cyclic.write_text("@" + str(cyclic))
        with self.assertRaisesRegex(ValueError, "cycle"):
            expand_responses(("@" + str(cyclic),), self.root)
        with self.assertRaisesRegex(ValueError, "missing"):
            expand_responses(("@" + str(self.root / "missing.rsp"),), self.root)

    def test_test_owner_context_can_cover_a_production_header_without_linting_tests(self):
        contexts = load_contexts(self.root, [self.build])
        owners = contexts_for(self.header, contexts, self.root, test_sources=frozenset({self.owner}))
        self.assertEqual(owners, contexts)

    def test_adaptation_change_retains_historical_materialized_profile(self):
        context = load_contexts(self.root, [self.build])[0]
        first_arguments = list(context.arguments)
        with patch("quality_compile_context.frontend_arguments", return_value=first_arguments):
            first = materialize(self.owner, context, self.root / "contexts")
        first_contents = (first / "compile_commands.json").read_text()
        with patch("quality_compile_context.frontend_arguments", return_value=[*first_arguments, "-mcpu=esp32s3"]):
            second = materialize(self.owner, context, self.root / "contexts")
        self.assertNotEqual(first, second)
        self.assertEqual((first / "compile_commands.json").read_text(), first_contents)
        self.assertEqual(json.loads((second / "compile_commands.json").read_text())[0]["arguments"][-1],
                         "-mcpu=esp32s3")

    def test_gnu_response_multiline_flags_preserve_actual_idf_arguments(self):
        response = self.root / "cxxflags"
        response.write_bytes(b'"-specs=C:/sdk/specs/picolibc.specs"\r\n-Wno-frame-address\r\n-mlongcalls\r\n')
        arguments, provenance = expand_responses(("native-g++", "@" + str(response)), self.root)
        self.assertEqual(arguments, ("native-g++", "-specs=C:/sdk/specs/picolibc.specs",
                                     "-Wno-frame-address", "-mlongcalls"))
        self.assertEqual(provenance[0][0], str(response))

    def test_gnu_response_quote_escape_and_continuation_rules(self):
        self.assertEqual(split_gnu_response("'two words' \"C:\\sdk\\path\" escaped\\ space \"\""),
                         ("two words", "C:\\sdk\\path", "escaped space", ""))
        self.assertEqual(split_gnu_response("-DFLAG=a\\\nvalue\t-DOTHER=2"), ("-DFLAG=avalue", "-DOTHER=2"))

    def test_gnu_response_malformed_input_is_rejected(self):
        for contents in ("'unclosed", "trailing\\", "bad\0value"):
            with self.subTest(contents=contents), self.assertRaises(ValueError):
                split_gnu_response(contents)

    def test_official_clang_profiles_route_to_embedded_tool_without_gcc_adaptation(self):
        for cpu in ("esp32", "esp32s2", "esp32s3"):
            with self.subTest(cpu=cpu):
                arguments = ("C:/official/bin/clang++.exe", "--target=xtensa-esp-elf", "-mcpu=" + cpu,
                             "-Werror", "-Wno-extern-c-compat", "-c", str(self.owner))
                context = CompileContext(self.owner, self.root, arguments, self.build / "compile_commands.json")
                self.assertEqual(embedded_target(context), "xtensa-" + cpu + "-elf")
                self.assertEqual(frontend_arguments(context), list(arguments))

    def test_native_gcc_profile_requires_real_official_clang_configuration(self):
        for cpu in ("esp32", "esp32s2", "esp32s3"):
            with self.subTest(cpu=cpu):
                context = CompileContext(self.owner, self.root,
                    ("C:/tool/bin/xtensa-" + cpu + "-elf-g++.exe", "-c", str(self.owner)),
                    self.build / "compile_commands.json")
                with self.assertRaisesRegex(ValueError, "IDF_TOOLCHAIN=clang"):
                    frontend_arguments(context)

    def test_unproven_official_clang_cpu_or_target_fails_closed(self):
        for flags in (("--target=xtensa-esp-elf",),
                      ("--target=xtensa-esp-elf", "-mcpu=unknown"),
                      ("--target=xtensa-esp32-elf", "-mcpu=esp32")):
            with self.subTest(flags=flags):
                context = CompileContext(self.owner, self.root, ("clang++", *flags, "-c", str(self.owner)),
                                         self.build / "compile_commands.json")
                with self.assertRaises(ValueError):
                    embedded_target(context)

    def test_split_official_clang_flags_are_recognized_without_modification(self):
        flags = ("-target", "xtensa-esp-elf", "-mcpu", "esp32s3")
        context = CompileContext(self.owner, self.root, ("clang++", *flags, "-c", str(self.owner)),
                                 self.build / "compile_commands.json")
        self.assertEqual(embedded_target(context), "xtensa-esp32s3-elf")
        self.assertEqual(frontend_arguments(context), list(context.arguments))

    def test_late_target_cpu_override_and_missing_values_fail_closed(self):
        base = ("clang++", "--target=xtensa-esp-elf", "-mcpu=esp32")
        for tail in (("-target", "x86_64-pc-windows-msvc"), ("--target=x86_64-pc-windows-msvc",),
                     ("-mcpu", "esp32s3"), ("-mcpu=unknown",), ("--target",), ("-mcpu=",)):
            with self.subTest(tail=tail):
                context = CompileContext(self.owner, self.root, (*base, *tail), self.build / "compile_commands.json")
                with self.assertRaises(ValueError):
                    embedded_target(context)

    def test_generic_native_xtensa_driver_cannot_fall_back_to_host(self):
        context = CompileContext(self.owner, self.root, ("xtensa-esp-elf-g++.exe", "-c", str(self.owner)),
                                 self.build / "compile_commands.json")
        with self.assertRaisesRegex(ValueError, "IDF_TOOLCHAIN=clang"):
            frontend_arguments(context)

    def test_desktop_clang_context_is_not_reclassified_as_embedded(self):
        context = load_contexts(self.root, [self.build])[0]
        self.assertIsNone(embedded_target(context))
        self.assertEqual(frontend_arguments(context), list(context.arguments))

    def test_missing_or_empty_profiles_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            load_contexts(self.root, [self.root / "absent"])
        self.entries = []
        self.save()
        with self.assertRaisesRegex(ValueError, "empty"):
            load_contexts(self.root, [self.build])

    def _real_policy_tools(self):
        executable = os.environ.get("CLANG_TIDY") or shutil.which("clang-tidy")
        self.assertTrue(executable, "clang-tidy is a required meta-hook dependency")
        tools = [(executable, False)]
        if os.environ.get("ESP_CLANG_TIDY"):
            embedded = shutil.which(os.environ["ESP_CLANG_TIDY"])
            self.assertTrue(embedded, "Configured ESP_CLANG_TIDY must exist")
            tools.append((embedded, True))
        return tools

    def _real_policy_context(self, executable, embedded):
        context = contexts_for(self.header, load_contexts(self.root, [self.build]), self.root)[0]
        if embedded:
            compiler = Path(executable).parent / "clang++.exe"
            self.assertTrue(compiler.is_file())
            context = CompileContext(self.owner, self.root,
                (str(compiler), "--target=xtensa-esp-elf", "-mcpu=esp32s3", "-std=c++23", "-c", str(self.owner)),
                self.build / "compile_commands.json")
        return context

    def _assert_policy_diagnostic(self, executable, profile, main, *, header_filter=None):
        options = ["--header-filter=" + header_filter,
                   "--line-filter=" + json.dumps([{"name": self.header.name}])] if header_filter else []
        result = subprocess.run([executable, "-p=" + str(profile), "--extra-arg-before=-xc++", *options,
            "--checks=-*,readability-function-size", "--warnings-as-errors=*",
            "--config={CheckOptions: {readability-function-size.StatementThreshold: 20}}", str(main)],
            capture_output=True, encoding="utf-8", errors="replace")
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("readability-function-size", output)
        normalized = output.replace(chr(92), "/")
        self.assertTrue(any(line.startswith(self.header.as_posix() + ":") and
                            ": error: function 'oversized'" in line for line in normalized.splitlines()), output)
        if header_filter:
            self.assertNotIn("function 'scenario_owner'", output)
        for forbidden in ("after last input file has no effect", "unknown type name", "file not found",
                          "redefinition", "pragma once in main file"):
            self.assertNotIn(forbidden, output)
        print("Real header policy regression:", executable, "fragment=" + str(bool(header_filter)))

    def test_unguarded_fragment_policy_blocks_in_its_real_owner_once(self):
        statements = "\n".join("    value += " + str(index) + ";" for index in range(25))
        self.header.write_text("inline OwnerType oversized() {\n    OwnerType value=0;\n" + statements + "\n    return value;\n}\n")
        self.owner.write_text(self.owner.read_text() +
                              "\nint scenario_owner() { int value=0;\n" + statements + "\nreturn value; }\n")
        for executable, embedded in self._real_policy_tools():
            with self.subTest(executable=executable):
                context = self._real_policy_context(executable, embedded)
                profile = materialize(self.header, context, self.root / "contexts", fragment=True)
                self._assert_policy_diagnostic(executable, profile, self.owner,
                                               header_filter=exact_header_filter(self.header))
                entry = json.loads((profile / "compile_commands.json").read_text())[0]
                self.assertEqual(entry["file"], str(self.owner))
                self.assertEqual(entry["arguments"], list(context.arguments))

    def test_self_contained_header_uses_header_language_before_input(self):
        statements = "\n".join("    value += " + str(index) + ";" for index in range(25))
        self.header.write_text("#pragma once\ninline int oversized() {\n    int value=0;\n" + statements + "\n    return value;\n}\n")
        for executable, embedded in self._real_policy_tools():
            with self.subTest(executable=executable):
                context = self._real_policy_context(executable, embedded)
                profile = materialize(self.header, context, self.root / "contexts")
                self._assert_policy_diagnostic(executable, profile, self.header)
                entry = json.loads((profile / "compile_commands.json").read_text())[0]
                index = entry["arguments"].index(str(self.header))
                self.assertEqual(entry["arguments"][index - 2:index], ["-x", "c++-header"])


if __name__ == "__main__":
    unittest.main()
