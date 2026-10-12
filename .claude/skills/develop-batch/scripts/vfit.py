# vfit.py IMG...: robust fit of near-vertical line tilt  theta(x) = r + k*(x-0.5)
# theta in degrees, + = top of the line leans right; x = horizontal position 0..1.
# r = rotation residual, k = keystone residual (deg across the full width).
import sys, cv2, numpy as np
def fit(path, maxdev=10.0, minlen=0.035):
    img = cv2.imread(path, cv2.IMREAD_GRAYSCALE); h, w = img.shape
    L = cv2.createLineSegmentDetector().detect(img)[0]
    pts = []
    for x1, y1, x2, y2 in (L.reshape(-1, 4) if L is not None else []):
        if y1 > y2: x1, y1, x2, y2 = x2, y2, x1, y1      # (x1,y1) = top
        l = np.hypot(x2 - x1, y2 - y1)
        if l < minlen * h: continue
        th = np.degrees(np.arctan2(x1 - x2, y2 - y1))    # top right of bottom => +
        if abs(th) > maxdev: continue
        pts.append((th, l, (x1 + x2) / 2 / w))
    if len(pts) < 3: return None
    a = np.array(pts); th, wt, x = a[:, 0], a[:, 1], a[:, 2] - 0.5
    keep = np.ones(len(a), bool)
    for _ in range(6):
        A = np.stack([np.ones(keep.sum()), x[keep]], 1); W = wt[keep]
        if keep.sum() >= 6 and np.ptp(x[keep]) > 0.3:
            r, k = np.linalg.lstsq(A * W[:, None], th[keep] * W, rcond=None)[0]
        else:
            r, k = np.sum(th[keep] * W) / W.sum(), 0.0
        res = np.abs(th - (r + k * x))
        mad = np.median(res[keep]) * 1.5 + 0.3
        keep = res < max(1.0, 2.5 * mad)
    return r, k, int(keep.sum()), len(a)
if __name__ == '__main__':
    for p in sys.argv[1:]:
        f = fit(p)
        print(p.split('/')[-1], 'none' if f is None else f'r={f[0]:+.2f} k={f[1]:+.2f} n={f[2]}/{f[3]}')
