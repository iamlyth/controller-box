#!/usr/bin/env bash
# Repair volatile Ralph markers and continue the same single-writer loop.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
RALPH_DIR="$PROJECT_ROOT/.ralph"
SCRATCHPAD="$RALPH_DIR/agent/scratchpad.md"
TASKS_FILE="$RALPH_DIR/agent/tasks.jsonl"
LOOP_MARKER="$RALPH_DIR/current-loop-id"
EVENTS_MARKER="$RALPH_DIR/current-events"
LOCK_FILE="$RALPH_DIR/loop.lock"
DRY_RUN=false
PREPARE_ONLY=false
MODE=implementation
EXPLICIT_LOOP_ID=""
ORIGINAL_ARGS=("$@")

usage() {
    cat <<'EOF'
Usage: scripts/ralph-recover.sh [options]

Options:
  --loop-id ID       Override inferred loop ID
  --mode MODE        implementation (default), planning, campaign-audit,
                     maintenance-planning, or maintenance
  --prepare-only     Repair markers but do not start Ralph
  --dry-run          Print the recovery plan without changing files
  -h, --help         Show help
EOF
}

die() { echo "ralph-recover: $*" >&2; exit 1; }
warn() { echo "ralph-recover: warning: $*" >&2; }

while (( $# > 0 )); do
    case "$1" in
        --loop-id) (( $# >= 2 )) || die "--loop-id requires a value"; EXPLICIT_LOOP_ID=$2; shift 2 ;;
        --mode) (( $# >= 2 )) || die "--mode requires a value"; MODE=$2; shift 2 ;;
        --prepare-only) PREPARE_ONLY=true; shift ;;
        --dry-run) DRY_RUN=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option '$1'" ;;
    esac
done
case "$MODE" in
    implementation|planning|campaign-audit|maintenance-planning|maintenance) ;;
    *) die "invalid mode '$MODE'" ;;
esac

# Serialize recovery with planning and implementation, and pass the inherited
# lock descriptor into the resumed supervisor.
# shellcheck source=scripts/factory-lock.sh
source "$SCRIPT_DIR/factory-lock.sh"
factory_lock_bootstrap "$PROJECT_ROOT/.factory-lock" "$PROJECT_ROOT/scripts/ralph-recover.sh" "${ORIGINAL_ARGS[@]}"
factory_lock_acquire "$PROJECT_ROOT/.factory-lock"
[[ -d "$RALPH_DIR" && ! -L "$RALPH_DIR" ]] || die "missing or unsafe $RALPH_DIR"
python3 - "$RALPH_DIR" <<'PY' || die "unsafe Ralph recovery paths"
import os
import stat
import sys
from pathlib import Path

root=Path(sys.argv[1])
for directory in (root, root/'agent'):
    try: info=os.lstat(directory)
    except FileNotFoundError: raise SystemExit(f'ralph-recover: missing {directory}')
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise SystemExit(f'ralph-recover: unsafe directory {directory}')
for path in (root/'agent/scratchpad.md', root/'agent/tasks.jsonl', root/'current-loop-id',
             root/'current-events', root/'loop.lock'):
    try: info=os.lstat(path)
    except FileNotFoundError: continue
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.getuid():
        raise SystemExit(f'ralph-recover: unsafe runtime file {path}')
PY
MODE_MARKER="$PROJECT_ROOT/.factory-state/loop-mode"
if [[ -s "$MODE_MARKER" ]]; then
    recorded_mode=$(tr -d '[:space:]' < "$MODE_MARKER")
    [[ "$recorded_mode" == "$MODE" ]] || die "requested mode '$MODE' does not match recorded loop mode '$recorded_mode'"
else
    warn "old run has no .factory-state/loop-mode marker; continuing with requested mode '$MODE'"
fi

if [[ -f "$LOCK_FILE" ]]; then
    lock_pid=$(python3 - "$LOCK_FILE" <<'PY'
import json, os, stat, sys
path = sys.argv[1]
fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
try:
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.getuid():
        raise SystemExit('ralph-recover: unsafe loop lock')
    raw = os.read(fd, 16385)
    if len(raw) > 16384:
        raise SystemExit('ralph-recover: loop lock exceeds the validation limit')
    data = json.loads(raw.decode('utf-8'))
finally:
    os.close(fd)
if not isinstance(data, dict) or not isinstance(data.get('pid'), int) or data['pid'] <= 0:
    raise SystemExit('ralph-recover: loop lock has no unambiguous positive PID')
print(data['pid'])
PY
) || die "ambiguous Ralph loop lock; refusing removal"
    [[ ! -d "/proc/$lock_pid" ]] || die "live process $lock_pid owns the Ralph loop lock"
    warn "stale loop lock detected for dead PID $lock_pid"
    if [[ "$DRY_RUN" == false ]]; then
        python3 - "$RALPH_DIR" "$lock_pid" <<'PY'
import os
import stat
import sys
root, raw_pid = sys.argv[1:]
if os.path.isdir(f'/proc/{raw_pid}'):
    raise SystemExit(f'ralph-recover: PID {raw_pid} became live; refusing loop-lock removal')
fd=os.open(root, os.O_RDONLY | os.O_DIRECTORY | getattr(os, 'O_NOFOLLOW', 0))
try:
    try: info=os.stat('loop.lock', dir_fd=fd, follow_symlinks=False)
    except FileNotFoundError: raise SystemExit(0)
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.getuid():
        raise SystemExit('ralph-recover: unsafe loop lock')
    os.unlink('loop.lock', dir_fd=fd)
    os.fsync(fd)
finally:
    os.close(fd)
PY
    fi
fi

if [[ ! -s "$SCRATCHPAD" ]]; then
    if git -C "$PROJECT_ROOT" ls-files --error-unmatch .ralph/agent/scratchpad.md >/dev/null 2>&1; then
        warn "restoring recovery scratchpad from HEAD"
        $DRY_RUN || git -C "$PROJECT_ROOT" restore --source=HEAD --worktree -- .ralph/agent/scratchpad.md
    else
        die "recovery scratchpad is missing"
    fi
fi

# Select the newest valid stream. A marker is only a candidate; it must not
# override a newer fallback stream left by a failed continuation.
event_file=""
candidates=()
if [[ -s "$EVENTS_MARKER" ]]; then
    marker_value=$(<"$EVENTS_MARKER")
    if [[ "$marker_value" == /* ]]; then
        candidates+=("$marker_value")
    else
        candidates+=("$PROJECT_ROOT/$marker_value")
    fi
fi
shopt -s nullglob
candidates+=("$RALPH_DIR"/events-*.jsonl "$RALPH_DIR"/events.jsonl)
shopt -u nullglob
ralph_real=$(realpath -e -- "$RALPH_DIR")
for candidate in "${candidates[@]}"; do
    [[ -s "$candidate" ]] || continue
    candidate_real=$(realpath -e -- "$candidate")
    [[ $(dirname -- "$candidate_real") == "$ralph_real" ]] || continue
    candidate_name=$(basename -- "$candidate_real")
    [[ "$candidate_name" =~ ^events(-[0-9]{8}-[0-9]{6})?\.jsonl$ ]] || continue
    [[ -z "$event_file" || "$candidate_real" -nt "$event_file" ]] && event_file=$candidate_real
done
[[ -n "$event_file" ]] || die "no non-empty valid Ralph event stream found under $RALPH_DIR"
event_name=$(basename -- "$event_file")
event_relative=".ralph/$event_name"

loop_id=$EXPLICIT_LOOP_ID
if [[ -z "$loop_id" && -s "$TASKS_FILE" ]]; then
    set +e
    loop_id=$(python3 - "$TASKS_FILE" <<'PY'
import json, sys
latest = {}
for line in open(sys.argv[1], encoding='utf-8'):
    try: task = json.loads(line)
    except json.JSONDecodeError: continue
    if task.get('id'): latest[task['id']] = task
ids = {t.get('loop_id') for t in latest.values() if t.get('status') in {'open', 'in_progress'} and t.get('loop_id')}
if len(ids) == 1: print(next(iter(ids)))
elif len(ids) > 1: raise SystemExit(2)
PY
)
    task_rc=$?
    set -e
    (( task_rc != 2 )) || die "multiple loops own unfinished tasks; pass --loop-id"
fi
if [[ -z "$loop_id" && -s "$LOOP_MARKER" ]]; then
    loop_id=$(tr -d '[:space:]' < "$LOOP_MARKER")
fi
if [[ -z "$loop_id" && "$event_name" =~ ^events-([0-9]{8}-[0-9]{6})\.jsonl$ ]]; then
    loop_id="primary-${BASH_REMATCH[1]}"
fi
[[ -n "$loop_id" ]] || die "cannot infer loop ID; pass --loop-id"
[[ "$loop_id" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || die "invalid loop ID '$loop_id'"

printf 'Ralph recovery plan\n  mode:   %s\n  loop:   %s\n  events: %s\n' "$MODE" "$loop_id" "$event_relative"
$DRY_RUN && { echo "Dry run: no files changed."; exit 0; }
python3 - "$RALPH_DIR" "$loop_id" "$event_relative" <<'PY'
import os
import secrets
import stat
import sys

root, loop_id, event_relative=sys.argv[1:]
dir_fd=os.open(root, os.O_RDONLY | os.O_DIRECTORY | getattr(os, 'O_NOFOLLOW', 0))
try:
    for name, value in (('current-loop-id', loop_id), ('current-events', event_relative)):
        try: existing=os.stat(name, dir_fd=dir_fd, follow_symlinks=False)
        except FileNotFoundError: existing=None
        if existing is not None and (not stat.S_ISREG(existing.st_mode) or existing.st_nlink != 1 or existing.st_uid != os.getuid()):
            raise SystemExit(f'ralph-recover: unsafe marker {name}')
        temporary=f'.{name}.{secrets.token_hex(16)}'
        out_fd=os.open(
            temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0),
            0o600, dir_fd=dir_fd,
        )
        try:
            data=(value+'\n').encode()
            view=memoryview(data)
            while view:
                written=os.write(out_fd, view)
                view=view[written:]
            os.fsync(out_fd)
        finally:
            os.close(out_fd)
        try:
            os.rename(temporary, name, src_dir_fd=dir_fd, dst_dir_fd=dir_fd)
        except BaseException:
            try: os.unlink(temporary, dir_fd=dir_fd)
            except FileNotFoundError: pass
            raise
    os.fsync(dir_fd)
finally:
    os.close(dir_fd)
PY
$PREPARE_ONLY && exit 0

case "$MODE" in
    planning) exec "$SCRIPT_DIR/ralph-plan.sh" --resume ;;
    implementation) exec "$SCRIPT_DIR/ralph-run.sh" --resume ;;
    campaign-audit) exec "$SCRIPT_DIR/ralph-audit.sh" --resume ;;
    maintenance-planning)
        selection="$PROJECT_ROOT/.factory-state/maintenance-bug-id"
        [[ -s "$selection" ]] || die "missing maintenance bug selection"
        bug_id=$(tr -d '[:space:]' < "$selection")
        exec "$SCRIPT_DIR/ralph-maintenance-plan.sh" "$bug_id" --resume
        ;;
    maintenance) exec "$SCRIPT_DIR/ralph-maintenance-run.sh" --resume ;;
esac
