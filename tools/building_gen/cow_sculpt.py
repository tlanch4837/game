"""The cow sculpt shared by the SPOM LOD (industry_spom.py) and the rigged close-up mesh (gen_cattle.py): a 3D signed-distance
field of blended muscle masses, jointed legs, head, dewlap, udder and tail. Coordinates: a along the body (tail 0 .. nose 2.6),
h up, s across. stance: True = legs staggered (a walking pose, for the SPOM volume: no two legs share a column seen from the side);
False = square stance for the rig, which moves the legs itself.
"""
import numpy as np


def _smin(a, b, k):
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0, 1)
    return b + (a - b) * h - k * h * (1 - h)


def _ell(P, c, r):
    q = [(P[i] - c[i]) / r[i] for i in range(3)]
    k0 = np.sqrt(q[0] ** 2 + q[1] ** 2 + q[2] ** 2)
    k1 = np.sqrt((q[0] / r[0]) ** 2 + (q[1] / r[1]) ** 2 + (q[2] / r[2]) ** 2)
    return k0 * (k0 - 1) / np.maximum(k1, 1e-6)


def _cap(P, a, b, ra, rb):
    """capsule (tapering from ra at a to rb at b)."""
    pa = [P[i] - a[i] for i in range(3)]
    ba = [b[i] - a[i] for i in range(3)]
    t = np.clip(sum(pa[i] * ba[i] for i in range(3)) / max(sum(x * x for x in ba), 1e-9), 0, 1)
    d = np.sqrt(sum((pa[i] - ba[i] * t) ** 2 for i in range(3)))
    return d - (ra + (rb - ra) * t)


def _noise3(P, f, seed):
    """cheap smooth 3D value noise: sum of a few rotated sines (smooth blobs, no grid)."""
    r = np.random.default_rng(seed)
    n = 0
    for _ in range(5):
        d = r.normal(size=3)
        d /= np.linalg.norm(d)
        ph = r.uniform(0, 6.28)
        n = n + np.sin((P[0] * d[0] + P[1] * d[1] + P[2] * d[2]) * f * r.uniform(0.7, 1.4) + ph)
    return n / 5 ** 0.5


def cow_sdf(P, graze, stance=True):
    """returns (distance, part id): 0 body, 1 head, 2 leg, 3 hoof, 4 udder, 5 muzzle, 6 eye, 7 tail switch, 8 ear, 9 horn base."""
    k = 0.07
    body = _ell(P, (1.0, 1.13, 0), (0.92, 0.36, 0.33))
    body = _smin(body, _ell(P, (0.95, 0.94, 0), (0.62, 0.24, 0.31)), k)              # belly
    body = _smin(body, _ell(P, (0.32, 1.27, 0), (0.33, 0.24, 0.28)), k)              # hips / rump
    body = _smin(body, _ell(P, (0.3, 1.0, 0), (0.26, 0.3, 0.25)), k)                 # thighs
    body = _smin(body, _ell(P, (0.06, 1.33, 0), (0.12, 0.09, 0.11)), 0.04)          # tail head
    for s in (-0.22, 0.22):
        body = _smin(body, _ell(P, (0.42, 1.36, s), (0.09, 0.07, 0.07)), 0.05)      # hook bones
    body = _smin(body, _ell(P, (1.62, 1.16, 0), (0.36, 0.34, 0.27)), k)              # shoulders
    body = _smin(body, _ell(P, (1.86, 0.88, 0), (0.2, 0.2, 0.19)), k)                # brisket
    body = _smin(body, _ell(P, (1.1, 1.43, 0), (0.85, 0.08, 0.15)), 0.08)           # topline
    if graze:
        neck = _cap(P, (1.72, 1.15, 0), (2.08, 0.62, 0), 0.22, 0.15)
        hd_a, hd_b, mz = (2.1, 0.6, 0), (2.28, 0.17, 0), (2.3, 0.1, 0)
        ear_c, eye_c = (2.07, 0.6), (2.19, 0.38)
        dew = _ell(P, (1.92, 0.8, 0), (0.18, 0.16, 0.06))
    else:
        neck = _cap(P, (1.72, 1.17, 0), (2.12, 1.38, 0), 0.22, 0.15)
        hd_a, hd_b, mz = (2.17, 1.43, 0), (2.56, 1.16, 0), (2.62, 1.12, 0)
        ear_c, eye_c = (2.18, 1.46), (2.35, 1.37)
        dew = _ell(P, (1.95, 0.98, 0), (0.22, 0.15, 0.06))
    body = _smin(body, neck, 0.08)
    body = _smin(body, dew, 0.05)
    head = _cap(P, hd_a, hd_b, 0.13, 0.085)
    head = _smin(head, _ell(P, (hd_a[0] + 0.02, hd_a[1] + 0.02, 0), (0.1, 0.09, 0.11)), 0.03)  # poll
    muzzle = _ell(P, mz, (0.085, 0.08, 0.095))
    head = _smin(head, muzzle, 0.03)
    ears = np.minimum(_ell(P, (ear_c[0], ear_c[1], 0.19), (0.05, 0.035, 0.1)), _ell(P, (ear_c[0], ear_c[1], -0.19), (0.05, 0.035, 0.1)))
    d = _smin(body, head, 0.06)
    d = np.minimum(d, ears)
    # legs: upper (forearm / gaskin), cannon, hoof; hind legs bend back at the hock
    legs = []
    for a, s in (((1.78, 0.16), (1.5, -0.16)) if stance else ((1.66, 0.17), (1.66, -0.17))):  # front
        legs.append((_smin(_cap(P, (a, 0.98, s), (a + 0.02, 0.48, s), 0.1, 0.065), _cap(P, (a + 0.02, 0.48, s), (a + 0.03, 0.1, s), 0.055, 0.05), 0.03),
                     _cap(P, (a + 0.03, 0.1, s), (a + 0.05, 0.0, s), 0.06, 0.065)))
    for a, s in (((0.22, 0.16), (0.5, -0.16)) if stance else ((0.36, 0.17), (0.36, -0.17))):  # hind
        legs.append((_smin(_cap(P, (a + 0.05, 1.0, s), (a - 0.06, 0.52, s), 0.12, 0.06), _cap(P, (a - 0.06, 0.52, s), (a, 0.1, s), 0.05, 0.048), 0.03),
                     _cap(P, (a, 0.1, s), (a + 0.03, 0.0, s), 0.058, 0.063)))
    leg = np.minimum.reduce([l[0] for l in legs])
    hoof = np.minimum.reduce([l[1] for l in legs])
    d = _smin(d, leg, 0.05)
    d = np.minimum(d, hoof)
    udder = _ell(P, (0.66, 0.74, 0), (0.19, 0.12, 0.15))
    for ta, ts in ((0.6, 0.06), (0.6, -0.06), (0.74, 0.06), (0.74, -0.06)):
        udder = np.minimum(udder, _cap(P, (ta, 0.68, ts), (ta, 0.6, ts), 0.018, 0.015))
    tail = _cap(P, (0.0, 1.33, 0), (-0.08, 0.55, 0), 0.03, 0.02)
    switch = _ell(P, (-0.08, 0.5, 0), (0.05, 0.11, 0.05))
    d = np.minimum(d, np.minimum(tail, switch))
    eyes = np.minimum(_ell(P, (eye_c[0], eye_c[1], 0.11), (0.025, 0.02, 0.02)), _ell(P, (eye_c[0], eye_c[1], -0.11), (0.025, 0.02, 0.02)))
    part = np.zeros(d.shape, np.int8)
    for pid, dd in ((1, head), (8, ears), (2, leg), (3, hoof), (5, muzzle), (7, switch), (6, eyes)):
        part[dd <= np.minimum(d, 0) + 0.012] = pid
    return d, part, udder


