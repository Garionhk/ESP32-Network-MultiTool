#!/usr/bin/env bash
# Compiles the sketch's pure logic natively and runs the assertions. No board
# required.
#
# The real source files are compiled in -- nothing is copied or reimplemented --
# against the small Arduino shim in shim/. The transport (ntfy), the blocklist
# engine, the DNS server and the scheduler are stubbed inside test_commands.cpp
# so replies can be inspected.
#
#     ./test/run.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKETCH="$HERE/../MultiTool"

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

JSON="${ARDUINOJSON_SRC:-$HOME/Documents/Arduino/libraries/ArduinoJson/src}"
if [ ! -d "$JSON" ]; then
  echo "ArduinoJson not found at $JSON" >&2
  echo "Set ARDUINOJSON_SRC to its src/ directory." >&2
  exit 1
fi

FLAGS=(
  -std=gnu++17 -w
  -DARDUINOJSON_ENABLE_ARDUINO_STRING=1
  -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0
  -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0
  -DARDUINOJSON_ENABLE_PROGMEM=0
  -DARDUINOJSON_ENABLE_STD_STRING=0
  -I "$HERE/shim" -I "$SKETCH" -I "$JSON"
)

echo "=== settings ==="
g++ "${FLAGS[@]}" -o "$OUT/test_settings" "$HERE/test_settings.cpp" "$SKETCH/settings.cpp"
"$OUT/test_settings"

echo
echo "=== schedule ==="
g++ "${FLAGS[@]}" -o "$OUT/test_schedule" "$HERE/test_schedule.cpp"
"$OUT/test_schedule"

echo
echo "=== DNS messages ==="
g++ "${FLAGS[@]}" -o "$OUT/test_dnsmsg" "$HERE/test_dnsmsg.cpp"
"$OUT/test_dnsmsg"

echo
echo "=== local DNS records ==="
g++ "${FLAGS[@]}" -o "$OUT/test_localrec" "$HERE/test_localrec.cpp"
"$OUT/test_localrec"

echo
echo "=== status LED ==="
g++ "${FLAGS[@]}" -o "$OUT/test_led" "$HERE/test_led.cpp"
"$OUT/test_led"

echo
echo "=== blocklist parsing ==="
g++ "${FLAGS[@]}" -o "$OUT/test_blparse" "$HERE/test_blparse.cpp"
"$OUT/test_blparse"

echo
echo "=== wake-on-lan store ==="
g++ "${FLAGS[@]}" -o "$OUT/test_wolstore" "$HERE/test_wolstore.cpp" "$SKETCH/wolstore.cpp"
"$OUT/test_wolstore"

echo
echo "=== commands ==="
g++ "${FLAGS[@]}" -o "$OUT/test_commands" \
    "$HERE/test_commands.cpp" "$SKETCH/commands.cpp" "$SKETCH/adblock_cmd.cpp" \
    "$SKETCH/wol_cmd.cpp" "$SKETCH/wolstore.cpp" \
    "$SKETCH/settings.cpp"
"$OUT/test_commands"
