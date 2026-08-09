# Epimone ZDOTDIR shim (.zlogin): login shells, read last. See .zshenv.
#
# Without this passthrough, turning on "Run as login shell" would stop the
# user's own ~/.zlogin from running, because ZDOTDIR points here and not at
# their home directory.
ZDOTDIR="${EPIMONE_ZDOTDIR_REAL:-$HOME}"
[ -f "$ZDOTDIR/.zlogin" ] && source "$ZDOTDIR/.zlogin"
ZDOTDIR="$EPIMONE_OURS_ZDOTDIR"
