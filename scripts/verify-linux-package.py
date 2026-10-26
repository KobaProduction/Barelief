"""Verify the extracted DEB runtime on an explicitly isolated Xvfb display."""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import shutil
import subprocess
import tempfile
import time


def command(arguments, environment, **options):
    return subprocess.run(arguments, env=environment, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **options).stdout


def check_libraries(root, environment):
    applications = [root / "bin" / name for name in
                    ("Barelief", "pipeline_cli", "aggregator_server_app", "aggregator_client_app")]
    for application in applications:
        if not application.is_file():
            raise RuntimeError(f"Missing packaged application: {application}")
    binaries = {path.resolve() for path in [*applications, *root.rglob("*.so*")] if path.is_file()}
    for binary in sorted(binaries):
        output = command(["ldd", str(binary)], environment, timeout=20)
        if "not found" in output:
            raise RuntimeError(f"Unresolved dependencies of {binary}:\n{output}")
        for library in re.findall(r"libQt6\S+\s+=>\s+(\S+)", output):
            if not Path(library).resolve().is_relative_to(root):
                raise RuntimeError(f"Qt dependency escaped the package: {library}")
    return len(binaries)


def wait_for_server(process, port):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("Packaged synthetic engine exited before listening")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=.2):
                return
        except OSError:
            time.sleep(.05)
    raise RuntimeError("Packaged synthetic engine did not start listening")


def check_gui(root, environment, temporary, evidence):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    config = temporary / "gui.json"
    shutil.copyfile(Path(__file__).resolve().parents[1] / "tests/fixtures/linux-package.json", config)
    trace = evidence / "installed-gui-trace.json"
    trace.unlink(missing_ok=True)
    with (evidence / "installed-engine.log").open("w") as engine_log:
        engine = subprocess.Popen([str(root / "bin/pipeline_cli"), "--synthetic", "--server",
                                   "--allow-mutations", "--config", str(config),
                                   "--port", str(port), "--duration", "40000"],
                                  env=environment, cwd=temporary, stdout=engine_log,
                                  stderr=subprocess.STDOUT)
        try:
            wait_for_server(engine, port)
            gui_environment = dict(environment, QT_QPA_PLATFORM="xcb", LIBGL_ALWAYS_SOFTWARE="1",
                                   BARELIEF_PACKAGE_PROBE_FILE=str(trace))
            with (evidence / "installed-gui.log").open("w") as gui_log:
                subprocess.run([str(root / "bin/Barelief"), "--config", str(config),
                                "--remote-host", "127.0.0.1", "--remote-port", str(port)],
                               env=gui_environment, cwd=temporary, timeout=35, check=True,
                               stdout=gui_log, stderr=subprocess.STDOUT)
            output = (evidence / "installed-gui.log").read_text()
            if re.search(r"module .* is not installed|Error loading QML|failed to load component", output):
                raise RuntimeError("Packaged GUI could not load its QML dependencies")
            if not trace.is_file():
                raise RuntimeError("Packaged GUI did not produce its delivery report")
            report = json.loads(trace.read_text())
            if report.get("schema") != 1 or report.get("kind") != "packaged_gui_delivery":
                raise RuntimeError("Packaged GUI produced an unsupported delivery report")
            delivery = {key: report[key] for key in
                        ("numeric_frames_delivered", "history_rows_delivered", "history_snapshots_delivered")}
            if not report["sample_started"] or not any(value > 0 for value in delivery.values()):
                raise RuntimeError("Packaged GUI did not receive synthetic engine frames")
            return delivery
        finally:
            if engine.poll() is None:
                engine.terminate()
                try:
                    engine.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    engine.kill()
                    engine.wait(timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    args = parser.parse_args()
    display = re.fullmatch(r":(\d+)(?:\.\d+)?", os.environ.get("DISPLAY", ""))
    if os.environ.get("BARELIEF_TEST_VIRTUAL_DISPLAY") != "1" or not display or int(display[1]) < 99:
        parser.error("Run inside xvfb-run --auto-servernum with BARELIEF_TEST_VIRTUAL_DISPLAY=1")
    evidence = args.evidence_dir.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("QT_", "QML", "LD_", "CORE_APP_TRACE", "BARELIEF_PACKAGE_PROBE"))}
    with tempfile.TemporaryDirectory(prefix="barelief-package-") as directory:
        temporary = Path(directory)
        command(["dpkg-deb", "--extract", str(args.package.resolve()), str(temporary)], environment)
        root = (temporary / "opt/barelief").resolve()
        runtime = temporary / "runtime"
        runtime.mkdir(mode=0o700)
        environment["XDG_RUNTIME_DIR"] = str(runtime)
        libraries = check_libraries(root, environment)
        version = command([str(root / "bin/Barelief"), "--version"],
                          dict(environment, QT_QPA_PLATFORM="offscreen"), timeout=5).strip()
        frames = check_gui(root, environment, temporary, evidence)
        report = {"package": args.package.name, "version": version,
                  "elf_files_checked": libraries, "gui_delivery": frames,
                  "scope": "Extracted DEB, private Xvfb and synthetic engine; no native hardware/FPS claim"}
        (evidence / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
