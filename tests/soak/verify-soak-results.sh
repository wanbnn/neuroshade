#!/bin/sh
set -eu

[ "$#" -eq 2 ] || {
    echo "usage: verify-soak-results.sh <result-directory> <minimum-seconds>" >&2
    exit 64
}
result_dir=$1
minimum_seconds=$2

field() {
    sed -n "s/^$2=//p" "$1" | head -n 1
}

verify_one() {
    expected_mode=$1
    result="$result_dir/$expected_mode.txt"
    [ -r "$result" ] || { echo "missing soak result: $result" >&2; exit 66; }
    schema=$(field "$result" schema_version)
    mode=$(field "$result" mode)
    iterations=$(field "$result" iterations)
    elapsed=$(field "$result" elapsed_seconds)
    failures=$(field "$result" failures)
    [ "$schema" = 1 ] && [ "$mode" = "$expected_mode" ] || {
        echo "invalid $expected_mode result identity" >&2
        exit 65
    }
    case "$iterations:$elapsed:$failures:$minimum_seconds" in
        *[!0-9:]*|::*|*::*) echo "invalid numeric field in $result" >&2; exit 65;;
    esac
    [ "$iterations" -gt 0 ] && [ "$elapsed" -ge "$minimum_seconds" ] &&
        [ "$failures" -eq 0 ] || {
        echo "$expected_mode soak did not meet its gate" >&2
        exit 1
    }
    printf 'mode=%s iterations=%s elapsed_seconds=%s failures=%s verified=yes\n' \
        "$mode" "$iterations" "$elapsed" "$failures"
}

verify_one testbed
verify_one temporal
printf 'm9_soak_gate=pass minimum_seconds=%s\n' "$minimum_seconds"
