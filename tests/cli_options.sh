#!/usr/bin/env bash
# Usage: cli_options.sh <binary> <config fixture>
# Runs the binary headless with every value-taking option once. Exit status 2 means the parser rejected an
# option; a timeout (124) or a normal exit are both fine. Options that exit immediately are checked separately.
set -u
BIN="$1"; CFG="$2"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cp "$CFG" "$TMP/config.json"
fail=0
check() {  # description, args...
    local d="$1"; shift
    timeout 2 "$BIN" "$@" >"$TMP/out.txt" 2>&1
    local rc=$?
    if [ "$rc" -eq 2 ]; then echo "FAIL: $d: $(head -1 "$TMP/out.txt")"; fail=1; else echo "ok:   $d (rc=$rc)"; fi
}
check "headless + config + host + port + profile + catalog + quiet" \
    --headless --config "$TMP/config.json" --host 127.0.0.1 --port 1 --profile A32NX --catalog "$(dirname "$CFG")/../../data/a32nx_catalog.json" --quiet
check "motor-sweep with and without device" --headless --config "$TMP/config.json" --motor-sweep 0x04 --quiet
check "probe-winwing" --probe-winwing --quiet
check "dump-winwing-config" --dump-winwing-config --quiet
"$BIN" --version >/dev/null 2>&1 || { echo "FAIL: --version"; fail=1; }
"$BIN" --help 2>&1 | grep -q -- '--config' || { echo "FAIL: --help does not list --config"; fail=1; }
# the config must have been read, not rejected: the log names the profile
timeout 2 "$BIN" --headless --config "$TMP/config.json" --port 1 2>&1 | grep -q 'Config: .*profile "A32NX", 2 bindings' || { echo "FAIL: --config was not applied"; fail=1; }
exit $fail
