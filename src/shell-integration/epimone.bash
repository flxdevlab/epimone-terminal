# epimone.bash: Epimone shell integration for bash.
#
# Provides:
#   * OSC 7: reports the working directory to the terminal on every prompt
#            and directory change, so Epimone knows each pane's cwd (this is
#            what makes "split inherits the current directory" work reliably
#            on distributions that do not wire OSC 7 up by default).
#   * OSC 133: prompt/command/output boundary marks (A/B/C/D), a foundation
#              for future command-block features.
#
# Epimone injects this as bash's startup file via `bash --rcfile <this>` and
# sets EPIMONE_BASH_RCFILE=1; because that replaces the user's personal
# ~/.bashrc, this file sources it back first (the system /etc/bash.bashrc is
# still sourced by bash itself, independent of --rcfile). The file is also safe
# to `source` manually from an existing rc: it never recurses and never
# clobbers an existing PROMPT_COMMAND / PS1 / DEBUG trap; it appends/chains.

# --- Injection bootstrap: restore the user's normal bash startup ------------
if [ -n "${EPIMONE_BASH_RCFILE:-}" ]; then
  unset EPIMONE_BASH_RCFILE
  if [ -r "$HOME/.bashrc" ]; then
    source "$HOME/.bashrc"
  fi
fi

# --- Integration (only inside Epimone, only once) ---------------------------
if [ -n "${EPIMONE:-}" ] && [ -z "${EPIMONE_BASH_INTEGRATION:-}" ]; then
  EPIMONE_BASH_INTEGRATION=1

  # Byte-wise percent-encoder. Runs in a subshell so LC_ALL stays local, which
  # keeps the ${..%%..} manipulation working one byte at a time (correct for
  # UTF-8 paths). Modeled on VTE's own vte.sh.
  __epimone_urlencode() (
    LC_ALL=C
    str="$1"
    while [ -n "$str" ]; do
      safe="${str%%[!a-zA-Z0-9/:_.~-]*}"
      printf '%s' "$safe"
      str="${str#"$safe"}"
      if [ -n "$str" ]; then
        printf '%%%02X' "'$str"
        str="${str#?}"
      fi
    done
  )

  __epimone_osc7() {
    printf '\033]7;file://%s%s\a' "${HOSTNAME:-}" "$(__epimone_urlencode "$PWD")"
  }

  # The zero-width OSC 133 B mark (end of prompt / start of user input). The
  # \[ \] wrap keeps bash's line-editing width math correct. This is never
  # written into the user's stored PS1 at load time; instead it is stripped
  # before the user's prompt logic runs and re-appended after, every prompt.
  # That preserves the user's PS1 verbatim (including \w and colors), survives
  # setups that rebuild PS1 in PROMPT_COMMAND, and never accumulates.
  __epimone_b='\[\e]133;B\a\]'

  # First in PROMPT_COMMAND: capture the exit code, remove any leftover mark so
  # the user's prompt logic sees a pristine PS1, then emit OSC 133 D + OSC 7 +
  # OSC 133 A. $? is returned so a chained user PROMPT_COMMAND still sees it.
  __epimone_prompt() {
    local __epimone_status=$?
    PS1=${PS1//"$__epimone_b"/}
    printf '\033]133;D;%s\a' "$__epimone_status"
    __epimone_osc7
    printf '\033]133;A\a'
    return "$__epimone_status"
  }

  # Last in PROMPT_COMMAND: append exactly one B to whatever PS1 the user's rc /
  # PROMPT_COMMAND produced, and arm the preexec (C) mark.
  __epimone_preexec_done=1
  __epimone_finish() {
    case "$PS1" in
      *'133;B'*) : ;;
      *) PS1="${PS1}${__epimone_b}" ;;
    esac
    __epimone_preexec_done=0
  }

  # DEBUG trap → OSC 133 C (command about to run). Fires once per prompt cycle.
  __epimone_debug() {
    [ -n "${COMP_LINE:-}" ] && return 0                 # tab-completion, not exec
    [ "$__epimone_preexec_done" = 1 ] && return 0       # already marked / in prompt
    __epimone_preexec_done=1
    printf '\033]133;C\a'
  }
  # Do not clobber an existing DEBUG trap (e.g. bash-preexec); if one is set we
  # skip the C mark rather than break the user's setup.
  if [ -z "$(trap -p DEBUG)" ]; then
    trap '__epimone_debug' DEBUG
  fi

  # Chain into PROMPT_COMMAND without clobbering: ours first (clean PS1 +
  # capture $?), the user's, then ours last (re-append the mark).
  case ";${PROMPT_COMMAND:-};" in
    *";__epimone_prompt;"*|*"__epimone_prompt "*) : ;;   # already chained
    *) PROMPT_COMMAND="__epimone_prompt${PROMPT_COMMAND:+;$PROMPT_COMMAND};__epimone_finish" ;;
  esac
fi
