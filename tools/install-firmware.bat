@echo off
REM Flash the Pomodoro merged image to an AI Passport (ESP32-C3) from 0x0.
REM Usage: install-firmware.bat [image] [port]
setlocal
set "BIN=%~1"
if "%BIN%"=="" set "BIN=build\FoloToy-AI-Passport-full.bin"
if not exist "%BIN%" (
    echo ERROR: image not found: %BIN%
    echo Download FoloToy-AI-Passport-full.bin from the releases page, or
    echo build it with tools\validate.cmd / validate.sh --firmware
    exit /b 1
)
set "PORT=%~2"
if "%PORT%"=="" (
    echo ERROR: pass the COM port explicitly: install-firmware.bat %BIN% COM5
    exit /b 1
)
python -m esptool --chip esp32c3 -p %PORT% -b 460800 --before default_reset --after hard_reset write_flash 0x0 "%BIN%"
endlocal
