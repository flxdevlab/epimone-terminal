# Epimone ZDOTDIR shim (.zshrc): interactive shells.
#
# Load the user's real .zshrc, then Epimone's integration. After this, leave
# ZDOTDIR restored to the user's real directory for the rest of the session
# (so nested zsh and any $ZDOTDIR references behave normally).
ZDOTDIR="${EPIMONE_ZDOTDIR_REAL:-$HOME}"
[ -f "$ZDOTDIR/.zshrc" ] && source "$ZDOTDIR/.zshrc"
[ -n "$EPIMONE_SHELL_INTEGRATION_DIR" ] && source "$EPIMONE_SHELL_INTEGRATION_DIR/epimone.zsh"
