"""Run a generated build command with this checkout's local tool environment."""

import argparse
from pathlib import Path
import subprocess
import sys

from project_environment import activate


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("A build command is required")
    try:
        activate(args.source.resolve())
        return subprocess.run(command).returncode
    except (OSError, ValueError) as error:
        print("Project build environment: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
