# vfix.py PHOTO [--horizon] [--no-keystone] [--iters N] [--dry]
# Iteratively sets rotate/vertical so that near-vertical lines are vertical and parallel
# (or, with --horizon, the longest near-horizontal line is level; keystone still from verticals).
# The result is applied as one history step labelled "Verticals (fit)".
import sys, os, json, subprocess, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np, cv2
from vfit import fit
SIL = os.environ['SIL']; S = os.environ['WORK']
a = sys.argv; photo = a[1]; horizon = '--horizon' in a; nokey = '--no-keystone' in a
iters = int(a[a.index('--iters') + 1]) if '--iters' in a else 4
def sil(*args, out=False):
    r = subprocess.run([SIL, *args], capture_output=True, text=True, check=True); return r.stdout
def cur():
    d = json.loads(sil('--json', 'settings', photo))['values']; return d['rotate'], d['vertical']
def horizon_tilt(path):
    g = cv2.imread(path, cv2.IMREAD_GRAYSCALE); h, w = g.shape
    L = cv2.createLineSegmentDetector().detect(g)[0]; r = []
    for x1, y1, x2, y2 in L.reshape(-1, 4):
        if x1 > x2: x1, y1, x2, y2 = x2, y2, x1, y1
        l = np.hypot(x2 - x1, y2 - y1); t = np.degrees(np.arctan2(y1 - y2, x2 - x1))
        if l > 0.08 * w and abs(t) < 6: r.append((t, l, (y1 + y2) / 2 / h))
    if not r: return None
    r.sort(key=lambda t: -t[1]); y0 = r[0][2]
    band = [t for t in r if abs(t[2] - y0) < 0.05][:6]
    return float(np.average([t[0] for t in band], weights=[t[1] for t in band]))
tmp = os.path.join(S, 'vfix_tmp.jpg')
def measure():
    sil('render', photo, tmp, '--size', '2400')
    f = fit(tmp); hz = horizon_tilt(tmp) if horizon else None
    return f, hz
start = int([l for l in sil('history', photo).splitlines() if l.startswith('*')][0].split()[1])
rot, ver = cur(); gain = -0.22; prev = None; log = []
for i in range(iters):
    f, hz = measure()
    if f is None and hz is None: break
    r, k, n, _ = f if f else (0, 0, 0, 0)
    log.append(f'it{i}: rotate={rot:.2f} vertical={ver:.0f} -> r={r:+.2f} k={k:+.2f} n={n}' + (f' horizon={hz:+.2f}' if hz is not None else ''))
    err_r = -hz if horizon else r          # residual roll (deg): +r means rotate down
    usek = not nokey and n >= 8
    if abs(err_r) < 0.12 and (not usek or abs(k) < 0.5): break
    if prev and usek and abs(ver - prev[0]) > 0.5:
        g = (k - prev[1]) / (ver - prev[0])
        if g < -0.03: gain = g
    prev = (ver, k)
    nrot = float(np.clip(rot - err_r, -45, 45))
    nver = float(np.clip(round(ver - k / gain), -100, 100)) if usek else ver
    if abs(nrot - rot) < 0.05 and nver == ver: break
    rot, ver = round(nrot, 2), nver
    sil('set', photo, f'rotate={rot}', f'vertical={int(ver)}', '--label', 'fit step')
print(photo, ' | '.join(log))
# collapse the fit steps into one history entry
if int([l for l in sil('history', photo).splitlines() if l.startswith('*')][0].split()[1]) != start:
    sil('undo', photo, '--to', str(start))
    if '--dry' not in a:
        sil('set', photo, f'rotate={rot}', f'vertical={int(ver)}', '--label', 'Verticals (fit)')
print(photo, 'final rotate', rot, 'vertical', int(ver), '(dry)' if '--dry' in a else '')
