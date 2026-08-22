#!/usr/bin/env python3
"""atomic-publish.py — atomic no-replace publish + fail-closed fsync/identity helper.

Owned primitive used by scripts/visual-capture-driver.sh so the driver never
uses an ordinary overwriting `mv` (which is TOCTOU-prone and would silently
replace an unowned destination that appears after an existence check).

Subcommands:

  publish <src> <dst>
      Atomically rename <src> to <dst> ONLY if <dst> does not already exist
      (as a regular file, symlink, dangling symlink, hardlink, or any other
      directory entry). Uses renameat2(RENAME_NOREPLACE) on Linux; falls back
      to link()+unlink() ONLY when the kernel/filesystem genuinely lacks the
      no-replace flag (ENOSYS/EINVAL/EOPNOTSUPP). Any other error (EACCES,
      EPERM, EIO, EROFS, ...) is a real failure, not a reason to silently
      change strategy. Both paths must be on the same filesystem (the driver
      guarantees this by writing its owned temps in the same directory as the
      destination).
      Before publishing, the source is validated to be a CURRENT-USER-owned
      regular file with a single link (never a symlink, directory, or an
      inode hardlinked elsewhere), and the destination existence check uses
      follow_symlinks=False (lexists) so a dangling or real symlink is never
      followed/replaced. On the link() fallback, a failure to unlink the
      source after the destination is published is treated as a failure
      (fail closed) rather than a silent success.
      Exit: 0 success; 2 destination already exists (never overwritten);
            3 any other error (including a non-regular/non-owned/multi-link
               source or a hard error from the no-replace syscall).

  fsync <path>
      fsync the given file or directory and propagate failure via a nonzero
      exit. This is the driver's fail-closed durability primitive: a fsync
      error is never swallowed with `|| true`, so the driver cannot claim a
      crash/power-durable publish unless every file and directory entry it
      depends on was actually flushed to stable storage.
      Exit: 0 success; 3 on any open/fsync error.

  receipt <out> <field=value> [...]
      Build a capture receipt as JSON through the Python json serializer so
      every field value (which may legitimately contain quotes, backslashes,
      tabs, newlines, or other JSON metacharacters in paths/titles) is
      correctly escaped — never string-interpolated into a hand-built JSON
      blob. The binary_sha256 and image_sha256 fields are REQUIRED to be
      exactly 64 lowercase/uppercase hex characters; any other value fails
      closed (exit 3) so a capture is never recorded without a verifiable
      content hash.
      Exit: 0 success; 3 on a missing required field, a non-64-hex hash, or a
            write error.

  recover <receipt_path> <output_path>
      Bounded power-loss/crash recovery of a committed-but-unpublished image.
      The driver's receipt-first ordering makes <receipt_path> the durable
      COMMIT MARKER: its bytes and directory entry are fsynced before the image
      is renamed to <output_path>. A power loss in that window leaves the
      committed image's bytes durable but stranded at an orphaned
      `.cbx-capture.*` temp in the same directory (never at OUTPUT). This
      recovers exactly that case: if <output_path> is absent and <receipt_path>
      is a current-user committed receipt whose image_sha256 matches an owned
      `.cbx-capture.*` temp's bytes, that temp IS the committed image and is
      restored atomically (no-replace) and made durable. It also removes
      provably-uncommitted owned `.cbx-receipt.*` temps (a receipt is only ever
      published under the fixed <output>.receipt.json name, so such a temp is
      always an interrupted draft, never a commit marker). Recovery NEVER sweeps
      arbitrary files and NEVER treats an orphan temp as evidence on its own.
      Exit: 0 always (best-effort restoration of prior state; it is never a
            reason to withhold a fresh capture).
"""

import ctypes
import errno
import glob
import hashlib
import json
import os
import re
import sys

AT_FDCWD = -100
RENAME_NOREPLACE = 1

# Fall back to link()+unlink() only for filesystems/kernels that genuinely do
# not implement renameat2(RENAME_NOREPLACE). Any other errno is a real
# failure. ENOTSUP is an alias of EOPNOTSUPP on Linux; the set tolerates both.
_FALLBACK_ERRNOS = {errno.ENOSYS, errno.EINVAL, errno.EOPNOTSUPP, errno.ENOTSUP}

_HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")

_ME = os.geteuid()


def _lstat_regular_owned_single_link(path: str) -> bool:
    """True iff <path> (via lstat, never following a symlink) is a current-
    user-owned regular file with a single link."""
    try:
        st = os.lstat(path)
    except OSError:
        return False
    return _regular_owned_single_link(st)


def _rm_owned(path: str) -> None:
    """Remove <path> ONLY if it is a current-user-owned regular single-link
    file; anything owned by another principal, a symlink, a directory, or a
    hardlinked elsewhere is never removed."""
    if not _lstat_regular_owned_single_link(path):
        return
    try:
        os.unlink(path)
    except OSError:
        pass


def _sha256_of(path: str):
    """Return the lowercase hex sha256 of <path> or None on a read error."""
    try:
        h = hashlib.sha256()
        with open(path, "rb") as fh:
            for blk in iter(lambda: fh.read(1 << 16), b""):
                h.update(blk)
        return h.hexdigest()
    except OSError:
        return None


def _regular_owned_single_link(st) -> bool:
    """True iff st is a current-user-owned regular file with link count 1."""
    return (
        st is not None
        and st.st_mode & 0o170000 == 0o100000  # S_IFREG
        and st.st_uid == _ME
        and st.st_nlink == 1
    )


def _validate_source(src: str):
    """Fail closed unless src is a current-user-owned single-link regular file.

    Opens O_NOFOLLOW (never follows a symlink) and returns the fstat on
    success or None on any violation, printing the reason to stderr.
    """
    try:
        fd = os.open(src, os.O_RDONLY | os.O_NOFOLLOW)
    except OSError as e:
        print(f"atomic-publish: source open {src}: {e}", file=sys.stderr)
        return None
    try:
        st = os.fstat(fd)
    finally:
        os.close(fd)
    if not _regular_owned_single_link(st):
        print(
            f"atomic-publish: source not a current-user-owned single-link "
            f"regular file: {src}",
            file=sys.stderr,
        )
        return None
    return st


def _renameat2_noreplace(src: str, dst: str):
    """Return 0 success, 2 dst-exists, None to fall back, or -1 on a hard error."""
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        renameat2 = getattr(libc, "renameat2", None)
    except OSError:
        renameat2 = None
    if renameat2 is None:
        return None
    renameat2.argtypes = [
        ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint,
    ]
    renameat2.restype = ctypes.c_int
    rc = renameat2(AT_FDCWD, os.fsencode(src), AT_FDCWD, os.fsencode(dst),
                   RENAME_NOREPLACE)
    if rc == 0:
        return 0
    err = ctypes.get_errno()
    if err == errno.EEXIST:
        return 2
    if err in _FALLBACK_ERRNOS:
        # Kernel/filesystem lacks RENAME_NOREPLACE; fall back to link()+unlink().
        return None
    print(
        f"atomic-publish: renameat2 {src} -> {dst}: {os.strerror(err)}",
        file=sys.stderr,
    )
    return -1


def _publish_link(src: str, dst: str):
    """Atomic no-replace publish via link()+unlink(); 0 ok, 2 exists, 3 error."""
    try:
        os.link(src, dst)
    except FileExistsError:
        return 2
    except OSError as e:
        if e.errno == errno.EEXIST:
            return 2
        print(f"atomic-publish: link {src} -> {dst}: {e}", file=sys.stderr)
        return 3
    # The destination is published; remove the source temp. A failure here is
    # FATAL (fail closed): leaving a still-present source would let a caller
    # mistake an already-published capture for an unpublished one. To keep the
    # pre-publish state (and the driver's "no destination-without-receipt / no
    # receipt-without-image" invariant), first withdraw the just-created
    # no-replace destination (which is provably ours), then report the error.
    try:
        os.unlink(src)
    except OSError as e:
        try:
            os.unlink(dst)
        except OSError:
            pass
        print(
            f"atomic-publish: unlink source {src} after publish: {e}; "
            f"destination {dst} withdrawn to restore pre-publish state",
            file=sys.stderr,
        )
        return 3
    return 0


def cmd_publish(src: str, dst: str) -> int:
    if not _validate_source(src):
        return 3
    # Fast path for a clearly pre-existing/unowned destination. lexists uses
    # follow_symlinks=False (lstat), so a dangling or real symlink is seen as
    # "exists" and never followed. The no-replace syscall below is still the
    # authoritative gate: a destination that appears after this check is
    # refused atomically in the same syscall as the publish.
    if os.path.lexists(dst):
        return 2
    rc = _renameat2_noreplace(src, dst)
    if rc == -1:
        return 3
    if rc is not None:
        return rc
    return _publish_link(src, dst)


def cmd_fsync(path: str) -> int:
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    except OSError as e:
        print(f"atomic-publish: open {path} for fsync: {e}", file=sys.stderr)
        return 3
    try:
        os.fsync(fd)
    except OSError as e:
        print(f"atomic-publish: fsync {path}: {e}", file=sys.stderr)
        return 3
    finally:
        os.close(fd)
    return 0


_RECEIPT_REQUIRED = (
    "schema", "state", "commit", "install_prefix", "binary_sha256",
    "image", "image_sha256", "window_title", "display", "finished_at",
)
_RECEIPT_HASH_FIELDS = ("binary_sha256", "image_sha256")


def cmd_receipt(out: str, pairs) -> int:
    """Build a receipt as JSON, requiring 64-hex hashes; 0 ok, 3 error."""
    fields: dict[str, str] = {}
    for pair in pairs:
        key, sep, value = pair.partition("=")
        if sep != "=" or not key:
            print(f"atomic-publish: receipt field must be key=value: {pair!r}",
                  file=sys.stderr)
            return 3
        fields[key] = value
    missing = [k for k in _RECEIPT_REQUIRED if k not in fields]
    if missing:
        print(
            f"atomic-publish: receipt missing required fields: {', '.join(missing)}",
            file=sys.stderr,
        )
        return 3
    for k in _RECEIPT_HASH_FIELDS:
        if not _HEX64.fullmatch(fields[k]):
            print(
                f"atomic-publish: receipt field {k} is not a 64-hex hash: "
                f"{fields[k]!r}",
                file=sys.stderr,
            )
            return 3
    try:
        with open(out, "w", encoding="utf-8") as fh:
            json.dump(fields, fh, indent=2, sort_keys=True, ensure_ascii=False)
            fh.write("\n")
    except OSError as e:
        print(f"atomic-publish: receipt write {out}: {e}", file=sys.stderr)
        return 3
    return 0


def cmd_recover(receipt_path: str, output_path: str) -> int:
    """Bounded power-loss/crash recovery; see the module docstring recover.

    Always returns 0: restoration of prior state is best-effort and must never
    be a reason to withhold a fresh capture. It is idempotent and safe to call
    on every driver startup (a no-op unless a committed receipt is present and
    its image is still missing at <output_path>).
    """
    d = os.path.dirname(output_path) or "."
    # 1. Remove provably-uncommitted owned receipt temps. Receipts are only
    #    ever published under the fixed <output>.receipt.json name, so a
    #    `.cbx-receipt.*` temp is always an interrupted draft, never a commit
    #    marker. It is never evidence and is inert; removing it is safe bounded
    #    recovery.
    for name in glob.glob(os.path.join(d, ".cbx-receipt.*")):
        _rm_owned(name)
    # 2. If the image is already at OUTPUT, nothing to restore.
    if os.path.lexists(output_path):
        return 0
    # 3. A restore requires a committed, current-user receipt whose referenced
    #    image is still missing.
    if not _lstat_regular_owned_single_link(receipt_path):
        return 0
    try:
        with open(receipt_path, encoding="utf-8") as fh:
            rec = json.load(fh)
        rec_hash = rec.get("image_sha256", "")
    except (OSError, ValueError):
        return 0
    if not _HEX64.fullmatch(rec_hash):
        return 0
    # 4. Find an owned `.cbx-capture.*` temp whose bytes hash to the committed
    #    image; it IS the committed image and is restored atomically (no-replace
    #    refuses a concurrently-created OUTPUT) and made durable. Only the exact
    #    committed hash is restored; an unrelated or unowned orphan is left
    #    alone (a sweep could orphan another committed receipt's image).
    for name in glob.glob(os.path.join(d, ".cbx-capture.*")):
        if not _lstat_regular_owned_single_link(name):
            continue
        if _sha256_of(name) != rec_hash:
            continue
        rc = cmd_publish(name, output_path)
        if rc == 0:
            # Make the restored image's directory entry durable so the commit
            # marker is no longer the only durable half of the pair.
            cmd_fsync(d)
            _rm_owned(name)
        return 0
    return 0


def main(argv) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 3
    sub = argv[1]
    if sub == "publish" and len(argv) == 4:
        return cmd_publish(argv[2], argv[3])
    if sub == "fsync" and len(argv) == 3:
        return cmd_fsync(argv[2])
    if sub == "receipt" and len(argv) >= 3:
        return cmd_receipt(argv[2], argv[3:])
    if sub == "recover" and len(argv) == 4:
        return cmd_recover(argv[2], argv[3])
    print(
        f"atomic-publish: usage: {argv[0]} publish <src> <dst> | fsync <path> "
        f"| receipt <out> <field=value> [...] | recover <receipt> <output>",
        file=sys.stderr,
    )
    return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv))
