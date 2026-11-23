# Vater Sovereign Terminal — Zsh Shell Integration (OSC 133 + OSC 7)
# Source this file from ~/.zshrc:
#   source /usr/share/vater/shell-integration/vater.zsh

if [[ -z "${VATER_ZSH_INTEGRATION_LOADED:-}" ]] && [[ -o interactive ]]; then
    VATER_ZSH_INTEGRATION_LOADED=1
    autoload -Uz add-zsh-hook
    typeset -g __vater_cmd_running=0

    __vater_osc7() {
        local pwd_enc="${PWD//%/%25}"
        pwd_enc="${pwd_enc// /%20}"
        printf '\033]7;file://%s%s\033\\' "${HOST:-localhost}" "$pwd_enc"
    }

    __vater_preexec() {
        __vater_cmd_running=1
        # OSC 133;C — Command execution started
        printf '\033]133;C\033\\'
    }

    __vater_precmd() {
        local last_exit="$?"
        if (( __vater_cmd_running )); then
            # OSC 133;D;<exit> — Command finished with exit code
            printf '\033]133;D;%d\033\\' "$last_exit"
            __vater_cmd_running=0
        fi
        __vater_osc7
        # OSC 133;A — Prompt start
        printf '\033]133;A\033\\'
    }

    PROMPT="${PROMPT}%{\033]133;B\033\\%}"
    add-zsh-hook precmd __vater_precmd
    add-zsh-hook preexec __vater_preexec
fi

