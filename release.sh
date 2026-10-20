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
FQBN_S3="esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB"
REPO="tscek/atem-tally-esp32"
VER=$(sed -n 's/^#define FW_VERSION *"\([^"]*\)".*/\1/p' AtemTally.ino)
OUT=build/release
rm -rf "$OUT"
mkdir -p "$OUT"

build() {  # image name (chip), build dir, FQBN
  "$CLI" compile --fqbn "$3" --build-path "build/$2" .
  cp "build/$2/AtemTally.ino.bin" "$OUT/AtemTally-$1.bin"
}

entry() {  # manifest key, image name (chip)
  local f="$OUT/AtemTally-$2.bin"
  printf '    "%s": {"url": "https://github.com/%s/releases/download/v%s/AtemTally-%s.bin", "md5": "%s", "size": %s}' \
    "$1" "$REPO" "$VER" "$2" "$(md5sum "$f" | cut -d' ' -f1)" "$(stat -c%s "$f")"
}

# One image per chip; the firmware recognises the board at boot.
build ESP32-C6 c6 "$FQBN"
build ESP32-S3 s3 "$FQBN_S3"
{
  echo "{"
  echo "  \"version\": \"$VER\","
  echo "  \"boards\": {"
  entry ESP32-C6 ESP32-C6; echo ","
  entry ESP32-S3 ESP32-S3; echo ","
  # FW 2.4 and older look their board up by board name
  entry ESP32-C6-LCD-1.47 ESP32-C6; echo ","
  entry ESP32-C6-Touch-LCD-1.47 ESP32-C6; echo ","
  entry ESP32-S3-LCD-1.47B ESP32-S3; echo
  echo "  }"
  echo "}"
} > "$OUT/manifest.json"
cat "$OUT/manifest.json"

[ -n "${DRY:-}" ] && exit 0
gh release create "v$VER" "$OUT"/* --repo "$REPO" --title "FW $VER" --notes "${NOTES:-FW $VER}"
