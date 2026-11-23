# Vater Sovereign Terminal — Bash Shell Integration (OSC 133 + OSC 7)
# Source this file from ~/.bashrc:
#   source /usr/share/vater/shell-integration/vater.bash

if [[ -z "${VATER_BASH_INTEGRATION_LOADED:-}" ]] && [[ $- == *i* ]]; then
    VATER_BASH_INTEGRATION_LOADED=1
    __vater_cmd_running=0

    __vater_osc7() {
        local pwd_enc="${PWD//%/%25}"
        pwd_enc="${pwd_enc// /%20}"
        printf '\033]7;file://%s%s\033\\' "${HOSTNAME:-localhost}" "$pwd_enc"
    }

    __vater_preexec() {
        if [[ "$__vater_cmd_running" -eq 0 ]]; then
            __vater_cmd_running=1
            # OSC 133;C — Command execution started
            printf '\033]133;C\033\\'
        fi
    }

    __vater_precmd() {
        local last_exit="$?"
        if [[ "$__vater_cmd_running" -eq 1 ]]; then
            # OSC 133;D;<exit> — Command finished with exit code
            printf '\033]133;D;%d\033\\' "$last_exit"
            __vater_cmd_running=0
        fi
        __vater_osc7
        # OSC 133;A — Prompt start
        printf '\033]133;A\033\\'
    }

    # Wrap PS1 with OSC 133;B (prompt end / command input start)
    PS1="${PS1}\[\033]133;B\033\\\\\]"

    trap '__vater_preexec' DEBUG
    if [[ -n "${PROMPT_COMMAND:-}" ]]; then
        PROMPT_COMMAND="__vater_precmd; ${PROMPT_COMMAND}"
    else
        PROMPT_COMMAND="__vater_precmd"
    fi
fi

