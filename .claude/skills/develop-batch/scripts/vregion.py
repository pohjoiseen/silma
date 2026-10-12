# vregion.py X0 X1 IMG... : length-weighted mean tilt of near-vertical lines with x in [X0,X1]
import sys, cv2, numpy as np
x0, x1 = float(sys.argv[1]), float(sys.argv[2])
for p in sys.argv[3:]:
    g = cv2.imread(p, cv2.IMREAD_GRAYSCALE); h, w = g.shape
    L = cv2.createLineSegmentDetector().detect(g)[0]; r = []
    for a, b, c, d in L.reshape(-1, 4):
        if b > d: a, b, c, d = c, d, a, b
        l = np.hypot(c - a, d - b); t = np.degrees(np.arctan2(a - c, d - b)); xm = (a + c) / 2 / w
        if l > 0.03 * h and abs(t) < 8 and x0 <= xm <= x1: r.append((t, l))
    r = np.array(r)
    if len(r) == 0: print(p, 'none'); continue
    med = np.median(r[:, 0]); r = r[np.abs(r[:, 0] - med) < 1.5]
    print(p.split('/')[-1], f'tilt={np.average(r[:,0], weights=r[:,1]):+.2f} n={len(r)}')
