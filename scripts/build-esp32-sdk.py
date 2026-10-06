"""Build the library with caller-provided ESP-IDF and compiler; no PlatformIO."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def build_environment(original, idf, toolchain, tools_cache):
    env = dict(original)
    env.pop("PYTHONPATH", None)
    env.pop("PYTHONHOME", None)
    env["IDF_PATH"] = str(idf.resolve())
    env["ESP32_TOOLCHAIN_DIR"] = str(toolchain.resolve())
    env["IDF_COMPONENT_MANAGER"] = "0"
    if tools_cache:
        env["IDF_TOOLS_PATH"] = str(tools_cache.resolve() / "idf-tools")
        env["IDF_PYTHON_ENV_PATH"] = str(tools_cache.resolve() / "python")
    env["PATH"] = str(toolchain.resolve() / "bin") + os.pathsep + env.get("PATH", "")
    return env


def run(command, env):
    print("+ " + subprocess.list2cmdline([str(arg) for arg in command]), flush=True)
    subprocess.run([str(arg) for arg in command], env=env, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, default=os.getenv("IDF_PATH"))
    parser.add_argument("--toolchain-dir", type=Path, default=os.getenv("ESP32_TOOLCHAIN_DIR"))
    parser.add_argument("--prepare-python", action="store_true", help="Prepare only isolated ESP-IDF Python dependencies")
    parser.add_argument("--tools-cache", type=Path)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-sdk-esp32")
    parser.add_argument("--fresh", action="store_true", help="Reset CMake configuration after changing compiler or SDK")
    parser.add_argument("--fft-capacity", choices=[512, 1024, 2048, 4096], type=int, default=4096)
    parser.add_argument("--backend", choices=["esp-dsp", "ansi"], default="esp-dsp")
    parser.add_argument("--cmake", default=shutil.which("cmake"))
    parser.add_argument("--ninja", default=shutil.which("ninja"))
    parser.add_argument("--bdsp-source", type=Path)
    args = parser.parse_args()
    if not args.idf_path or not args.toolchain_dir:
        parser.error("Set IDF_PATH and ESP32_TOOLCHAIN_DIR or supply --idf-path and --toolchain-dir")
    if not (args.idf_path / "tools/idf_tools.py").is_file() or not (args.toolchain_dir / "bin").is_dir():
        parser.error("Existing IDF source tree and compiler root containing bin are required")
    if not args.cmake or not args.ninja:
        parser.error("CMake >=3.30 and Ninja are required")
    env = build_environment(os.environ, args.idf_path, args.toolchain_dir, args.tools_cache)
    if args.prepare_python:
        if not args.tools_cache:
            parser.error("--prepare-python requires --tools-cache for isolation")
        Path(env["IDF_PYTHON_ENV_PATH"]).mkdir(parents=True, exist_ok=True)
        run([sys.executable, args.idf_path / "tools/idf_tools.py", "install-python-env"], env)
    python = sys.executable
    if env.get("IDF_PYTHON_ENV_PATH"):
        python = str(Path(env["IDF_PYTHON_ENV_PATH"]) / ("Scripts/python.exe" if os.name == "nt" else "bin/python"))
    command = [args.cmake, "-S", ROOT / "adapters/esp_idf/library", "-B", args.build_dir.resolve(), "-G", "Ninja",
               f"-DCMAKE_MAKE_PROGRAM={Path(args.ninja).resolve().as_posix()}",
               f"-DESP_IDF_SOURCE_DIR={args.idf_path.resolve().as_posix()}",
               f"-DESP32_TOOLCHAIN_DIR={args.toolchain_dir.resolve().as_posix()}",
               f"-DPYTHON={Path(python).as_posix()}",
               f"-DPORTABLE_SDK_MAX_FFT_SIZE={args.fft_capacity}",
               f"-DPORTABLE_SDK_ESP_DSP={'ON' if args.backend == 'esp-dsp' else 'OFF'}"]
    if args.bdsp_source:
        command.append(f"-DBDSP_SOURCE_DIR={args.bdsp_source.resolve().as_posix()}")
    if args.fresh:
        command.append("--fresh")
    run(command, env)
    run([args.cmake, "--build", args.build_dir.resolve(), "--target", "esp32_sdk", "--parallel", "4"], env)
    run([args.cmake, "--install", args.build_dir.resolve(), "--prefix", args.build_dir.resolve() / "prefix"], env)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
