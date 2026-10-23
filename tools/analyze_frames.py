import cv2, numpy as np, sys

src = sys.argv[1]
cap = cv2.VideoCapture(src)
rows = []
idx = 0
while True:
    ret, frame = cap.read()
    if not ret:
        break
    # 屏幕区域（由抽帧目测）：x 60..390, y 290..640
    crop = frame[290:640, 60:390]
    gray = cv2.cvtColor(crop, cv2.COLOR_BGR2GRAY)
    rows.append((idx, float(gray.mean()), float(gray.std())))
    idx += 1
cap.release()

print("frame mean std")
prev = None
for i, m, s in rows:
    mark = ""
    if prev is not None:
        if m - prev > 40:
            mark = "  <== BRIGHT JUMP"
        elif prev - m > 40:
            mark = "  <== DARK JUMP"
    if i % 5 == 0 or mark:
        print(f"{i:4d} {m:6.1f} {s:6.1f}{mark}")
    prev = m
