#!/usr/bin/env python3
"""Record a machine audit receipt for one coordinator-executed command.

An audit coordinator may only certify runtime behavior by executing a command
through this wrapper (or by referencing an accepted runner manifest). The
receipt binds the exact argv, the command's exit code, and SHA-256 digests of
the bounded stdout/stderr transcript under `.factory-state/audit-receipts/`.
`scripts/check-audit-receipts.py` requires a matching receipt (exit 0 for PASS)
for every executable-evidence line in the campaign audit report; subagent prose
cannot certify runtime.

Authorization (receipt minting is coordinator-bounded):
- a bare model call with no campaign binding fails;
- inside a campaign audit the coordinator exports the protected launch
  binding: `FACTORY_CAMPAIGN_AUDIT_ROUND`, `FACTORY_CAMPAIGN_AUDIT_BASE`, and
  `FACTORY_CAMPAIGN_AUDIT_NONCE`. The nonce is minted into the protected
  `.factory-state/audit-coordinator.json` state by the audit coordinator
  (`scripts/initialize-campaign-audit.py`) and never by the model;
- the receipt records `evidence_commit` equal to the campaign audit base
  (strict 40-hex) plus the coordinator round/nonce binding, so stale or
  cross-round receipts are rejected by the campaign-audit gate.

Usage:
  scripts/machine-receipt.py --tag <tag> -- <argv...>
  (campaign bound: FACTORY_CAMPAIGN_AUDIT_ROUND/BASE/NONCE must be exported)
  tests may pass --audit-round/--evidence-commit/--nonce explicitly together
  with a matching `.factory-state/audit-coordinator.json` fixture.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import importlib.util
import json
import os
import re
import resource
import select
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TAG = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
SHA1 = re.compile(r"^[0-9a-f]{40}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
MAX_LOG = 4 * 1024 * 1024
_EXEC_AUTHORIZATION_BYTES = 32
COORDINATOR_FILE = ".factory-state/audit-coordinator.json"
# The wrapper runs against a target repository it must never mutate: bytecode
# caching is disabled so importing the trusted loop authority (``lock.py``
# and its imports) can never create a ``__pycache__``/``*.pyc`` artifact in
# the target tree (a clean-tree checker would otherwise fail on the untracked
# bytecode files).
sys.dont_write_bytecode = True


def fail(message: str) -> None:
    raise SystemExit(f"machine-receipt: {message}")


def _private_directory(path: Path, what: str) -> None:
    """Require an exact current-user-owned mode-0700 real directory."""
    try:
        info = path.lstat()
    except OSError as exc:
        fail(f"cannot validate {what}: {exc}")
    if (not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode)
            or info.st_uid != os.getuid()
            or stat.S_IMODE(info.st_mode) != 0o700):
        fail(f"{what} must be a current-user-owned exact mode-0700 real directory")


def receipts_dir(root: Path) -> Path:
    runtime = root / ".factory-state"
    # Never chmod-repair runtime authority. Unsafe metadata is evidence of an
    # unsafe boundary and minting fails without changing it.
    _private_directory(runtime, ".factory-state")
    receipts = runtime / "audit-receipts"
    if not receipts.exists():
        try:
            receipts.mkdir(mode=0o700)
        except OSError as exc:
            fail(f"cannot create audit-receipts directory: {exc}")
    _private_directory(receipts, "audit-receipts")
    return receipts


def atomic_write(path: Path, data: bytes) -> None:
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o600)
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def verify_artifact(path: Path, what: str) -> None:
    """Harden one published receipt artifact (owner/mode/link-count/inode).

    The adjacent stdout/stderr transcripts and the receipt JSON must be
    regular single-link current-user-owned files with mode exactly 0600;
    a symlink, hardlink alias, foreign owner, read-only/permissive/executable
    mode, or inode substitution fails closed so a substituted artifact can
    never certify runtime.
    """
    if path.is_symlink() or not path.is_file():
        fail(f"{what} is not a regular file: {path}")
    try:
        info = path.stat()
    except OSError as exc:
        fail(f"cannot stat {what}: {path}: {type(exc).__name__}")
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_uid != os.getuid()
        or info.st_nlink != 1
        or stat.S_IMODE(info.st_mode) != 0o600
    ):
        fail(f"{what} is unsafe (owner/mode/link-count/inode): {path}")


def atomic_write_noreplace(path: Path, data: bytes) -> None:
    """Publish one receipt artifact with atomic no-replace semantics.

    Task 12 §19: same-tag coordinator receipt publication must fail closed
    rather than silently replace an existing receipt (Task 10 residual): an
    existing canonical artifact, or a raced pathname, can never be
    overwritten.  Publication uses ``linkat``-style ``os.link`` (unlike
    ``rename`` it cannot clobber), then the published inode is re-validated
    and the temporary unlinked.
    """
    if path.is_symlink() or path.exists():
        fail(f"receipt artifact already exists; same-tag publication fails "
             f"closed (no-replace): {path.name}")
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary_path = Path(temporary)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o600)
        try:
            os.link(temporary, str(path))
        except FileExistsError:
            fail(f"receipt artifact raced during publication: {path.name}")
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        verify_artifact(path, f"receipt artifact {path.name}")
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def regular_json(path: Path, maximum: int) -> dict:
    if path.is_symlink() or not path.is_file():
        fail(f"unsafe or missing coordinator state: {path}")
    if path.stat().st_size > maximum:
        fail(f"coordinator state exceeds the size limit: {path}")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        fail(f"invalid coordinator state {path}: {exc}")
    if not isinstance(data, dict):
        fail(f"coordinator state must be an object: {path}")
    return data


def _load_evidence_authority(root: Path):
    """Load the canonical active-coordinator authority from installed/source bytes."""
    candidates = [
        Path(__file__).resolve().parent.parent / ".factory" / "loop",
        Path(root).absolute() / ".factory" / "loop",
    ]
    for index, loop in enumerate(candidates):
        path = loop / "evidence.py"
        if not path.is_file() or path.is_symlink():
            continue
        previous = list(sys.path)
        try:
            sys.path.insert(0, str(loop))
            spec = importlib.util.spec_from_file_location(
                f"machine_receipt_evidence_{index}", path
            )
            if spec is None or spec.loader is None:
                continue
            module = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = module
            spec.loader.exec_module(module)
            if callable(getattr(module, "active_coordinator", None)):
                return module
        except Exception:
            sys.modules.pop(f"machine_receipt_evidence_{index}", None)
            continue
        finally:
            sys.path[:] = previous
    fail("canonical active-coordinator authority is unavailable")


def coordinator_binding(root: Path, round_number: int | None, evidence_commit: str | None,
                        nonce: str | None) -> tuple[int, str, str]:
    """Validate the audit coordinator binding; every receipt is round-bound.

    LOW5 (trusted coordinator-only mint path): minting is authorized **only**
    by the protected ``.factory-state/audit-coordinator.json`` state that the
    trusted audit coordinator mints (``scripts/initialize-campaign-audit.py``),
    never by environment variables alone.  The resolved round/base/nonce
    (from the env or explicit test binding) must match that protected state
    exactly; an auditor that inherits no coordinator binding, or that sets
    env keys against a missing/deleted state, can never mint a receipt.
    """
    env_round = os.environ.get("FACTORY_CAMPAIGN_AUDIT_ROUND", "")
    env_base = os.environ.get("FACTORY_CAMPAIGN_AUDIT_BASE", "")
    env_nonce = os.environ.get("FACTORY_CAMPAIGN_AUDIT_NONCE", "")
    if round_number is None:
        round_number = int(env_round) if env_round.isdigit() else None
    if evidence_commit is None:
        evidence_commit = env_base or None
    if nonce is None:
        nonce = env_nonce or None
    if (
        round_number is None or round_number < 1
        or not isinstance(evidence_commit, str) or not SHA1.fullmatch(evidence_commit)
        or not isinstance(nonce, str) or not SHA256.fullmatch(nonce)
    ):
        fail(
            "receipt minting is bound to the audit coordinator: "
            "FACTORY_CAMPAIGN_AUDIT_ROUND/BASE/NONCE (or explicit test binding) are required; "
            "a bare model receipt call is not authorized"
        )
    # Use the one canonical no-follow, retained-descriptor coordinator
    # parser. It requires exact mode 0700 on .factory-state and exact 0600 on
    # the regular single-link coordinator; 0400/0644 are rejected, never
    # repaired or independently reinterpreted here.
    try:
        data = _load_evidence_authority(root).active_coordinator(root)
    except Exception as exc:
        fail(f"audit coordinator state is missing or unsafe: {exc}")
    if data["round"] != round_number or data["base_commit"] != evidence_commit or data["nonce"] != nonce:
        fail("supplied audit binding does not match the protected coordinator state")
    return round_number, evidence_commit, nonce


def _load_lock_supervision(root: Path):
    """Load the trusted root-descriptor lock/supervision authority.

    The wrapper authority reuses the hidden control plane's supervised
    process boundary (``.factory/loop/lock.py``) for the descendant capture,
    escaped-descendant detection, and the live group-gone probe — the same
    trusted launch/lock primitives the campaign and launch supervision use.
    The authority is resolved from the wrapper's own installed/source tree
    first (the installed copy always carries the full hidden surface), then
    from the target repository root.  An unavailable authority fails closed
    instead of degrading the runner.
    """
    candidates = [
        Path(__file__).resolve().parent.parent / ".factory" / "loop",
        Path(root).absolute() / ".factory" / "loop",
    ]
    seen: set[str] = set()
    for loop in candidates:
        if str(loop) in seen or not (loop / "lock.py").is_file():
            continue
        seen.add(str(loop))
        if str(loop) not in sys.path:
            sys.path.insert(0, str(loop))
        try:
            import lock as lock_module  # noqa: PLC0415
        except Exception:  # ImportError and boundary failures alike
            continue
        return lock_module
    fail("trusted lock supervision is unavailable")
    return None


def _install_subreaper() -> None:
    """Install the *dedicated broker* as a child subreaper.

    ``run_bounded`` forks a fresh broker before calling this function.  That
    broker has no pre-existing children, so every child or adopted orphan in
    its lineage is command-owned.  The receipt coordinator itself is never a
    subreaper and therefore never has to guess whether a newly adopted child
    came from an unrelated pre-existing child that forked and exited.
    """
    import ctypes

    libc = ctypes.CDLL(None, use_errno=True)
    result = libc.prctl(36, 1, 0, 0, 0)  # PR_SET_CHILD_SUBREAPER == 36
    if result != 0:
        raise RuntimeError(
            "cannot install the dedicated child subreaper (prctl errno "
            f"{ctypes.get_errno()})"
        )


# Bounded receipt supervision.  The coordinator owns only one broker PID;
# the freshly forked broker owns only the command lineage.  No baseline-child
# subtraction exists anywhere in this boundary.  Numeric process-group ids
# are diagnostic data only and never seed ownership or receive a signal.
RUN_TIMEOUT = 7200.0
KILL_GRACE = 5.0
REAP_BOUND = 10.0
STABILIZE_WINDOW = 0.3
STABILIZE_BOUND = 15.0
DRAIN_BOUND = 10.0
_BROKER_PROTOCOL_MAX = 2 * MAX_LOG + 64 * 1024


def _proc_snapshot(lock_module) -> dict[int, tuple[int, int, int, str]]:
    """Return ``pid -> (ppid, pgid, starttime, state)`` from one proc scan."""
    snapshot: dict[int, tuple[int, int, int, str]] = {}
    for pid in lock_module._iter_pids():
        fields = lock_module._proc_stat_fields(pid)
        if fields is None or len(fields) < 20:
            continue
        try:
            snapshot[pid] = (
                int(fields[1]), int(fields[2]), int(fields[19]), fields[0]
            )
        except ValueError:
            continue
    return snapshot


def _owned_lineage_from_snapshot(
    snapshot: dict[int, tuple[int, int, int, str]],
    *,
    broker_pid: int,
    leader_pid: int,
    leader_starttime: int,
) -> dict[int, tuple[int, int, str]]:
    """Prove ownership solely by the dedicated broker's child lineage.

    The exact leader identity and every direct child of the fresh broker are
    roots; transitive children of those roots are owned.  A process whose
    numeric PGID happens to equal ``leader_pid`` is not a root and is never
    included by that coincidence.  Keeping the leader unreaped until this
    lineage settles also prevents its PID/PGID from being recycled while
    membership is pinned.
    """
    owned: set[int] = set()
    leader = snapshot.get(leader_pid)
    if leader is not None and leader[2] == leader_starttime:
        owned.add(leader_pid)
    owned.update(
        pid for pid, (parent, _pgid, _starttime, _state) in snapshot.items()
        if parent == broker_pid
    )
    frontier = list(owned)
    while frontier:
        parent = frontier.pop()
        for pid, (candidate_parent, _pgid, _starttime, _state) in snapshot.items():
            if candidate_parent == parent and pid not in owned:
                owned.add(pid)
                frontier.append(pid)
    return {
        pid: (snapshot[pid][2], snapshot[pid][1], snapshot[pid][3])
        for pid in owned
    }


def _leader_exited_nonreaping(pid: int) -> bool:
    """Observe a child leader's termination without consuming its status."""
    flags = os.WEXITED | os.WNOHANG | os.WNOWAIT
    try:
        result = os.waitid(os.P_PID, pid, flags)
    except InterruptedError:
        return False
    except ChildProcessError as exc:
        raise RuntimeError(
            f"the dedicated broker lost command leader {pid} before settlement"
        ) from exc
    return result is not None


def _direct_broker_children(lock_module, broker_pid: int) -> dict[int, tuple[int, str]]:
    """Return exact identities/states currently parented to the broker."""
    return {
        pid: (starttime, state)
        for pid, (parent, _pgid, starttime, state) in _proc_snapshot(lock_module).items()
        if parent == broker_pid
    }


def _reap_owned_children(lock_module, broker_pid: int, leader_pid: int) -> None:
    """Reap direct broker children by PID, retaining the leader's status."""
    for pid in _direct_broker_children(lock_module, broker_pid):
        if pid == leader_pid:
            continue
        try:
            os.waitpid(pid, os.WNOHANG)
        except (ChildProcessError, InterruptedError):
            continue


class _RetainedCommand:
    """A manually forked command whose exact pidfd is retained through reap."""

    def __init__(
        self,
        pid: int,
        argv: list[str],
        stdout,
        stderr,
        pidfd: int,
        pidfd_send_signal,
    ) -> None:
        self.pid = pid
        self.argv = list(argv)
        self.stdout = stdout
        self.stderr = stderr
        self.pidfd = pidfd
        self._pidfd_send_signal = pidfd_send_signal
        self.returncode: int | None = None

    def signal_pinned(self, lock_module, starttime: int, signum: int) -> bool:
        """Signal the revalidated command identity through its retained pidfd."""
        if not lock_module._is_live_with_identity(self.pid, starttime):
            return False
        try:
            self._pidfd_send_signal(self.pidfd, signum, None, 0)
        except ProcessLookupError:
            return False
        except OSError as exc:
            raise RuntimeError(
                f"cannot pidfd-signal retained command leader {self.pid}: {exc}"
            ) from exc
        return True

    def _close_pidfd(self) -> None:
        if self.pidfd < 0:
            return
        try:
            os.close(self.pidfd)
        except OSError:
            pass
        self.pidfd = -1

    def wait(self, timeout: float | None = None) -> int:
        """Consume the exact terminal status, then release the retained pidfd."""
        if self.returncode is not None:
            return self.returncode
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            try:
                waited, status = os.waitpid(
                    self.pid, 0 if deadline is None else os.WNOHANG
                )
            except InterruptedError:
                continue
            except ChildProcessError as exc:
                raise RuntimeError(
                    f"the dedicated broker lost command leader {self.pid}"
                ) from exc
            if waited == self.pid:
                if os.WIFEXITED(status):
                    self.returncode = os.WEXITSTATUS(status)
                elif os.WIFSIGNALED(status):
                    self.returncode = -os.WTERMSIG(status)
                else:
                    continue
                self._close_pidfd()
                return self.returncode
            if deadline is not None and time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(self.argv, timeout)
            time.sleep(0.01)


def _stopped_wrapper_identity(
    lock_module, pid: int, broker_pid: int, starttime: int
) -> bool:
    """Revalidate the exact stopped direct wrapper before untrusted exec."""
    fields = lock_module._proc_stat_fields(pid)
    if fields is None or len(fields) < 20:
        return False
    try:
        return (
            fields[0] in {"T", "t"}
            and int(fields[1]) == broker_pid
            and int(fields[19]) == starttime
        )
    except ValueError:
        return False


def _wipe_authorization(value: bytearray) -> None:
    """Best-effort removal of a short-lived authorization token."""
    value[:] = b"\0" * len(value)


def _consume_exec_authorization(
    authorization_fd: int, expected: bytearray
) -> None:
    """Read one exact parent capability and close it before command exec."""
    received = bytearray()
    authorized = False
    try:
        # Read through EOF, with one extra byte of capacity, so a prefix or a
        # token followed by any extra byte is never accepted as authorization.
        while len(received) <= len(expected):
            try:
                chunk = os.read(
                    authorization_fd, len(expected) + 1 - len(received)
                )
            except InterruptedError:
                continue
            if not chunk:
                break
            received.extend(chunk)
        authorized = (
            len(received) == len(expected)
            and hmac.compare_digest(received, expected)
        )
    finally:
        try:
            os.close(authorization_fd)
        except OSError:
            pass
        _wipe_authorization(expected)
        _wipe_authorization(received)
    if not authorized:
        raise RuntimeError("receipt command authorization failed before exec")


def _write_exec_authorization(
    authorization_fd: int, token: bytearray
) -> None:
    """Write the exact one-shot capability, then close the parent endpoint."""
    offset = 0
    try:
        while offset < len(token):
            try:
                written = os.write(authorization_fd, token[offset:])
            except InterruptedError:
                continue
            if written <= 0:
                raise BrokenPipeError("authorization pipe accepted no bytes")
            offset += written
    finally:
        # Wipe before close publishes EOF: the resumed child cannot validate
        # and exec while the parent still retains its token copy.
        _wipe_authorization(token)
        try:
            os.close(authorization_fd)
        except OSError:
            pass


def _abort_stopped_wrapper(
    pid: int,
    stdout_fd: int,
    stderr_fd: int,
    authorization_fd: int,
    authorization_token: bytearray,
    message: str,
    *,
    pidfd: int | None = None,
) -> None:
    """Deny authorization, kill/reap the retained wrapper, then fail closed."""
    # Closing without writing is the primary fail-closed action.  Even if an
    # external SIGCONT already resumed the wrapper, it can only unblock with
    # EOF and reject authorization; it cannot reach the untrusted exec.
    try:
        os.close(authorization_fd)
    except OSError:
        pass
    _wipe_authorization(authorization_token)
    if pidfd is not None:
        try:
            os.close(pidfd)
        except OSError:
            pass
    # The wrapper's terminal status has not been consumed, so this exact
    # direct-child PID cannot be reused.  It is therefore safe to kill by PID
    # whether it remains stopped or was resumed into the authorization read.
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    while True:
        try:
            os.waitpid(pid, 0)
            break
        except InterruptedError:
            continue
        except ChildProcessError:
            break
    for descriptor in (stdout_fd, stderr_fd):
        try:
            os.close(descriptor)
        except OSError:
            pass
    raise RuntimeError(message)


def _spawn_pidfd_staged_command(
    argv: list[str], root: Path, lock_module, broker_pid: int
) -> tuple[_RetainedCommand, int]:
    """Stop a trusted fork stage, prove pidfd signaling, then authorize exec.

    The child has the read end of a private CLOEXEC capability pipe and will
    accept only one unpredictable exact token.  Any resume, including an
    external SIGCONT before pidfd setup completes, merely blocks the child on
    that pipe.  The parent writes the token only after opening/revalidating the
    pidfd, proving signal 0, revalidating the stopped identity again, and
    successfully resuming through that pidfd.  Missing APIs or any pidfd/
    authorization failure closes the pipe without a token and kills/reaps the
    retained wrapper before any untrusted command can execute.
    """
    pipe_flags = getattr(os, "O_CLOEXEC", 0)
    stdout_read, stdout_write = os.pipe2(pipe_flags)
    stderr_read, stderr_write = os.pipe2(pipe_flags)
    authorization_read, authorization_write = os.pipe2(pipe_flags)
    authorization_token = bytearray(os.urandom(_EXEC_AUTHORIZATION_BYTES))
    pid = os.fork()
    if pid == 0:
        try:
            os.close(stdout_read)
            os.close(stderr_read)
            os.close(authorization_write)
            os.dup2(stdout_write, 1)
            os.dup2(stderr_write, 2)
            if stdout_write > 2:
                os.close(stdout_write)
            if stderr_write > 2:
                os.close(stderr_write)

            # A broker crash must kill this stage both before and after exec.
            import ctypes
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.prctl(1, signal.SIGKILL, 0, 0, 0) != 0:  # PR_SET_PDEATHSIG
                raise RuntimeError(
                    f"cannot install command parent-death signal (errno "
                    f"{ctypes.get_errno()})"
                )
            if os.getppid() != broker_pid:
                raise RuntimeError("receipt broker died during command staging")
            os.setsid()
            os.chdir(root)
            open_limit = resource.getrlimit(resource.RLIMIT_NOFILE)[0]
            close_bound = (
                1_048_576 if open_limit == resource.RLIM_INFINITY
                else max(256, int(open_limit))
            )
            # Preserve exactly one private read endpoint as fd 3 while closing
            # every other non-stdio descriptor.  dup2(..., inheritable=False)
            # retains CLOEXEC even if authorization validation later regresses.
            os.dup2(authorization_read, 3, inheritable=False)
            if authorization_read != 3:
                os.close(authorization_read)
            os.closerange(4, close_bound)
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
            resource.setrlimit(resource.RLIMIT_NOFILE, (256, 256))

            # SIGSTOP is only a scheduling barrier.  Authorization is a
            # separate one-shot parent capability, so an unsolicited SIGCONT
            # cannot release this stage into the untrusted command.
            os.kill(os.getpid(), signal.SIGSTOP)
            _consume_exec_authorization(3, authorization_token)
            if os.getppid() != broker_pid:
                raise RuntimeError("receipt broker died before command exec")
            os.execvpe(argv[0], list(argv), os.environ)
        except BaseException as exc:
            try:
                os.write(2, f"machine-receipt command stage: {exc}\n".encode())
            except OSError:
                pass
        os._exit(127)

    os.close(stdout_write)
    os.close(stderr_write)
    os.close(authorization_read)
    while True:
        try:
            waited, status = os.waitpid(pid, os.WUNTRACED)
            break
        except InterruptedError:
            continue
    if (
        waited != pid or not os.WIFSTOPPED(status)
        or os.WSTOPSIG(status) != signal.SIGSTOP
    ):
        for descriptor in (stdout_read, stderr_read, authorization_write):
            try:
                os.close(descriptor)
            except OSError:
                pass
        _wipe_authorization(authorization_token)
        raise RuntimeError(
            "trusted command wrapper exited before the pidfd pre-exec handshake"
        )

    fields = lock_module._proc_stat_fields(pid)
    try:
        starttime = int(fields[19]) if fields is not None and len(fields) >= 20 else None
    except ValueError:
        starttime = None
    if starttime is None or not _stopped_wrapper_identity(
        lock_module, pid, broker_pid, starttime
    ):
        _abort_stopped_wrapper(
            pid, stdout_read, stderr_read, authorization_write,
            authorization_token,
            "cannot identity-pin the stopped command wrapper before exec",
        )

    pidfd_open = getattr(os, "pidfd_open", None)
    pidfd_send_signal = getattr(signal, "pidfd_send_signal", None)
    if not callable(pidfd_open) or not callable(pidfd_send_signal):
        _abort_stopped_wrapper(
            pid, stdout_read, stderr_read, authorization_write,
            authorization_token,
            "pidfd signaling is unavailable before command exec; command not started",
        )
    try:
        pidfd = pidfd_open(pid, 0)
    except OSError as exc:
        _abort_stopped_wrapper(
            pid, stdout_read, stderr_read, authorization_write,
            authorization_token,
            f"cannot pidfd-pin stopped command wrapper before exec: {exc}",
        )
    if not _stopped_wrapper_identity(lock_module, pid, broker_pid, starttime):
        _abort_stopped_wrapper(
            pid, stdout_read, stderr_read, authorization_write,
            authorization_token,
            "stopped command wrapper identity changed after pidfd_open",
            pidfd=pidfd,
        )
    try:
        # Signal 0 proves this Python/kernel pidfd path for the exact command
        # identity.  The final stopped-state check precedes the pidfd resume;
        # only after that resume succeeds does the parent send authorization.
        pidfd_send_signal(pidfd, 0, None, 0)
        if not _stopped_wrapper_identity(lock_module, pid, broker_pid, starttime):
            _abort_stopped_wrapper(
                pid, stdout_read, stderr_read, authorization_write,
                authorization_token,
                "stopped command wrapper identity changed before pidfd release",
                pidfd=pidfd,
            )
        stdout = os.fdopen(stdout_read, "rb", buffering=0)
        stderr = os.fdopen(stderr_read, "rb", buffering=0)
        process = _RetainedCommand(
            pid, argv, stdout, stderr, pidfd, pidfd_send_signal
        )
        pidfd_send_signal(pidfd, signal.SIGCONT, None, 0)
        authorization_fd = authorization_write
        authorization_write = -1
        _write_exec_authorization(authorization_fd, authorization_token)
    except OSError as exc:
        # fdopen ownership moves to ``process`` immediately before the pidfd
        # resume.  Without a complete exact token, even a resumed wrapper can
        # only reject EOF; it cannot release the untrusted command.
        if "process" in locals():
            try:
                process.stdout.close()
            except OSError:
                pass
            try:
                process.stderr.close()
            except OSError:
                pass
            stdout_read = stderr_read = -1
        _abort_stopped_wrapper(
            pid, stdout_read, stderr_read, authorization_write,
            authorization_token,
            f"cannot prove/resume/authorize command before exec: {exc}",
            pidfd=pidfd,
        )
    return process, starttime


def _run_bounded_in_broker(
    argv: list[str], root: Path, lock_module
) -> tuple[int, bytes, bytes, bool]:
    """Run one command inside a fresh, unrelated-child-free subreaper."""
    broker_pid = os.getpid()
    _install_subreaper()
    if _direct_broker_children(lock_module, broker_pid):
        raise RuntimeError(
            "the dedicated receipt broker unexpectedly has pre-existing children"
        )

    process, leader_starttime = _spawn_pidfd_staged_command(
        argv, root, lock_module, broker_pid
    )
    leader_pid = process.pid
    stdout = bytearray()
    stderr = bytearray()
    overflow = threading.Event()

    def drain(stream, output: bytearray) -> None:
        try:
            while True:
                chunk = stream.read(65_536)
                if not chunk:
                    return
                if len(output) + len(chunk) > MAX_LOG:
                    overflow.set()
                    return
                output.extend(chunk)
        finally:
            try:
                stream.close()
            except OSError:
                pass

    threads = [
        threading.Thread(target=drain, args=(process.stdout, stdout), daemon=True),
        threading.Thread(target=drain, args=(process.stderr, stderr), daemon=True),
    ]
    for thread in threads:
        thread.start()

    pinned: dict[int, int] = {leader_pid: leader_starttime}
    escaped_seen: set[int] = set()

    def refresh_owned() -> dict[int, tuple[int, int, str]]:
        current = _owned_lineage_from_snapshot(
            _proc_snapshot(lock_module),
            broker_pid=broker_pid,
            leader_pid=leader_pid,
            leader_starttime=leader_starttime,
        )
        for pid, (starttime, _pgid, _state) in current.items():
            # A recycled PID can legitimately name a later command child.
            # Current ownership is re-proven from the broker lineage before
            # its new starttime supersedes the dead identity; numeric reuse
            # alone can never add a process.
            pinned[pid] = starttime
        return current

    def live_pinned(*, include_leader: bool) -> dict[int, int]:
        return {
            pid: starttime for pid, starttime in pinned.items()
            if (include_leader or pid != leader_pid)
            and lock_module._is_live_with_identity(pid, starttime)
        }

    def terminate_lineage(
        *, include_leader: bool, record_surviving_escapes: bool = False
    ) -> None:
        """TERM, full grace, KILL, and reap only broker-proven identities."""
        term_sent: set[tuple[int, int]] = set()
        grace_deadline = time.monotonic() + KILL_GRACE
        while True:
            current = refresh_owned()
            if record_surviving_escapes:
                escaped_seen.update(
                    pid for pid, (starttime, pgid, _state) in current.items()
                    if pid != leader_pid and pgid != leader_pid
                    and lock_module._is_live_with_identity(pid, starttime)
                )
            for pid, starttime in live_pinned(include_leader=include_leader).items():
                identity = (pid, starttime)
                if identity not in term_sent:
                    if pid == leader_pid:
                        process.signal_pinned(
                            lock_module, starttime, signal.SIGTERM
                        )
                    else:
                        lock_module._signal_pid_pinned(
                            pid, starttime, signal.SIGTERM
                        )
                    term_sent.add(identity)
            _reap_owned_children(lock_module, broker_pid, leader_pid)
            remaining = grace_deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(0.02, remaining))

        kill_deadline = time.monotonic() + REAP_BOUND
        while True:
            refresh_owned()
            for pid, starttime in live_pinned(include_leader=include_leader).items():
                if pid == leader_pid:
                    process.signal_pinned(
                        lock_module, starttime, signal.SIGKILL
                    )
                else:
                    lock_module._signal_pid_pinned(
                        pid, starttime, signal.SIGKILL
                    )
            _reap_owned_children(lock_module, broker_pid, leader_pid)
            refresh_owned()
            live = live_pinned(include_leader=include_leader)
            direct = _direct_broker_children(lock_module, broker_pid)
            unsettled_direct = {
                pid: value for pid, value in direct.items() if pid != leader_pid
            }
            if not live and not unsettled_direct:
                return
            if time.monotonic() >= kill_deadline:
                raise RuntimeError(
                    "the dedicated command lineage did not die and reap within "
                    f"the {REAP_BOUND:.1f}s KILL bound: "
                    f"{sorted(set(live) | set(unsettled_direct))}"
                )
            time.sleep(0.02)

    timed_out = False
    deadline = time.monotonic() + RUN_TIMEOUT
    try:
        # WNOWAIT retains the leader identity/status while every descendant
        # membership is pinned.  ``Popen.poll`` is deliberately forbidden in
        # this phase: it would free the numeric PID/PGID before settlement.
        while not overflow.is_set():
            refresh_owned()
            if _leader_exited_nonreaping(leader_pid):
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                timed_out = True
                break
            time.sleep(min(0.05, remaining))

        refresh_owned()
        descendants_live = live_pinned(include_leader=False)
        if timed_out or overflow.is_set() or descendants_live:
            terminate_lineage(
                include_leader=timed_out or overflow.is_set(),
                record_surviving_escapes=not timed_out and not overflow.is_set(),
            )

        # A settled interval closes adoption/reap races and requires pipe EOF.
        settle_deadline = time.monotonic() + STABILIZE_BOUND
        clean_since: float | None = None
        while True:
            refresh_owned()
            _reap_owned_children(lock_module, broker_pid, leader_pid)
            descendants = live_pinned(include_leader=False)
            direct = {
                pid: value
                for pid, value in _direct_broker_children(
                    lock_module, broker_pid
                ).items()
                if pid != leader_pid
            }
            pipes_done = not any(thread.is_alive() for thread in threads)
            if not descendants and not direct and pipes_done:
                if clean_since is None:
                    clean_since = time.monotonic()
                elif time.monotonic() - clean_since >= STABILIZE_WINDOW:
                    break
            else:
                clean_since = None
                if descendants:
                    terminate_lineage(
                        include_leader=False,
                        record_surviving_escapes=not timed_out and not overflow.is_set(),
                    )
            if time.monotonic() >= settle_deadline:
                raise RuntimeError(
                    "the dedicated command lineage/pipes did not settle within "
                    f"{STABILIZE_BOUND:.1f}s: "
                    f"{sorted(set(descendants) | set(direct))}"
                )
            time.sleep(0.02)

        # Only now may the exact leader status be consumed.  Retaining it up
        # to this point prevents PID/PGID reuse from widening ownership.
        try:
            returncode = process.wait(timeout=REAP_BOUND)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(
                f"command leader {leader_pid} was not reapable after settlement"
            ) from exc
    except BaseException:
        try:
            refresh_owned()
            terminate_lineage(include_leader=True)
        except BaseException:
            pass
        try:
            process.wait(timeout=REAP_BOUND)
        except BaseException:
            pass
        raise
    finally:
        for thread in threads:
            thread.join(timeout=DRAIN_BOUND)

    if timed_out:
        raise RuntimeError(
            "the bounded command exceeded the "
            f"{RUN_TIMEOUT:.0f}s timeout; the run was terminated and no "
            "receipt was minted"
        )
    if escaped_seen:
        raise RuntimeError(
            "escaped descendants survived the bounded command termination: "
            f"{sorted(escaped_seen)} (all were terminated and reaped by the "
            "dedicated broker; a run with escaped descendants can never mint "
            "a receipt)"
        )
    return returncode, bytes(stdout), bytes(stderr), overflow.is_set()


def _write_all(descriptor: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        try:
            written = os.write(descriptor, view)
        except InterruptedError:
            continue
        if written <= 0:
            raise RuntimeError("the dedicated broker result pipe was short")
        view = view[written:]


def _broker_main(
    descriptor: int, argv: list[str], root: Path, lock_module
) -> None:
    """Serialize one trusted broker result over a CLOEXEC private pipe."""
    try:
        returncode, stdout, stderr, overflow = _run_bounded_in_broker(
            argv, root, lock_module
        )
        header = {
            "ok": True,
            "returncode": returncode,
            "overflow": overflow,
            "stdout": len(stdout),
            "stderr": len(stderr),
        }
        payload = stdout + stderr
    except BaseException as exc:
        header = {"ok": False, "error": str(exc)}
        payload = b""
    data = json.dumps(header, separators=(",", ":")).encode("utf-8") + b"\n" + payload
    try:
        _write_all(descriptor, data)
    except BaseException:
        pass


def _signal_broker(lock_module, pid: int, starttime: int) -> None:
    try:
        lock_module._signal_pid_pinned(pid, starttime, signal.SIGKILL)
    except BaseException:
        pass


def run_bounded(argv: list[str], cwd: Path) -> tuple[int, bytes, bytes, bool]:
    """Run through a dedicated subreaper broker and return its exact result.

    The caller waits only for the exact broker PID and never scans, signals,
    or reaps any other child.  Thus a pre-existing caller child may fork a
    worker and exit at any point without either identity or exit status being
    adopted as receipt-owned work.
    """
    root = Path(cwd).absolute()
    lock_module = _load_lock_supervision(root)
    pipe_flags = getattr(os, "O_CLOEXEC", 0)
    read_fd, write_fd = os.pipe2(pipe_flags)
    ack_read, ack_write = os.pipe2(pipe_flags)
    broker_pid = os.fork()
    if broker_pid == 0:
        try:
            os.close(read_fd)
            os.close(ack_write)
            # Do not spawn the command until the parent has pinned this exact
            # broker identity.  EOF (including parent death) exits childless.
            while True:
                try:
                    acknowledged = os.read(ack_read, 1)
                    break
                except InterruptedError:
                    continue
            os.close(ack_read)
            if acknowledged == b"G":
                _broker_main(write_fd, list(argv), root, lock_module)
        finally:
            try:
                os.close(write_fd)
            except OSError:
                pass
        os._exit(0)

    os.close(write_fd)
    os.close(ack_read)
    broker_starttime = None
    pin_deadline = time.monotonic() + 1.0
    while time.monotonic() < pin_deadline:
        fields = lock_module._proc_stat_fields(broker_pid)
        if fields is not None and len(fields) >= 20:
            try:
                broker_starttime = int(fields[19])
                break
            except ValueError:
                pass
        time.sleep(0.005)
    if broker_starttime is None:
        # Closing the acknowledgement pipe makes the still-childless broker
        # exit.  Only its exact status is consumed; no command was spawned.
        os.close(ack_write)
        os.close(read_fd)
        try:
            os.waitpid(broker_pid, 0)
        except ChildProcessError:
            pass
        fail("cannot pin the dedicated receipt broker identity")
    try:
        _write_all(ack_write, b"G")
    finally:
        os.close(ack_write)

    data = bytearray()
    broker_bound = (
        RUN_TIMEOUT + KILL_GRACE + REAP_BOUND + STABILIZE_BOUND + DRAIN_BOUND + 5.0
    )
    deadline = time.monotonic() + broker_bound
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                _signal_broker(lock_module, broker_pid, broker_starttime)
                fail("the dedicated receipt broker exceeded its lifecycle bound")
            try:
                readable, _, _ = select.select([read_fd], [], [], min(0.2, remaining))
            except InterruptedError:
                continue
            if not readable:
                continue
            chunk = os.read(read_fd, 65_536)
            if not chunk:
                break
            data.extend(chunk)
            if len(data) > _BROKER_PROTOCOL_MAX:
                _signal_broker(lock_module, broker_pid, broker_starttime)
                fail("the dedicated receipt broker returned an oversized result")
    except BaseException:
        _signal_broker(lock_module, broker_pid, broker_starttime)
        raise
    finally:
        try:
            os.close(read_fd)
        except OSError:
            pass
        try:
            waited, status = os.waitpid(broker_pid, 0)
        except ChildProcessError:
            waited, status = 0, 0

    if waited != broker_pid or not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
        fail("the dedicated receipt broker exited without a trusted result")
    header_raw, separator, payload = bytes(data).partition(b"\n")
    if not separator or len(header_raw) > 64 * 1024:
        fail("the dedicated receipt broker returned a malformed result")
    try:
        header = json.loads(header_raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        fail(f"the dedicated receipt broker result is invalid: {exc}")
    if not isinstance(header, dict) or not header.get("ok"):
        error = header.get("error") if isinstance(header, dict) else None
        fail(str(error or "the dedicated receipt broker failed closed"))
    stdout_len = header.get("stdout")
    stderr_len = header.get("stderr")
    returncode = header.get("returncode")
    overflow = header.get("overflow")
    if (
        type(stdout_len) is not int or not 0 <= stdout_len <= MAX_LOG
        or type(stderr_len) is not int or not 0 <= stderr_len <= MAX_LOG
        or len(payload) != stdout_len + stderr_len
        or type(returncode) is not int
        or type(overflow) is not bool
    ):
        fail("the dedicated receipt broker result fields are malformed")
    return (
        returncode,
        payload[:stdout_len],
        payload[stdout_len:],
        overflow,
    )


def record_receipt(root: Path, tag: str, argv: list[str], exit_code: int,
                   stdout: bytes, stderr: bytes, started: float,
                   round_number: int, evidence_commit: str, nonce: str) -> Path:
    receipts = receipts_dir(root)
    stdout_path = receipts / f"{tag}.stdout"
    stderr_path = receipts / f"{tag}.stderr"
    # Task 12 §19: same-tag receipt publication is no-replace — an existing
    # artifact (a pre-planted or forged receipt, or a reused tag) fails
    # closed instead of being silently replaced.
    for existing in (stdout_path, stderr_path, receipts / f"{tag}.json"):
        if existing.is_symlink() or existing.exists():
            fail(
                f"receipt tag {tag!r} already published; same-tag "
                "publication fails closed (no-replace)"
            )
    atomic_write_noreplace(stdout_path, stdout)
    atomic_write_noreplace(stderr_path, stderr)
    receipt = {
        "schema": "ralph-audit-receipt/v1",
        "tag": tag,
        "argv": argv,
        "argv_sha256": hashlib.sha256(json.dumps(argv, separators=(",", ":")).encode()).hexdigest(),
        "exit_code": exit_code,
        "stdout_sha256": hashlib.sha256(stdout).hexdigest(),
        "stderr_sha256": hashlib.sha256(stderr).hexdigest(),
        "started_at": int(started),
        "finished_at": int(time.time()),
        "evidence_commit": evidence_commit,
        "coordinator_round": round_number,
        "coordinator_nonce": nonce,
    }
    receipt_path = receipts / f"{tag}.json"
    atomic_write_noreplace(receipt_path, (json.dumps(receipt, sort_keys=True, indent=2) + "\n").encode())
    verify_artifact(stdout_path, f"receipt stdout {tag}")
    verify_artifact(stderr_path, f"receipt stderr {tag}")
    verify_artifact(receipt_path, f"receipt record {tag}")
    return receipt_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True)
    parser.add_argument("--root", default=str(ROOT))
    parser.add_argument("--audit-round", type=int)
    parser.add_argument("--evidence-commit")
    parser.add_argument("--nonce")
    parser.add_argument("argv", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    if not TAG.fullmatch(args.tag):
        fail("invalid receipt tag")
    argv = args.argv
    if argv and argv[0] == "--":
        argv = argv[1:]
    if not argv:
        fail("no command provided")
    if any(item == "" for item in argv):
        fail("argv must be non-empty strings")
    round_number, evidence_commit, nonce = coordinator_binding(
        root, args.audit_round, args.evidence_commit, args.nonce
    )
    # Validate (or safely create) the exact private publication directory
    # before the command runs; an unsafe runtime mode never executes a command
    # and is never chmod-repaired as a side effect.
    receipts_dir(root)
    started = time.time()
    returncode, stdout, stderr, overflow = run_bounded(argv, root)
    if overflow:
        # Overflow keeps a bounded diagnostic on stderr (never the raw
        # truncated transcript, which can carry secrets) and exits nonzero:
        # a truncated run is never recorded, and the diagnostic itself is
        # bounded and secret-free.
        fail(
            "command output exceeded the receipt limit (stdout/stderr "
            f"bounded to {MAX_LOG} bytes); the run was terminated and the "
            "truncated transcript is withheld from the diagnostic to avoid "
            "leaking raw secrets; no receipt was minted"
        )
    receipt_path = record_receipt(
        root, args.tag, argv, returncode, stdout, stderr, started,
        round_number, evidence_commit, nonce,
    )
    print(f"[receipt: {receipt_path.relative_to(root)}]")
    return returncode if returncode < 255 else 1


if __name__ == "__main__":
    raise SystemExit(main())
