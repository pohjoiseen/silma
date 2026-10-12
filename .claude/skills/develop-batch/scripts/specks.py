# specks.py IMG...: small isolated dark specks on smooth bright areas (birds, insects, dust in the sky).
# Prints x,y (0..1 of the image) and pixel area; check each candidate visually before healing.
import sys, cv2, numpy as np
for path in sys.argv[1:]:
    g = cv2.imread(path, cv2.IMREAD_GRAYSCALE).astype(np.float32) / 255
    h, w = g.shape
    bg = cv2.medianBlur((g * 255).astype(np.uint8), 31).astype(np.float32) / 255
    # smoothness of the background: std in 41px window of the median image
    m = cv2.blur(bg, (41, 41)); sd = np.sqrt(np.maximum(cv2.blur(bg * bg, (41, 41)) - m * m, 0))
    d = bg - g
    mask = ((d > 0.08) & (bg > 0.45) & (sd < 0.03)).astype(np.uint8)
    n, lab, st, cen = cv2.connectedComponentsWithStats(mask)
    detail = (np.abs(d) > 0.035).astype(np.float32)
    dens = cv2.boxFilter(detail, -1, (121, 121), normalize=False)
    hits = []
    for i in range(1, n):
        x, y, bw, bh, a = st[i]
        if not (4 <= a <= 400) or max(bw, bh) > 40 or y > 0.7 * h: continue
        cx, cy = int(cen[i][0]), int(cen[i][1])
        if dens[cy, cx] - a * 2.5 > 15: continue   # other detail nearby: branch, wire, roof...
        if sd[cy, cx] > 0.015: continue
        hits.append((cen[i][0] / w, cen[i][1] / h, a))
    print(path.split('/')[-1], len(hits), ' '.join(f'({x:.3f},{y:.3f},{a})' for x, y, a in hits[:12]))
