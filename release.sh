#!/usr/bin/env bash
# Builds the firmware for both boards and publishes a GitHub release that the lights
# update from (OTA): AtemTally-<board>.bin for each board plus manifest.json with the
# version, download URL and MD5 of each image.
#
#   ./release.sh                  build + publish release v<FW_VERSION>
#   NOTES="..." ./release.sh      release notes
#   DRY=1 ./release.sh            only build into build/release
set -euo pipefail
cd "$(dirname "$0")"

CLI="${ARDUINO_CLI:-/c/Program Files/Arduino CLI/arduino-cli.exe}"
FQBN="esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=min_spiffs"
REPO="tscek/atem-tally-esp32"
VER=$(sed -n 's/^#define FW_VERSION *"\([^"]*\)".*/\1/p' AtemTally.ino)
OUT=build/release
rm -rf "$OUT"
mkdir -p "$OUT"

build() {  # board name, build dir, extra compiler flags
  local flags=()
  [ -n "$3" ] && flags=(--build-property "compiler.cpp.extra_flags=$3")
  "$CLI" compile --fqbn "$FQBN" "${flags[@]}" --build-path "build/$2" .
  cp "build/$2/AtemTally.ino.bin" "$OUT/AtemTally-$1.bin"
}

entry() {  # board name
  local f="$OUT/AtemTally-$1.bin"
  printf '    "%s": {"url": "https://github.com/%s/releases/download/v%s/AtemTally-%s.bin", "md5": "%s", "size": %s}' \
    "$1" "$REPO" "$VER" "$1" "$(md5sum "$f" | cut -d' ' -f1)" "$(stat -c%s "$f")"
}

build ESP32-C6-LCD-1.47 std ""
build ESP32-C6-Touch-LCD-1.47 touch "-DBOARD_TOUCH"
{
  echo "{"
  echo "  \"version\": \"$VER\","
  echo "  \"boards\": {"
  entry ESP32-C6-LCD-1.47; echo ","
  entry ESP32-C6-Touch-LCD-1.47; echo
  echo "  }"
  echo "}"
} > "$OUT/manifest.json"
cat "$OUT/manifest.json"

[ -n "${DRY:-}" ] && exit 0
gh release create "v$VER" "$OUT"/* --repo "$REPO" --title "FW $VER" --notes "${NOTES:-FW $VER}"
