#!/usr/bin/env bash
set -euo pipefail

PROVIDER=${OLLAMA_PROVIDER:-ollama}
MODEL=${OLLAMA_MODEL:-glm-5.2}
args=("$@")
prompt_prefix="Please read and execute the task in "

# Ralph may place a large prompt in host /tmp. Open it before Pi2 enters its
# private bubblewrap /tmp and stream it through stdin.
if (( ${#args[@]} > 0 )); then
    last_index=$(( ${#args[@]} - 1 ))
    prompt_arg=${args[$last_index]}
    if [[ "$prompt_arg" == "$prompt_prefix"* ]]; then
        prompt_file=${prompt_arg#"$prompt_prefix"}
        if [[ -r "$prompt_file" ]]; then
            unset "args[$last_index]"
            exec pi2 --provider "$PROVIDER" --model "$MODEL" "${args[@]}" < "$prompt_file"
        fi
    fi
fi

exec pi2 --provider "$PROVIDER" --model "$MODEL" "${args[@]}"
