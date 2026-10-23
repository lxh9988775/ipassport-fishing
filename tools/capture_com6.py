import serial, time, sys

PORT = "COM6"
BAUD = 115200
DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0
OUT = "build/boot_com6.log"

try:
    ser = serial.Serial(PORT, BAUD, timeout=0.5)
except Exception as e:
    print("OPEN_FAIL:", e)
    sys.exit(2)

# 触发一次复位（经典 DTR/RTS 脉冲），让设备重新启动并打印 boot 日志
try:
    ser.setDTR(False); ser.setRTS(True);  time.sleep(0.1)
    ser.setDTR(True);  ser.setRTS(False); time.sleep(0.2)
except Exception as e:
    print("RESET_PULSE_WARN:", e)

time.sleep(0.3)
ser.reset_input_buffer()
t0 = time.time()
buf = []
print("capturing %s for %.1fs ..." % (PORT, DUR))
try:
    while time.time() - t0 < DUR:
        line = ser.readline()
        if not line:
            continue
        try:
            s = line.decode("utf-8", "replace").rstrip("\r\n")
        except Exception:
            s = repr(line)
        buf.append(s)
        print(s)
finally:
    ser.close()

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(buf))
print("---SAVED %d lines -> %s---" % (len(buf), OUT))
