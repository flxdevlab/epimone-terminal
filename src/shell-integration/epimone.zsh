# epimone.zsh: Epimone shell integration for zsh.
#
# Same purpose as epimone.bash: OSC 7 (cwd reporting) and OSC 133
# prompt/command/output marks. Epimone injects this via a private
# ZDOTDIR whose .zshrc sources the user's real .zshrc and then this file, so
# the user's own dotfiles are never modified.
#
# Only activates inside Epimone; safe to source more than once; uses
# add-zsh-hook so it never clobbers the user's precmd/preexec/chpwd hooks.

[[ -n "${EPIMONE:-}" ]] || return 0
[[ -n "${EPIMONE_ZSH_INTEGRATION:-}" ]] && return 0
EPIMONE_ZSH_INTEGRATION=1

# Byte-wise percent-encoder (sh-emulated so ${..%%..} works one byte at a time).
__epimone_urlencode() {
  emulate -L sh
  LC_ALL=C
  local str="$1" safe
  while [ -n "$str" ]; do
    safe="${str%%[!a-zA-Z0-9/:_.~-]*}"
    printf '%s' "$safe"
    str="${str#"$safe"}"
    if [ -n "$str" ]; then
      printf '%%%02X' "'$str"
      str="${str#?}"
    fi
  done
}

__epimone_osc7() {
  printf '\033]7;file://%s%s\a' "${HOST:-${HOSTNAME:-}}" "$(__epimone_urlencode "$PWD")"
}

# The zero-width OSC 133 B mark (end of prompt / start of input), wrapped in
# %{ %} so zsh counts it as zero-width. It is appended to the prompt in precmd
# (which runs last, since this file is sourced after the user's .zshrc), never
# at load time, so the user's full prompt, including the working-directory
# segment, is preserved even for themes that rebuild PROMPT every precmd.
__epimone_b=$'%{\033]133;B\a%}'

# Before each prompt: OSC 133 D (last exit) + OSC 7 (cwd) + OSC 133 A (prompt),
# then re-append exactly one B to whatever PROMPT the user's theme produced
# (stripping any previous one so it never accumulates or is duplicated).
__epimone_precmd() {
  local st=$?
  printf '\033]133;D;%s\a' "$st"
  __epimone_osc7
  printf '\033]133;A\a'
  PS1="${PS1//"$__epimone_b"/}${__epimone_b}"
}

# Just before a command runs: OSC 133 C (output start).
__epimone_preexec() {
  printf '\033]133;C\a'
}

autoload -Uz add-zsh-hook
add-zsh-hook precmd  __epimone_precmd
add-zsh-hook preexec __epimone_preexec
add-zsh-hook chpwd   __epimone_osc7
