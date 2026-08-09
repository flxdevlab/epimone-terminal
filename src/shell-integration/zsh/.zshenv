# Epimone ZDOTDIR shim (.zshenv).
#
# Epimone points ZDOTDIR at this directory so it can source its integration
# without editing the user's dotfiles. Each shim loads the user's real file of
# the same name, keeping ZDOTDIR pointed here so zsh still finds Epimone's
# .zshrc next. EPIMONE_ZDOTDIR_REAL is the user's original ZDOTDIR (or $HOME).

: "${EPIMONE_ZDOTDIR_REAL:=$HOME}"
EPIMONE_OURS_ZDOTDIR="$ZDOTDIR"
ZDOTDIR="$EPIMONE_ZDOTDIR_REAL"
[ -f "$ZDOTDIR/.zshenv" ] && source "$ZDOTDIR/.zshenv"
ZDOTDIR="$EPIMONE_OURS_ZDOTDIR"
