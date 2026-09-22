@echo off
setlocal

set TOOLS_DIR=D:\monitoring systen\firmware_uploader\tools
set ARDUINO_CLI=%TOOLS_DIR%\arduino-cli.exe

echo === Installing Arduino libraries ===
"%ARDUINO_CLI%" lib install "LoRa"
"%ARDUINO_CLI%" lib install "Adafruit SHT31 Library"
"%ARDUINO_CLI%" lib install "PubSubClient"
"%ARDUINO_CLI%" lib install "ArduinoJson"

echo.
echo === Done. ===
pause