# Epimone ZDOTDIR shim (.zprofile): login shells. See .zshenv for the scheme.
ZDOTDIR="${EPIMONE_ZDOTDIR_REAL:-$HOME}"
[ -f "$ZDOTDIR/.zprofile" ] && source "$ZDOTDIR/.zprofile"
ZDOTDIR="$EPIMONE_OURS_ZDOTDIR"
