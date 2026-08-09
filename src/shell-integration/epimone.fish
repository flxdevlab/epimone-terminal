# epimone.fish: Epimone shell integration for fish (OSC 7 + OSC 133).
#
# NOTE: modern fish (>= 3.2) already emits OSC 7 and OSC 133 automatically when
# it detects a capable terminal, so Epimone does NOT auto-inject this file (to
# avoid double emission). It is shipped for reference and for older fish; source
# it manually if you want. Guarded to only run inside Epimone.

if status is-interactive; and set -q EPIMONE; and not set -q EPIMONE_FISH_INTEGRATION
    set -g EPIMONE_FISH_INTEGRATION 1

    function __epimone_osc7 --on-variable PWD --on-event fish_prompt
        printf '\033]7;file://%s%s\a' (hostname) "$PWD"
    end
    function __epimone_osc133_preexec --on-event fish_preexec
        printf '\033]133;C\a'
    end
    function __epimone_osc133_postexec --on-event fish_postexec
        printf '\033]133;D;%s\a' $status
    end
end
