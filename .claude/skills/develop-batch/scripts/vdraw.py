# vdraw.py IN.jpg OUT.jpg: draw the near-vertical segments vfit uses, labelled with their tilt
# (green: within 0.5 deg of plumb, orange: top leans right, magenta: top leans left)
import sys, cv2, numpy as np
img = cv2.imread(sys.argv[1]); g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY); h, w = g.shape
L = cv2.createLineSegmentDetector().detect(g)[0]
for x1, y1, x2, y2 in L.reshape(-1, 4):
    if y1 > y2: x1, y1, x2, y2 = x2, y2, x1, y1
    l = np.hypot(x2 - x1, y2 - y1); th = np.degrees(np.arctan2(x1 - x2, y2 - y1))
    if l < 0.035 * h or abs(th) > 10: continue
    c = (0, 255, 0) if abs(th) < 0.5 else (0, 200, 255) if th > 0 else (255, 0, 255)
    cv2.line(img, (int(x1), int(y1)), (int(x2), int(y2)), c, 5)
    cv2.putText(img, f'{th:+.1f}', (int(x1) + 6, int((y1 + y2) / 2)), cv2.FONT_HERSHEY_SIMPLEX, 1.1, c, 3)
cv2.imwrite(sys.argv[2], cv2.resize(img, (w // 2, h // 2), interpolation=cv2.INTER_AREA))
