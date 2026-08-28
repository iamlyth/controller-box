#!/usr/bin/env python3
"""Exact-commit Pi2 adapter executed inside the factory confinement boundary.

The control plane replaces the two immutable runtime markers while staging this
committed source.  The operator credential is never materialised in any
model/tool-readable path (B1 security review): the trusted parent carries the
``auth.json`` bytes in one anonymous memfd whose descriptor number arrives in
this adapter's transient argv (``--auth-fd N``).  This adapter creates the
per-launch agent directory symlink ``auth.json -> /proc/self/fd/N`` so the
model CLI reads/writes the credential only through the inherited descriptor;
the exact-commit extension closes the descriptor synchronously at Pi's common
``tool_call`` boundary before any enabled in-process or subprocess tool runs,
so no model tool can dereference it. The descriptor number is consumed here
and is absent from the model process argv after exec.
"""

from __future__ import annotations

import os
from pathlib import Path
import resource
import stat
import sys

NODE_EXECUTABLE = "@@FACTORY_PI2_NODE@@"
PI_CLI = "@@FACTORY_PI2_CLI@@"


def _parse_auth_fd(argv: list) -> tuple:
    """Extract ``--auth-fd N`` from the adapter argv (fail closed)."""
    auth_fd = -1
    remaining: list = []
    index = 0
    while index < len(argv):
        token = argv[index]
        if token == "--auth-fd":
            if auth_fd >= 0:
                raise SystemExit("factory-pi2-backend: --auth-fd was supplied twice")
            if index + 1 >= len(argv):
                raise SystemExit("factory-pi2-backend: --auth-fd requires a value")
            try:
                auth_fd = int(argv[index + 1])
            except ValueError:
                raise SystemExit("factory-pi2-backend: --auth-fd is not an integer")
            if auth_fd < 0:
                raise SystemExit("factory-pi2-backend: --auth-fd is negative")
            index += 2
            continue
        remaining.append(token)
        index += 1
    return auth_fd, remaining


def _link_auth_descriptor(agent_dir: Path, auth_fd: int) -> None:
    """Bind ``auth.json`` to the inherited credential descriptor.

    The symlink target is the descriptor path of *this* process; a tool
    subprocess that dereferences the same path resolves its own (closed)
    descriptor slot and fails, and Landlock denies ``/proc`` entirely, so the
    credential is unreachable from any model tool.  The descriptor is left
    non-CLOEXEC so the adapter->node exec keeps it; Node's spawn closes it
    for every tool subprocess.
    """
    if auth_fd < 0:
        raise SystemExit("factory-pi2-backend: no credential descriptor was supplied")
    try:
        info = os.fstat(auth_fd)
    except OSError as exc:
        raise SystemExit(
            f"factory-pi2-backend: cannot inspect the credential descriptor: {exc}"
        )
    if not stat.S_ISREG(info.st_mode):
        raise SystemExit("factory-pi2-backend: credential descriptor is not regular")
    target = agent_dir / "auth.json"
    if target.is_symlink() or target.exists():
        raise SystemExit("factory-pi2-backend: auth.json already exists in the agent dir")
    try:
        os.symlink(f"/proc/self/fd/{auth_fd}", target)
    except OSError as exc:
        raise SystemExit(
            f"factory-pi2-backend: cannot bind the credential descriptor: {exc}"
        )


def main() -> None:
    home = Path(os.environ["HOME"])
    agent_dir = home / ".pi" / "agent2"
    if not agent_dir.is_dir():
        raise SystemExit("factory-pi2-backend: private agent directory is missing")
    node = Path(NODE_EXECUTABLE)
    cli = Path(PI_CLI)
    if not node.is_absolute() or not cli.is_absolute():
        raise SystemExit("factory-pi2-backend: runtime binding is not absolute")
    auth_fd, remaining = _parse_auth_fd(sys.argv[1:])
    _link_auth_descriptor(agent_dir, auth_fd)
    env = dict(os.environ)
    env["PI_CODING_AGENT_DIR"] = str(agent_dir)
    env["PI_PACKAGE_DIR"] = str(cli.parents[1])
    env["NODE_PATH"] = str(cli.parents[3])
    # Non-secret identity metadata tells the exact-commit extension which one
    # inherited descriptor to close at the common tool boundary. The extension
    # fstats and matches all three values before close; malformed/missing or
    # reused descriptor identity blocks the tool without touching another fd.
    identity = os.fstat(auth_fd)
    # Bound the exact Node/tool process descriptor table so the extension can
    # synchronously inspect every possible numeric alias before a tool runs.
    # 4096 is ample for Pi/build tooling while making all-alias closure finite;
    # preserve an already-lower operator hard/soft limit.
    soft_limit, hard_limit = resource.getrlimit(resource.RLIMIT_NOFILE)
    finite_soft = 4096 if soft_limit == resource.RLIM_INFINITY else soft_limit
    finite_hard = 4096 if hard_limit == resource.RLIM_INFINITY else hard_limit
    fd_limit = min(4096, finite_soft, finite_hard)
    if fd_limit <= auth_fd or fd_limit < 64:
        raise SystemExit("factory-pi2-backend: descriptor limit cannot bound auth aliases")
    resource.setrlimit(resource.RLIMIT_NOFILE, (fd_limit, hard_limit))
    env["PI_FACTORY_TOOL_FD"] = str(auth_fd)
    env["PI_FACTORY_TOOL_FD_DEV"] = str(identity.st_dev)
    env["PI_FACTORY_TOOL_FD_INO"] = str(identity.st_ino)
    env["PI_FACTORY_TOOL_FD_LIMIT"] = str(fd_limit)
    os.execve(str(node), [str(node), str(cli), *remaining], env)


if __name__ == "__main__":
    main()
