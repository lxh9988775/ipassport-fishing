"""不复位的实时串口记录器：抓按键后触发的 panic 现场。
用法: python capture_live.py [秒数默认120] [输出文件默认build/live.log]
关键：绝不发 DTR/RTS 复位脉冲，绝不 clear input buffer，
     设备崩了重启的完整日志（含 Guru Meditation + backtrace）都会落盘。
"""
import serial, time, sys

PORT = "COM6"
BAUD = 115200
DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 120.0
OUT = sys.argv[2] if len(sys.argv) > 2 else "build/live.log"

ser = serial.Serial(PORT, BAUD, timeout=0.2)   # 打开串口本身不复位（不碰 DTR/RTS）
print("LIVE capturing %s for %.0fs -> %s (NO reset pulse)" % (PORT, DUR, OUT))
t0 = time.time()
buf = []
try:
    while time.time() - t0 < DUR:
        chunk = ser.read(4096)
        if chunk:
            s = chunk.decode("utf-8", "replace")
            buf.append(s)
finally:
    ser.close()

text = "".join(buf)
with open(OUT, "w", encoding="utf-8", errors="replace") as f:
    f.write(text)
print("---SAVED %d bytes -> %s---" % (len(text), OUT))
