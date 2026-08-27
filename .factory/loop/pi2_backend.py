#!/usr/bin/env python3
"""Exact-commit Pi2 adapter executed inside the factory confinement boundary.

The control plane replaces the two immutable runtime markers while staging this
committed source.  Credentials are copied by the trusted parent into the
per-launch sanitized HOME; this adapter never reads the operator's real home.
"""

from __future__ import annotations

import os
from pathlib import Path
import sys

NODE_EXECUTABLE = "@@FACTORY_PI2_NODE@@"
PI_CLI = "@@FACTORY_PI2_CLI@@"


def main() -> None:
    home = Path(os.environ["HOME"])
    agent_dir = home / ".pi" / "agent2"
    if not agent_dir.is_dir():
        raise SystemExit("factory-pi2-backend: private agent directory is missing")
    node = Path(NODE_EXECUTABLE)
    cli = Path(PI_CLI)
    if not node.is_absolute() or not cli.is_absolute():
        raise SystemExit("factory-pi2-backend: runtime binding is not absolute")
    env = dict(os.environ)
    env["PI_CODING_AGENT_DIR"] = str(agent_dir)
    env["PI_PACKAGE_DIR"] = str(cli.parents[1])
    env["NODE_PATH"] = str(cli.parents[3])
    os.execve(str(node), [str(node), str(cli), *sys.argv[1:]], env)


if __name__ == "__main__":
    main()
