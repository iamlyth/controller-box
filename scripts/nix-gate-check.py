#!/usr/bin/env python3
"""TOCTOU-free verification of the authenticated Nix-gate boundary capability.

The authenticated Nix gate (`scripts/nix-gate.sh` + `scripts/nix-gate-exec.sh`)
replaces the forgeable CBX_VERIFY_IN_NIX_SHELL / IN_NIX_SHELL environment-variable
trust with a wrapper-generated capability. The wrapper writes a fresh 64-hex nonce
into a mode-0600, current-user-owned, non-symlink regular file (nlink==1) and
carries it into `nix-shell`. This helper verifies that capability file with an
open-then-fstat / fstat-before-and-after protocol (O_NOFOLLOW so a symlink is
refused; owner/euid and exact mode checked; nlink==1 so a hardlink substitution
is refused; dev/ino/size/mtime compared before and after the read so a racing
replacement is refused).

The capability alone does NOT authorize anything: it only proves the current
process passed through the authenticated wrapper. The store-authority check in
`nix-gate.sh` (declared tools resolving under the immutable /nix/store) remains
the environment authenticator. A capability that fails any check is a
forge/expiry and is refused with exit 2.
"""
import os
import stat
import sys


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: nix-gate-check.py <nonce-file> <expected-nonce>\n")
        return 2
    path, expected = sys.argv[1], sys.argv[2]
    if sys.platform != "linux" or not hasattr(os, "O_NOFOLLOW"):
        sys.stderr.write("nix-gate-check: required Linux no-follow primitive is unavailable\n")
        return 2
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    except OSError:
        return 2
    try:
        before = os.fstat(descriptor)
        named = os.lstat(path)
        if (
            not stat.S_ISREG(before.st_mode)
            or before.st_uid != os.geteuid()
            or before.st_nlink != 1
            or (before.st_mode & 0o777) != 0o600
            or (before.st_dev, before.st_ino) != (named.st_dev, named.st_ino)
        ):
            return 2
        data = os.read(descriptor, 4096)
        after = os.fstat(descriptor)
        if (
            (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
            != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
        ):
            return 2
    finally:
        os.close(descriptor)
    content = data.decode("ascii", "replace").strip()
    return 0 if content == expected else 2


if __name__ == "__main__":
    sys.exit(main())
