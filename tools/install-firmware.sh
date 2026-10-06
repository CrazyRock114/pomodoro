#!/usr/bin/env bash
# Flash the Pomodoro merged image to an AI Passport (ESP32-C3) from 0x0.
# Usage: install-firmware.sh [image] [port]
#   image  defaults to build/FoloToy-AI-Passport-full.bin or the .bin given
#   port   auto-detected from /dev/cu.usbmodem* / /dev/ttyACM* / /dev/ttyUSB*
set -euo pipefail

BIN="${1:-build/FoloToy-AI-Passport-full.bin}"
if [[ ! -f "$BIN" ]]; then
    echo "ERROR: image not found: $BIN" >&2
    echo "Download FoloToy-AI-Passport-full.bin from the releases page, or" >&2
    echo "build it with ./tools/validate.sh --firmware" >&2
    exit 1
fi

if [[ $# -ge 2 ]]; then
    PORT="$2"
else
    PORT=""
    if command -v python >/dev/null 2>&1; then
        PORT="$(python -m serial.tools.list_ports -q 2>/dev/null | grep -iE 'usbmodem|usbserial|ttyACM|ttyUSB' | head -1 || true)"
    fi
    if [[ -z "$PORT" ]]; then
        for p in /dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyACM* /dev/ttyUSB*; do
            [[ -e "$p" ]] && PORT="$p" && break
        done
    fi
    if [[ -z "$PORT" ]]; then
        echo "ERROR: no serial port detected. Connect the device and retry," >&2
        echo "or pass the port explicitly: install-firmware.sh <image> <port>" >&2
        exit 1
    fi
fi

echo "Image: $BIN"
echo "Port:  $PORT"
python -m esptool --chip esp32c3 -p "$PORT" -b 460800 \
    --before default_reset --after hard_reset \
    write_flash 0x0 "$BIN"
