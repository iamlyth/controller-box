#!/usr/bin/env bash
# Source-only helper for the cross-process single-writer lock.

factory_lock_acquire() {
    local lock_path=${1:?factory lock path required}

    if [[ ${FACTORY_LOCK_HELD:-0} == 1 ]]; then
        if { : >&9; } 2>/dev/null; then
            return 0
        fi
        unset FACTORY_LOCK_HELD
    fi

    exec 9> "$lock_path"
    if ! flock -n 9; then
        echo "factory-lock: another planner, worker, or recovery process is active" >&2
        return 1
    fi
    export FACTORY_LOCK_HELD=1
}
