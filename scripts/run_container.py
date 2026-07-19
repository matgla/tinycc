#!/usr/bin/env python3
"""
Run a command (or an interactive shell) inside the tinycc-armv8m container.

The working tree is bind-mounted at /workspace and bash history is persisted
under $XDG_STATE_HOME so it survives across --rm runs.

Usage:
  scripts/run_container.py -v 0.1.0 -i              # interactive shell
  scripts/run_container.py -v 0.1.0 -c "make cross" # one-shot command

Env knobs:
  CONTAINER_RUNTIME     podman (default) or docker
  CONTAINER_REGISTRY    default: ghcr.io
  CONTAINER_REPOSITORY  default: matgla/tinycc-armv8m
"""

import argparse
import os
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-c", "--command", default="",
                    help="command to run inside the container via bash -lc")
    ap.add_argument("-v", "--container_version", default="",
                    help="image tag to run (required)")
    ap.add_argument("-i", "--interactive", action="store_true",
                    help="allocate a tty and drop into a shell")
    args = ap.parse_args()

    if not args.interactive and not args.command:
        print("Error: pass --interactive or --command", file=sys.stderr)
        return 1
    if not args.container_version:
        print("Error: --container_version is required", file=sys.stderr)
        return 1

    runtime = os.environ.get("CONTAINER_RUNTIME", "podman")
    registry = os.environ.get("CONTAINER_REGISTRY", "ghcr.io")
    repository = os.environ.get("CONTAINER_REPOSITORY", "matgla/tinycc-armv8m")
    image = f"{registry}/{repository}:{args.container_version}"

    state_home = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state"))
    history_dir = state_home / "tinycc-armv8m-container"
    history_file = history_dir / "bash_history"
    history_dir.mkdir(parents=True, exist_ok=True)
    history_file.touch(exist_ok=True)

    run_args = [
        "run", "--rm",
        "-v", f"{Path.cwd()}:/workspace",
        "-v", f"{history_file}:/root/.bash_history",
        "-e", "HISTFILE=/root/.bash_history",
        "-w", "/workspace",
    ]
    if runtime == "podman":
        run_args.append("--userns=keep-id")
    if args.interactive:
        run_args.append("-it")
    run_args.append(image)
    run_args += ["/bin/bash", "-lc", args.command] if args.command else ["/bin/bash"]

    try:
        os.execvp(runtime, [runtime] + run_args)
    except FileNotFoundError:
        print(f"Error: container runtime not found: {runtime}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
