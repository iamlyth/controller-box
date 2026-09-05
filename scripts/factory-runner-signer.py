#!/usr/bin/env python3
"""Root-owned, class-bound detached signer for factory runner receipts.

Production authorization is derived from sudo's numeric caller UID and the
root-owned runner policy.  Signer state and the pinned ssh-keygen executable
are opened without following symlinks and used through inherited descriptors,
so pathname replacement after validation cannot change the key, principal, or
executable that is used.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import stat
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from factory_runner_policy import PolicyError, class_for_name, class_for_uid, load_policy, policy_path

SHA1 = re.compile(r"^[0-9a-f]{40}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
NAME = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
ALGORITHM = "ssh-ed25519"
MAX_REQUEST = 1024 * 1024
MAX_SIGNATURE = 64 * 1024
# This is deliberately an installation-platform path, not a PATH lookup.  The
# deployment preflight verifies the same executable and complete ancestor
# chain before installing this helper.
SSH_KEYGEN_PATH = "/usr/bin/ssh-keygen"
ROOT_UID = 0
STATE_CHAIN_BOUNDARY = Path("/")
VALIDATE_EXECUTABLE_CHAIN = True


def fail(message: str) -> None:
    sys.stderr.write(f"factory-runner-signer: {message}\n")
    sys.stderr.flush()
    raise SystemExit(1)


def _validate_chain(path: Path, *, leaf_regular: bool, private_leaf: bool,
                    boundary: Path = Path("/"), executable: bool = False) -> None:
    """Validate every lexical path component without following symlinks."""
    if not path.is_absolute() or not boundary.is_absolute():
        fail("trusted signer paths must be absolute")
    try:
        relative = path.relative_to(boundary)
    except ValueError:
        fail(f"trusted path is outside its validation boundary: {path}")
    current = boundary
    components = [boundary, *(boundary / Path(*relative.parts[:i]) for i in range(1, len(relative.parts) + 1))]
    for index, current in enumerate(components):
        try:
            info = current.lstat()
        except OSError as exc:
            fail(f"signer state unavailable at {current}: {type(exc).__name__}")
        if stat.S_ISLNK(info.st_mode):
            fail(f"refusing a symlink path component: {current}")
        if info.st_uid != ROOT_UID:
            fail(f"trusted path component is not root-owned: {current}")
        is_leaf = index == len(components) - 1
        if info.st_mode & 0o022:
            fail(f"trusted path component is group/other-writable: {current}")
        if not is_leaf and not stat.S_ISDIR(info.st_mode):
            fail(f"trusted path ancestor is not a directory: {current}")
        if is_leaf:
            if leaf_regular and not stat.S_ISREG(info.st_mode):
                fail(f"trusted signer leaf is not a regular file: {current}")
            if private_leaf and info.st_mode & 0o077:
                fail(f"private signer state must be root-owned mode 0600: {current}")
            if executable and not info.st_mode & 0o111:
                fail(f"trusted signer executable is not executable: {current}")


def _open_bound(path: Path, *, private: bool) -> int:
    """Open a validated state file with directory-fd/no-follow binding."""
    _validate_chain(path, leaf_regular=True, private_leaf=private,
                    boundary=STATE_CHAIN_BOUNDARY)
    relative = path.relative_to(STATE_CHAIN_BOUNDARY)
    directory_fd = os.open(STATE_CHAIN_BOUNDARY, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        for component in relative.parts[:-1]:
            next_fd = os.open(
                component,
                os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
                dir_fd=directory_fd,
            )
            os.close(directory_fd)
            directory_fd = next_fd
        fd = os.open(relative.parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC,
                     dir_fd=directory_fd)
    except OSError as exc:
        fail(f"cannot bind signer state {path}: {type(exc).__name__}")
    finally:
        os.close(directory_fd)
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != ROOT_UID or (private and info.st_mode & 0o077):
        os.close(fd)
        fail(f"signer state changed while opening: {path}")
    return fd


def _open_executable() -> int:
    path = Path(SSH_KEYGEN_PATH)
    if VALIDATE_EXECUTABLE_CHAIN:
        _validate_chain(path, leaf_regular=True, private_leaf=False,
                        boundary=Path("/"), executable=True)
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    except OSError as exc:
        fail(f"trusted ssh-keygen is unavailable: {type(exc).__name__}")
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or not info.st_mode & 0o111:
        os.close(fd)
        fail("trusted ssh-keygen changed while opening")
    if VALIDATE_EXECUTABLE_CHAIN and info.st_uid != ROOT_UID:
        os.close(fd)
        fail("trusted ssh-keygen is not root-owned")
    return fd


def _run_keygen(executable_fd: int, args: list[str], *, key_fd: int,
                input_bytes: bytes | None = None, timeout: int = 30) -> subprocess.CompletedProcess:
    executable = f"/proc/self/fd/{executable_fd}"
    key = f"/proc/self/fd/{key_fd}"
    return subprocess.run(
        [executable, *[key if item == "{key}" else item for item in args]],
        executable=executable, input=input_bytes, capture_output=True,
        pass_fds=(executable_fd, key_fd), timeout=timeout,
    )


def _validate_ed25519_public(public: bytes) -> str:
    try:
        text = public.decode("ascii").rstrip("\n")
    except UnicodeDecodeError:
        fail("signer public key is invalid")
    fields = text.split()
    if len(fields) < 2:
        fail("signer public key is invalid")
    # ssh-keygen -y may append the private-key comment. Only its canonical
    # algorithm/blob pair is identity-bearing and enters repository trust.
    algorithm, encoded = fields[:2]
    text = f"{algorithm} {encoded}"
    if algorithm != ALGORITHM:
        fail("signer key type must be ssh-ed25519")
    try:
        wire = base64.b64decode(encoded, validate=True)
    except Exception:
        fail("signer public key encoding is invalid")
    if base64.b64encode(wire).decode("ascii") != encoded:
        fail("signer public key encoding is not canonical")
    try:
        kind_len = struct.unpack(">I", wire[:4])[0]
        offset = 4
        kind = wire[offset:offset + kind_len]
        offset += kind_len
        key_len = struct.unpack(">I", wire[offset:offset + 4])[0]
        offset += 4
        key = wire[offset:offset + key_len]
        offset += key_len
    except (struct.error, ValueError):
        fail("signer public key wire encoding is invalid")
    if kind != b"ssh-ed25519" or key_len != 32 or len(key) != 32 or offset != len(wire):
        fail("signer public key wire encoding is invalid")
    return text


def resolve_signing_state() -> tuple[Path, Path, dict | None]:
    sudo_user = os.environ.get("SUDO_USER")
    sudo_uid_text = os.environ.get("SUDO_UID")
    if sudo_user is not None or sudo_uid_text is not None:
        if not sudo_user or sudo_uid_text is None or not NAME.fullmatch(sudo_user):
            fail("sudo caller identity is incomplete or invalid")
        try:
            sudo_uid = int(sudo_uid_text, 10)
        except ValueError:
            fail("sudo uid is invalid")
        if sudo_uid <= 0:
            fail("sudo caller uid is invalid")
        try:
            account_by_uid = pwd.getpwuid(sudo_uid)
            account_by_name = pwd.getpwnam(sudo_user)
        except KeyError:
            fail("sudo caller account does not exist")
        if account_by_uid.pw_name != sudo_user or account_by_name.pw_uid != sudo_uid:
            fail("sudo caller name/uid account lookup is inconsistent")
        try:
            _validate_chain(policy_path(), leaf_regular=True, private_leaf=False, boundary=Path("/"))
            policy = load_policy()
            runner_class = class_for_uid(policy, sudo_uid)
        except PolicyError as exc:
            fail(f"signer class binding failed: {exc}")
        return Path(runner_class["signer_key"]), Path(runner_class["signer_principal_file"]), runner_class

    key_override = os.environ.get("FACTORY_SIGNER_KEY")
    principal_override = os.environ.get("FACTORY_SIGNER_PRINCIPAL_FILE")
    if not key_override or not principal_override:
        fail("signer must run via sudo or with explicit harness overrides")
    runner_class = None
    class_override = os.environ.get("FACTORY_SIGNER_CLASS")
    if class_override:
        try:
            runner_class = class_for_name(load_policy(), class_override)
        except PolicyError as exc:
            fail(f"runner policy is unavailable: {exc}")
    return Path(key_override), Path(principal_override), runner_class


def _load_principal(fd: int) -> str:
    try:
        raw = os.pread(fd, 4097, 0)
    except OSError as exc:
        fail(f"cannot read signer principal: {type(exc).__name__}")
    if len(raw) > 4096:
        fail("signer principal is too large")
    try:
        principal = raw.decode("utf-8")
    except UnicodeError:
        fail("signer principal is invalid")
    if not principal.endswith("\n") or principal.count("\n") != 1:
        fail("signer principal file is not canonical")
    principal = principal[:-1]
    if not NAME.fullmatch(principal):
        fail("signer principal is invalid")
    return principal


def validate_manifest(raw: bytes, runner_class: dict | None) -> dict:
    if len(raw) > MAX_REQUEST or not raw.endswith(b"\n"):
        fail("invalid signing request")
    try:
        request = json.loads(raw)
    except (UnicodeError, json.JSONDecodeError):
        fail("signing request is not valid JSON")
    if not isinstance(request, dict) or set(request) != {"schema", "manifest"} or request.get("schema") != "factory-runner-sign-request/v1":
        fail("signing request schema is invalid")
    manifest = request["manifest"]
    fields = {"schema", "result", "runner", "commit", "tree", "environment_blob", "verify_argv_sha256", "archive_sha256", "nonce", "capabilities", "exit_code", "timed_out", "started_at", "finished_at", "cleanup", "stdout_sha256", "stderr_sha256"}
    if not isinstance(manifest, dict) or set(manifest) != fields:
        fail("signing request manifest fields are invalid")
    if manifest.get("schema") != "factory-runner-receipt/v1" or manifest.get("result") != "pass":
        fail("only passing runner receipts can be signed")
    if not isinstance(manifest["runner"], str) or not NAME.fullmatch(manifest["runner"]):
        fail("signing request runner is invalid")
    if runner_class is not None and manifest["runner"] != runner_class["name"]:
        fail("signing request runner does not match the runner class")
    for field in ("commit", "tree", "environment_blob"):
        if not isinstance(manifest[field], str) or not SHA1.fullmatch(manifest[field]):
            fail(f"signing request {field} is invalid")
    for field in ("verify_argv_sha256", "archive_sha256", "nonce", "stdout_sha256", "stderr_sha256"):
        if not isinstance(manifest[field], str) or not SHA256.fullmatch(manifest[field]):
            fail(f"signing request {field} is invalid")
    if manifest["exit_code"] != 0 or manifest["timed_out"] is not False or manifest["cleanup"] is not True:
        fail("signing request does not prove a clean pass")
    if type(manifest["started_at"]) is not int or type(manifest["finished_at"]) is not int or manifest["started_at"] < 0 or manifest["finished_at"] < manifest["started_at"]:
        fail("signing request timestamps are invalid")
    capabilities = manifest["capabilities"]
    if not isinstance(capabilities, list) or not capabilities or len(capabilities) != len(set(capabilities)) or not all(isinstance(item, str) and NAME.fullmatch(item) for item in capabilities):
        fail("signing request capabilities are invalid")
    if runner_class is not None and sorted(capabilities) != sorted(runner_class["allowed_capabilities"]):
        fail("signing request capabilities do not equal the runner class allowlist")
    return manifest


def main() -> int:
    if os.getuid() != os.geteuid() or os.geteuid() != ROOT_UID:
        fail("signer must run as root")
    key_path, principal_path, runner_class = resolve_signing_state()
    key_fd = _open_bound(key_path, private=True)
    principal_fd = _open_bound(principal_path, private=True)
    executable_fd = _open_executable()
    try:
        principal = _load_principal(principal_fd)
        if runner_class is not None and principal != runner_class["name"]:
            fail("signer principal does not match the runner class")
        namespace = load_policy()["namespace"] if runner_class is not None else os.environ.get("FACTORY_SIGNER_NAMESPACE", "factory-runner-receipt")
        derived = _run_keygen(executable_fd, ["-y", "-f", "{key}"], key_fd=key_fd)
        if derived.returncode != 0:
            fail("cannot derive the signer public key from the private key")
        public = _validate_ed25519_public(derived.stdout)
        key_sha256 = hashlib.sha256(public.encode("ascii")).hexdigest()
        evidence = validate_manifest(sys.stdin.buffer.read(MAX_REQUEST + 1), runner_class)
        manifest = dict(evidence)
        manifest.update({"signer_principal": principal, "signer_key_sha256": key_sha256, "namespace": namespace, "signature_algorithm": ALGORITHM})
        canonical = (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode()
        signed = _run_keygen(executable_fd, ["-Y", "sign", "-f", "{key}", "-n", namespace], key_fd=key_fd, input_bytes=canonical, timeout=120)
        if signed.returncode != 0:
            fail("signature generation failed")
        signature = signed.stdout
        if not signature or len(signature) > MAX_SIGNATURE or not signature.startswith(b"-----BEGIN SSH SIGNATURE-----"):
            fail("signature generation produced invalid output")
        response = {"schema": "factory-runner-sign-response/v1", "result": "signed", "manifest_b64": base64.b64encode(canonical).decode("ascii"), "signature_b64": base64.b64encode(signature).decode("ascii"), "signer_principal": principal, "signer_key_sha256": key_sha256, "signature_algorithm": ALGORITHM, "namespace": namespace, "signature_sha256": hashlib.sha256(signature).hexdigest()}
        sys.stdout.write(json.dumps(response, sort_keys=True, separators=(",", ":")) + "\n")
        return 0
    finally:
        os.close(key_fd)
        os.close(principal_fd)
        os.close(executable_fd)


if __name__ == "__main__":
    raise SystemExit(main())
