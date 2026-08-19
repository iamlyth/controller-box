#!/usr/bin/env bash
# Source-only helper for the cross-process single-writer lock.

FACTORY_LOCK_HELPER_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

factory_lock_bootstrap() {
    local lock_path=${1:?factory lock path required}
    shift
    (( $# > 0 )) || { echo "factory-lock: lifecycle command required" >&2; return 2; }

    if [[ ${FACTORY_LOCK_HELD:-0} == 1 ]]; then
        python3 "$FACTORY_LOCK_HELPER_DIR/factory-lock-exec.py" "$lock_path" -- true
        return $?
    fi
    exec python3 "$FACTORY_LOCK_HELPER_DIR/factory-lock-exec.py" "$lock_path" -- "$@"
}

factory_lock_acquire() {
    local lock_path=${1:?factory lock path required}
    if [[ ${FACTORY_LOCK_HELD:-0} != 1 ]]; then
        echo "factory-lock: lifecycle did not bootstrap the factory lock" >&2
        return 1
    fi
    python3 "$FACTORY_LOCK_HELPER_DIR/factory-lock-exec.py" "$lock_path" -- true
}
