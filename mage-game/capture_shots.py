"""抓取 AIpassport 真机截图（FAP_SCREENSHOT_V1 串口帧缓冲）。

用法：
    python mage-game/capture_shots.py            # 抓 4 张：标题/城镇/战斗/伙伴
    python mage-game/capture_shots.py --out 目录  # 指定输出目录

前置：设备已刷入含 main/fap_screenshot.c 的固件，且数据线插着（COM6）。
每抓一张前会提示你把设备导航到对应画面。
"""
import argparse
import os
import struct
import sys
import time

try:
    import serial
except ImportError:
    print("缺少 pyserial：pip install pyserial")
    sys.exit(1)

CMD = b"FAP_SCREENSHOT_V1"
HDR_PREFIX = b"FAP_SCREENSHOT_V1 "

# 4 张要抓的画面，对应设备上要导航到的位置
SHOTS = [
    ("01-title", "标题页（开机默认画面）"),
    ("02-town", "魔法小镇（按 OK 进入冒险）"),
    ("03-battle", "战斗画面（进野外试炼打一场）"),
    ("04-partner", "伙伴页（长按 OK 打开功能栏 -> 伙伴）"),
]


def parse_header(line: bytes):
    """解析 'FAP_SCREENSHOT_V1 240 320 RGB565LE 153600'。"""
    parts = line.split()
    if len(parts) != 5:
        raise ValueError(f"无法解析截图头: {line!r}")
    _, w, h, enc, length = parts
    return int(w), int(h), enc.decode(), int(length)


def rgb565_to_png(w, h, payload: bytes) -> bytes:
    """把 RGB565LE 原始帧转成 PNG（纯标准库实现，免装 Pillow）。"""
    import zlib
    rows = []
    stride = w * 2
    for y in range(h):
        row = bytearray(b"\x00")  # PNG filter type 0
        base = y * stride
        for x in range(w):
            v = payload[base + x * 2] | (payload[base + x * 2 + 1] << 8)
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 2), (b << 3) | (b >> 2)))
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # 8bit truecolor
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def capture(port, out_path, timeout=90):
    with serial.Serial(port, baudrate=115200, timeout=0.05) as s:
        s.dtr = False
        s.rts = False
        time.sleep(1.0)
        s.reset_input_buffer()
        s.write(CMD)
        s.flush()

        deadline = time.time() + timeout
        buf = bytearray()
        header = None
        need = 0
        while time.time() < deadline:
            data = s.read(65536)
            if not data:
                continue
            buf += data
            if header is None:
                idx = buf.find(HDR_PREFIX)
                if idx >= 0:
                    nl = buf.find(b"\n", idx)
                    if nl > 0:
                        header = bytes(buf[idx:nl + 1])
                        w, h, enc, need = parse_header(header)
            if header and len(buf) - len(header) >= need:
                break

        if not header:
            raise RuntimeError(f"设备无响应（收到 {len(buf)} 字节）。设备是否已刷入带截屏协议的固件？")

        got = len(buf) - len(header)
        if got < need:
            raise RuntimeError(f"数据不完整：收到 {got}/{need} 字节（{got * 100.0 / need:.0f}%）")

        w, h, enc, need = parse_header(header)
        px = bytes(buf[len(header):len(header) + need])
        if enc == "RGB565LE":
            png = rgb565_to_png(w, h, px)
        else:
            png = px

        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        with open(out_path, "wb") as f:
            f.write(png)
        return w, h, len(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM6")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                  "submission", "shots"))
    args = ap.parse_args()

    print(f"逐张抓取 {len(SHOTS)} 张 3:4 真机截图（每张都是 240x320，天然 3:4）\n")
    ok = 0
    for name, desc in SHOTS:
        print(f"→ {name}  ({desc})")
        print("   请现在把设备切换到该画面，然后按回车开始抓取…")
        try:
            input()
        except EOFError:
            pass
        path = os.path.join(args.out, f"{name}.png")
        try:
            w, h, size = capture(args.port, path)
            print(f"   ✓ 已保存 {path}  ({w}x{h}, {size} 字节)\n")
            ok += 1
        except Exception as e:
            print(f"   ✗ 失败：{e}\n")

    print(f"完成：{ok}/{len(SHOTS)} 张")
    return 0 if ok == len(SHOTS) else 1


if __name__ == "__main__":
    sys.exit(main())
