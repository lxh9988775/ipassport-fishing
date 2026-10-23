import cv2, os, sys

src = sys.argv[1]
outdir = sys.argv[2] if len(sys.argv) > 2 else "build/frames"
n = int(sys.argv[3]) if len(sys.argv) > 3 else 12
os.makedirs(outdir, exist_ok=True)

cap = cv2.VideoCapture(src)
if not cap.isOpened():
    print("OPEN_FAIL")
    sys.exit(2)

total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
fps = cap.get(cv2.CAP_PROP_FPS)
w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
print(f"frames={total} fps={fps} size={w}x{h}")

step = max(1, total // n)
saved = []
idx = 0
while True:
    ret, frame = cap.read()
    if not ret:
        break
    if idx % step == 0 and len(saved) < n:
        p = os.path.join(outdir, f"frame_{len(saved):03d}_{idx:06d}.png")
        cv2.imwrite(p, frame)
        saved.append(p)
    idx += 1
cap.release()
print("SAVED", len(saved))
for p in saved:
    print(p)
