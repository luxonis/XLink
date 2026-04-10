#!/bin/bash
# Source: http://www.bashcookbook.com/bashinfo/source/bash-4.0/examples/scripts/timeout3

scriptName="${0##*/}"

declare -i DEFAULT_TIMEOUT=9
declare -i DEFAULT_INTERVAL=1
declare -i DEFAULT_DELAY=1

# Timeout.
declare -i timeout=DEFAULT_TIMEOUT
# Interval between checks if the process is still alive.
declare -i interval=DEFAULT_INTERVAL
# Delay between posting the SIGTERM signal and destroying the process by SIGKILL.
declare -i delay=DEFAULT_DELAY

function printUsage() {
    cat <<EOF

Synopsis
    $scriptName [-t timeout] [-i interval] [-d delay] command
    Execute a command with a time-out.
    Upon time-out expiration SIGTERM (15) is sent to the process. If SIGTERM
    signal is blocked, then the subsequent SIGKILL (9) terminates it.

    -t timeout
        Number of seconds to wait for command completion.
        Default value: $DEFAULT_TIMEOUT seconds.

    -i interval
        Interval between checks if the process is still alive.
        Positive integer, default value: $DEFAULT_INTERVAL seconds.

    -d delay
        Delay between posting the SIGTERM signal and destroying the
        process by SIGKILL. Default value: $DEFAULT_DELAY seconds.

As of today, Bash does not support floating point arithmetic (sleep does),
therefore all delay/time values must be integers.
EOF
}

# Options.
while getopts ":t:i:d:" option; do
    case "$option" in
        t) timeout=$OPTARG ;;
        i) interval=$OPTARG ;;
        d) delay=$OPTARG ;;
        *) printUsage; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

# $# should be at least 1 (the command to execute), however it may be strictly
# greater than 1 if the command itself has options.
if (($# == 0 || interval <= 0)); then
    printUsage
    exit 1
fi

if command -v setsid >/dev/null 2>&1; then
    setsid "$@" &
else
    "$@" &
fi
commandPid=$!

cleanup() {
    if kill -0 "$commandPid" 2>/dev/null; then
        kill -s SIGTERM -- "-$commandPid" 2>/dev/null || kill -s SIGTERM "$commandPid" 2>/dev/null || true
        sleep "$delay"
        kill -s SIGKILL -- "-$commandPid" 2>/dev/null || kill -s SIGKILL "$commandPid" 2>/dev/null || true
    fi
}

teardown() {
    cleanup
    if [[ -n "${watchdogPid:-}" ]]; then
        kill "$watchdogPid" 2>/dev/null || true
    fi
}

handleTermination() {
    trap - EXIT TERM INT
    teardown
    exit 124
}
trap teardown EXIT
trap handleTermination TERM INT

# kill -0 pid   Exit code indicates if a signal may be sent to $pid process.
(
    trap 'exit 0' TERM INT
    ((t = timeout))

    while ((t > 0)); do
        sleep "$interval"
        kill -0 "$commandPid" 2>/dev/null || exit 0
        ((t -= interval))
    done

    cleanup
    exit 124
) 2> /dev/null &
watchdogPid=$!

wait "$commandPid"
commandStatus=$?
trap - EXIT TERM INT

if ((commandStatus >= 128)); then
    wait "$watchdogPid" 2>/dev/null
    watchdogStatus=$?
    if [[ "$watchdogStatus" -eq 124 ]]; then
        exit 124
    fi
fi

kill "$watchdogPid" 2>/dev/null || true
exit "$commandStatus"
