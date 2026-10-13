#!/bin/bash
# AIpassport 固件烧录 + 真机截图 一体脚本（ESP32-C3 / COM6）
# 依赖：publisher venv 里的 esptool + pyserial（已装）
set -e

PY="C:/Users/8605464/.workbuddy/binaries/python/envs/publisher/Scripts/python.exe"
PROJ="C:/Users/8605464/Desktop/AIpassport"
PUB="$PY;C:/Users/8605464/.workbuddy/skills/folotoy-ai-passport-publisher/scripts/publisher.py"
PORT="COM6"
BIN="$PROJ/mage-game/FoloToy-AI-Passport-full.bin"
SHOT_DIR="$PROJ/mage-game/submission/shots"

case "$1" in
  flash)
    echo ">>> 烧录 $BIN 到 $PORT"
    "$PY" -m esptool --chip esp32c3 --port "$PORT" --baud 460800 \
      write_flash 0x0 "$BIN"
    echo ">>> 烧录完成，设备自动复位"
    ;;
  shot)
    mkdir -p "$SHOT_DIR"
    name="${2:-shot}"
    echo ">>> 抓取截图 -> $SHOT_DIR/$name.png"
    "$PY" "$PUB" capture-screen --port "$PORT" --output "$SHOT_DIR/$name.png"
    ;;
  probe)
    "$PY" -m esptool --chip esp32c3 --port "$PORT" chip_id
    ;;
  *)
    echo "用法: bash flash_mage.sh {flash|shot [名称]|probe}"
    exit 1
    ;;
esac
