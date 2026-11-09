"""Resolve checkout-local tool paths without evaluating shell code."""

import argparse
import json
import os
from pathlib import Path
import re
from string import Template

VARIABLES = frozenset({"QT_ROOT", "CC", "CXX", "CMAKE_EXE", "NINJA_EXE",
    "CLAZY_STANDALONE", "IDF_PATH", "IDF_TOOLS_PATH", "IDF_PYTHON_ENV_PATH",
    "ESP_CLANG_TIDY", "ESP32_TOOLCHAIN_DIR", "GMF_FFT_SOURCE_DIR", "ESP_DSP_SOURCE_DIR", "BDSP_SOURCE_DIR"})


def resolve(root: Path, environment: dict | None = None) -> dict[str, str]:
    environment = dict(os.environ if environment is None else environment)
    path = root / ".env.local"
    if not path.is_file():
        return {}
    result, seen = {}, set()
    for number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        key, value = key.strip(), value.strip()
        if not separator or key not in VARIABLES:
            raise ValueError(f"Unsupported project tool variable on line {number}: {key}")
        if key in seen:
            raise ValueError("Duplicate project tool variable: " + key)
        seen.add(key)
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        value = environment.get(key) or value
        if not value:
            continue
        if re.search(r"\$\(|`|[\r\n\0]", value):
            raise ValueError("Shell expressions are not allowed for " + key)
        try:
            value = Template(value).substitute({**environment, **result})
        except (KeyError, ValueError) as error:
            raise ValueError("Unresolved project tool path: " + key) from error
        result[key] = str((root / Path(value)).resolve())
    return result


def activate(root: Path) -> dict[str, str]:
    values = resolve(root)
    os.environ.update(values)
    if os.name == "nt":
        directories = dict.fromkeys(str(Path(os.environ[key]).parent) for key in ("CC", "CXX", "CMAKE_EXE", "NINJA_EXE") if os.environ.get(key))
        os.environ["PATH"] = os.pathsep.join([*directories, os.environ.get("PATH", "")])
    return values


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(resolve(args.source.resolve())))
    except (ValueError, OSError) as error:
        parser.exit(1, f"Project environment: {error}\n")


if __name__ == "__main__":
    main()
