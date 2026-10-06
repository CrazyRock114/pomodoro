<p align="right">
  <a href="pomodoro-install.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Pomodoro Firmware Installation

Install the three-button Pomodoro firmware on any FoloToy AI Passport
(ESP32-C3, 8 MB Flash) by flashing the merged image from address `0x0`.

## What you need

- A computer with [esptool](https://docs.espressif.com/projects/esptool/) installed
  (`pip install esptool`).
- A data-capable USB cable (charge-only cables will not enumerate).
- The merged image `FoloToy-AI-Passport-full.bin` from the
  [GitHub release](https://github.com/CrazyRock114/pomodoro/releases), or a local
  `build/FoloToy-AI-Passport-full.bin` produced by `./tools/validate.sh --firmware`.

## Install (macOS / Linux)

```bash
./tools/install-firmware.sh FoloToy-AI-Passport-full.bin
```

The script auto-detects the USB serial port; pass the port explicitly with
`./tools/install-firmware.sh <image> <port>` when several devices are attached.

## Install (Windows)

```bat
tools\install-firmware.bat FoloToy-AI-Passport-full.bin COM5
```

Or use Espressif's **Flash Download Tool**: select chip type ESP32-C3, load the
merged image at offset `0x0`, and press START.

## Notes

- The device reboots into the Pomodoro app automatically after flashing.
- Flashing the merged image overwrites the flash region from `0x0` including
  NVS; any settings stored by a previous firmware are reset.
- Buttons: UP/DOWN adjust the minutes (hold to repeat), OK starts/pauses, hold
  OK on the setup screen opens settings (step size, alert mode, volume,
  auto-sleep timing). The device sleeps automatically when idle and wakes with
  any button.

Related: [release page](https://github.com/CrazyRock114/pomodoro/releases).
