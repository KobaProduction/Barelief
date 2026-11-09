import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from project_environment import resolve
from prepare_quality_profiles import configure_command, cross_environment, prepare
from run_project_tool import main as run_project_tool


class ProjectEnvironmentTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()

    def write(self, content):
        (self.root / ".env.local").write_text(content, encoding="utf-8")

    def test_missing_local_configuration_does_not_require_a_machine_setup(self):
        self.assertEqual(resolve(self.root, {}), {})

    def test_relative_paths_quotes_and_expansion(self):
        self.write('IDF_PATH="SDK sources"\nIDF_TOOLS_PATH=${IDF_PATH}/tools\nCC=\n')
        values = resolve(self.root, {})
        self.assertEqual(values, {"IDF_PATH": str(self.root / "SDK sources"),
                                  "IDF_TOOLS_PATH": str(self.root / "SDK sources/tools")})

    def test_process_environment_takes_precedence_in_dependent_paths(self):
        self.write('IDF_PATH=old\nIDF_TOOLS_PATH=${IDF_PATH}/cache\n')
        values = resolve(self.root, {"IDF_PATH": str(self.root / "selected")})
        self.assertEqual(values["IDF_TOOLS_PATH"], str(self.root / "selected/cache"))

    def test_invalid_configuration_fails_without_shell_evaluation(self):
        for content in ('GH_TOKEN=secret\n', 'IDF_PATH=${UNDEFINED}\n',
                        'IDF_PATH=$(command)\n', 'CC=\nCC=compiler\n', 'no assignment\n'):
            with self.subTest(content=content):
                self.write(content)
                with self.assertRaises(ValueError):
                    resolve(self.root, {})

    def test_cross_profiles_do_not_inherit_desktop_compiler_overrides(self):
        environment = {"CC": "host-c", "CXX": "host-cxx", "PYTHONPATH": "unrelated",
            "ESP_CLANG_TIDY": str(self.root / "clang/bin/clang-tidy"),
            "ESP32_TOOLCHAIN_DIR": str(self.root / "gcc"),
            "IDF_PYTHON_ENV_PATH": str(self.root / "python"), "PATH": "original"}
        selected = cross_environment(environment)
        self.assertNotIn("CC", selected)
        self.assertNotIn("CXX", selected)
        self.assertNotIn("PYTHONPATH", selected)
        self.assertEqual(selected["IDF_TOOLCHAIN"], "clang")
        self.assertIn(str(self.root / "clang/bin"), selected["PATH"])
        self.assertEqual(environment["CC"], "host-c")

    def test_profile_commands_keep_sdkconfig_and_native_compilers_separate(self):
        environment = {name: str(self.root / name) for name in ("IDF_PATH", "IDF_PYTHON_ENV_PATH",
            "ESP32_TOOLCHAIN_DIR", "ESP_DSP_SOURCE_DIR", "GMF_FFT_SOURCE_DIR", "BDSP_SOURCE_DIR", "CC", "CXX")}
        profile = {"source": "examples/esp32_fft_benchmark", "target": "esp32s3", "cache": {}}
        build = self.root / "cmake-build-dev/quality-contexts/fft"
        command = configure_command(self.root, build, profile, environment, "cmake", "ninja")
        self.assertIn("-DIDF_TOOLCHAIN=clang", command)
        self.assertIn("-DIDF_TARGET=esp32s3", command)
        self.assertIn("-DSDKCONFIG=" + (build / "sdkconfig").as_posix(), command)
        self.assertFalse(any(value.startswith("-DCMAKE_CXX_COMPILER=") for value in command))
        profile.pop("target")
        host = configure_command(self.root, build, profile, environment, "cmake", "ninja")
        self.assertIn("-DCMAKE_CXX_COMPILER=" + environment["CXX"], host)

    def test_cmake_consumes_local_environment_before_project_configuration(self):
        cmake = os.environ.get("CMAKE_EXE") or shutil.which("cmake")
        ninja = os.environ.get("NINJA_EXE") or shutil.which("ninja")
        self.assertTrue(cmake and ninja, "CMake and Ninja must be available for the configured project")
        (self.root / "scripts").mkdir()
        shutil.copyfile(ROOT / "scripts/project_environment.py", self.root / "scripts/project_environment.py")
        self.write('QT_ROOT="Qt location"\nIDF_PATH=SDK\n')
        module = (ROOT / "cmake/ProjectEnvironment.cmake").as_posix()
        (self.root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.30)\ninclude("' + module + '")\n'
            'project(EnvironmentProbe LANGUAGES NONE)\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/observed.txt" "$ENV{IDF_PATH}\\n${CMAKE_PREFIX_PATH}")\n')
        environment = dict(os.environ)
        environment.pop("IDF_PATH", None)
        environment.pop("QT_ROOT", None)
        completed = subprocess.run([cmake, "-S", str(self.root), "-B", str(self.root / "build"),
            "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + ninja], env=environment, capture_output=True, text=True)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        observed = (self.root / "build/observed.txt").read_text().replace("\\", "/").splitlines()
        self.assertEqual(observed, [(self.root / "SDK").as_posix(), (self.root / "Qt location").as_posix()])

    def test_failed_preparation_keeps_previous_profile_registry(self):
        environment = {}
        required = {"IDF_PATH": "tools/idf_tools.py", "GMF_FFT_SOURCE_DIR": "src/common/fft_power2_q15.c",
            "ESP_DSP_SOURCE_DIR": "CMakeLists.txt", "BDSP_SOURCE_DIR": "CMakeLists.txt",
            "ESP32_TOOLCHAIN_DIR": "bin/xtensa-esp-elf-gcc.exe" if os.name == "nt" else "bin/xtensa-esp-elf-gcc",
            "IDF_PYTHON_ENV_PATH": "Scripts/python.exe" if os.name == "nt" else "bin/python"}
        for key, child in required.items():
            value = self.root / key
            path = value / child; path.parent.mkdir(parents=True, exist_ok=True); path.touch()
            environment[key] = str(value)
        for key in ("ESP_CLANG_TIDY", "CC", "CXX", "CMAKE_EXE", "NINJA_EXE"):
            path = self.root / (key + ".exe"); path.touch(); environment[key] = str(path)
        (self.root / "cmake").mkdir()
        (self.root / "cmake/quality-profiles.json").write_text(json.dumps({"schemaVersion": 1,
            "profiles": [{"name": "sdk", "source": "sdk", "target": "esp32", "cache": {}}]}))
        registry = self.root / ".agent-workspace/quality-contexts.json"
        registry.parent.mkdir(); registry.write_text("original profiles")
        with patch.dict(os.environ, environment), patch("prepare_quality_profiles.activate"), patch(
                "prepare_quality_profiles.subprocess.run", return_value=subprocess.CompletedProcess([], 1)):
            with self.assertRaisesRegex(ValueError, "failed"):
                prepare(self.root, self.root / "cmake-build-dev/quality-contexts")
        self.assertEqual(registry.read_text(), "original profiles")

    def test_native_build_works_without_compiler_dll_directory_in_parent_path(self):
        cmake = os.environ.get("CMAKE_EXE") or shutil.which("cmake")
        ninja = os.environ.get("NINJA_EXE") or shutil.which("ninja")
        compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
        self.assertTrue(cmake and ninja and compiler, "Native build tools must be configured")
        compiler = shutil.which(compiler) or compiler
        (self.root / "scripts").mkdir()
        shutil.copyfile(ROOT / "scripts/project_environment.py", self.root / "scripts/project_environment.py")
        self.write('CXX=' + compiler + '\nCMAKE_EXE=' + cmake + '\nNINJA_EXE=' + ninja + '\n')
        module = (ROOT / "cmake/ProjectEnvironment.cmake").as_posix()
        (self.root / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.30)\n'
            'include("' + module + '")\nproject(NativeEnvironment LANGUAGES CXX)\n'
            'set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\nadd_executable(probe main.cpp)\n'
            'add_custom_target(nested_tools ALL COMMAND cmake -E true)\n')
        (self.root / "main.cpp").write_text('int main() { return 0; }\n')
        environment = dict(os.environ)
        if os.name == "nt":
            environment["PATH"] = os.pathsep.join(map(str, (Path(os.environ["SystemRoot"]) / "System32",
                Path(sys.executable).parent)))
        configure = subprocess.run([cmake, "-S", str(self.root), "-B", str(self.root / "build"),
            "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + ninja], env=environment, capture_output=True, text=True)
        self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)
        build = subprocess.run([cmake, "--build", str(self.root / "build")], env=environment, capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        database = json.loads((self.root / "build/compile_commands.json").read_text())
        self.assertNotIn("run_project_tool", database[0]["command"], "Analysis must retain the compiler command, not its process launcher")

    def test_build_launcher_preserves_argument_boundaries_and_failure_status(self):
        with patch("run_project_tool.activate") as activate_environment, patch(
                "run_project_tool.subprocess.run", return_value=subprocess.CompletedProcess([], 37)) as execute:
            status = run_project_tool(["--source", str(self.root), "--", "compiler", "two words", "-DNAME=value"])
        activate_environment.assert_called_once_with(self.root)
        execute.assert_called_once_with(["compiler", "two words", "-DNAME=value"])
        self.assertEqual(status, 37)


if __name__ == "__main__":
    unittest.main()
