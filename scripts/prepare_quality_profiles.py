"""Prepare real SDK/benchmark compile databases for the primary checkout's lint."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from project_environment import activate


def required_path(environment: dict, key: str, child: str) -> Path:
    value = environment.get(key)
    if not value or not (Path(value) / child).is_file():
        raise ValueError(f"Set {key} in .env.local to an existing installation (missing {child})")
    return Path(value).resolve()


def tool(root: Path, environment: dict, key: str, fallback: str) -> str:
    executable = environment.get(key) or shutil.which(fallback)
    if not executable or not Path(executable).is_file():
        raise ValueError(f"Set {key} in {root / '.env.local'}")
    return str(Path(executable).resolve())


def cross_environment(environment: dict) -> dict:
    result = dict(environment)
    for name in ("CC", "CXX", "PYTHONPATH", "PYTHONHOME"):
        result.pop(name, None)
    result.update(IDF_TOOLCHAIN="clang", IDF_COMPONENT_MANAGER="0")
    clang = Path(result["ESP_CLANG_TIDY"]).parent
    compiler = Path(result["ESP32_TOOLCHAIN_DIR"]) / "bin"
    python = Path(result["IDF_PYTHON_ENV_PATH"]) / ("Scripts" if os.name == "nt" else "bin")
    result["PATH"] = os.pathsep.join(map(str, (python, clang, compiler))) + os.pathsep + result.get("PATH", "")
    return result


def configure_command(root: Path, build: Path, profile: dict, environment: dict, cmake: str, ninja: str) -> list[str]:
    source = root / profile["source"]
    options = {"CMAKE_MAKE_PROGRAM": ninja.replace("\\", "/"), "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
               "CMAKE_BUILD_TYPE": "Release", "GMF_FFT_SOURCE_DIR": Path(environment["GMF_FFT_SOURCE_DIR"]).as_posix()}
    if profile.get("target"):
        options.update(IDF_TARGET=profile["target"], IDF_TOOLCHAIN="clang",
            SDKCONFIG=(build / "sdkconfig").as_posix(),
            PYTHON=(Path(environment["IDF_PYTHON_ENV_PATH"]) / ("Scripts/python.exe" if os.name == "nt" else "bin/python")).as_posix(),
            ESP_IDF_SOURCE_DIR=Path(environment["IDF_PATH"]).as_posix(),
            ESP32_TOOLCHAIN_DIR=Path(environment["ESP32_TOOLCHAIN_DIR"]).as_posix(),
            ESP_DSP_SOURCE_DIR=Path(environment["ESP_DSP_SOURCE_DIR"]).as_posix(),
            FETCHCONTENT_SOURCE_DIR_ESP_DSP=Path(environment["ESP_DSP_SOURCE_DIR"]).as_posix(),
            BDSP_SOURCE_DIR=Path(environment["BDSP_SOURCE_DIR"]).as_posix())
    else:
        options.update(CMAKE_C_COMPILER=environment["CC"], CMAKE_CXX_COMPILER=environment["CXX"])
    options.update(profile["cache"])
    return [cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
            *[f"-D{key}={value}" for key, value in options.items()]]


def prepare(root: Path, build_root: Path, selected_names: set[str] | None = None) -> None:
    activate(root)
    environment = os.environ.copy()
    required_path(environment, "IDF_PATH", "tools/idf_tools.py")
    required_path(environment, "GMF_FFT_SOURCE_DIR", "src/common/fft_power2_q15.c")
    required_path(environment, "ESP_DSP_SOURCE_DIR", "CMakeLists.txt")
    required_path(environment, "BDSP_SOURCE_DIR", "CMakeLists.txt")
    required_path(environment, "ESP32_TOOLCHAIN_DIR", "bin/xtensa-esp-elf-gcc.exe" if os.name == "nt" else "bin/xtensa-esp-elf-gcc")
    for key in ("ESP_CLANG_TIDY", "CC", "CXX"):
        required_path(environment, key, "")
    required_path(environment, "IDF_PYTHON_ENV_PATH", "Scripts/python.exe" if os.name == "nt" else "bin/python")
    cmake, ninja = tool(root, environment, "CMAKE_EXE", "cmake"), tool(root, environment, "NINJA_EXE", "ninja")
    manifest = json.loads((root / "cmake/quality-profiles.json").read_text(encoding="utf-8-sig"))
    if manifest.get("schemaVersion") != 1:
        raise ValueError("Unsupported quality profile manifest")
    if selected_names and selected_names - {profile["name"] for profile in manifest["profiles"]}:
        raise ValueError("Unknown quality profile selection")
    paths = []
    cross = cross_environment(environment)
    for profile in manifest["profiles"]:
        build = build_root / profile["name"]
        build.mkdir(parents=True, exist_ok=True)
        if selected_names and profile["name"] not in selected_names:
            if not (build / "compile_commands.json").is_file():
                raise ValueError("Prepare the missing profile first: " + profile["name"])
            paths.append(build.relative_to(root).as_posix())
            continue
        if profile.get("sdkconfig"):
            # These files belong to this generated profile, never the example
            # source or a user's firmware build. Reconfigure from declared inputs.
            content = "".join(f"{key}={value}\n" for key, value in profile["sdkconfig"].items())
            (build / "sdkconfig").write_text(content, encoding="utf-8")
        selected = cross if profile.get("target") else environment
        commands = [configure_command(root, build, profile, environment, cmake, ninja)]
        if profile.get("buildTargets"):
            commands.append([cmake, "--build", str(build), "--target", *profile["buildTargets"], "--parallel", "4"])
        print("Preparing quality profile: " + profile["name"], flush=True)
        with (build / "quality-configure.log").open("w", encoding="utf-8") as output:
            for command in commands:
                result = subprocess.run(command, cwd=root, env=selected, stdout=output, stderr=subprocess.STDOUT)
                if result.returncode:
                    output.flush()
                    diagnostic = (build / "quality-configure.log").read_text(encoding="utf-8", errors="replace")
                    print("\n".join(diagnostic.splitlines()[-80:]), file=sys.stderr)
                    raise ValueError(f"Profile {profile['name']} failed; inspect {build / 'quality-configure.log'}")
        if not (build / "compile_commands.json").is_file():
            raise ValueError("Compile database was not produced: " + str(build))
        paths.append(build.relative_to(root).as_posix())
    configuration = root / ".agent-workspace/quality-contexts.json"
    configuration.parent.mkdir(exist_ok=True)
    contents = json.dumps({"context_build_paths": paths, "embedded_executable": environment["ESP_CLANG_TIDY"]}, indent=2) + "\n"
    temporary = configuration.with_suffix(".json.tmp")
    temporary.write_text(contents, encoding="utf-8")
    temporary.replace(configuration)
    print(f"Prepared {len(paths)} profiles for {root}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--build-root", type=Path)
    parser.add_argument("--profile", action="append", help="Reconfigure selected profiles; all others must already be prepared")
    args = parser.parse_args()
    root = args.source.resolve()
    build_root = (args.build_root or root / "cmake-build-dev/quality-contexts").resolve()
    if not build_root.is_relative_to(root / "cmake-build-dev"):
        parser.error("Quality profiles must stay inside this checkout's cmake-build-dev")
    try:
        prepare(root, build_root, set(args.profile) if args.profile else None)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("Quality profiles: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
