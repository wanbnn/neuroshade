#!/bin/sh
set -eu

prefix=
timeout_seconds=120

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix) prefix=$2; shift 2 ;;
        --timeout) timeout_seconds=$2; shift 2 ;;
        --) shift; break ;;
        *) echo "usage: run-contained-wine.sh --prefix DIR [--timeout SECONDS] -- COMMAND [ARG...]" >&2; exit 64 ;;
    esac
done

[ -n "$prefix" ] && [ -d "$prefix" ] || {
    echo "Wine prefix is not a directory: $prefix" >&2
    exit 66
}
[ "$#" -gt 0 ] || { echo "contained Wine command is missing" >&2; exit 64; }
case "$timeout_seconds" in
    ''|*[!0-9]*) echo "timeout must be a positive integer" >&2; exit 64 ;;
esac
[ "$timeout_seconds" -gt 0 ] || { echo "timeout must be positive" >&2; exit 64; }
command -v systemd-run >/dev/null 2>&1 || { echo "systemd-run is required" >&2; exit 69; }
systemctl --user is-system-running >/dev/null 2>&1 || {
    echo "the systemd user manager is not running" >&2
    exit 69
}

prefix=$(readlink -f "$prefix")

prefix_pids() {
    for environment in /proc/[0-9]*/environ; do
        [ -r "$environment" ] || continue
        if (tr '\000' '\n' <"$environment") 2>/dev/null |
                grep -Fqx "WINEPREFIX=$prefix"; then
            process=${environment#/proc/}
            printf '%s\n' "${process%/environ}"
        fi
    done
}

existing=$(prefix_pids)
if [ -n "$existing" ]; then
    echo "refusing to start: Wine prefix already has live process(es): $existing" >&2
    exit 75
fi

unit="neuroshade-proton-$(id -u)-$$"
cleanup() {
    systemctl --user kill --kill-whom=all "$unit.service" >/dev/null 2>&1 || true
    systemctl --user stop "$unit.service" >/dev/null 2>&1 || true
}
trap cleanup EXIT HUP INT TERM

set +e
systemd-run --user --quiet --wait --collect --pipe \
    --unit="$unit" \
    --service-type=exec \
    --property=KillMode=control-group \
    --property=TimeoutStopSec=5s \
    --property="RuntimeMaxSec=${timeout_seconds}s" \
    --setenv="WINEPREFIX=$prefix" \
    -- "$@"
result=$?
set -e

cleanup
attempt=0
while remaining=$(prefix_pids) && [ -n "$remaining" ]; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 50 ]; then
        echo "contained unit left Wine-prefix processes alive: $remaining" >&2
        exit 70
    fi
    sleep 0.1
done
trap - EXIT HUP INT TERM

exit "$result"
