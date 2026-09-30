"""Test map aimed at FixTJunctions rather than CullSides: boxes of differing
heights packed side by side, so neighbouring faces meet at genuine T-junctions,
with a share of them rotated so there are non-axial edges too. Unlike a regular
cube grid this produces a great many distinct edge lines, which is what
AddEdge's linear scan is actually sensitive to."""
import math
import random
import sys

X, Y, Z = (1, 0, 0), (0, 1, 0), (0, 0, 1)


def neg(a):
    return (-a[0], -a[1], -a[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def norm(a):
    l = math.sqrt(dot(a, a))
    return (a[0] / l, a[1] / l, a[2] / l)


BASIS = {
    X: (Y, Z), neg(X): (Z, Y),
    Y: (Z, X), neg(Y): (X, Z),
    Z: (X, Y), neg(Z): (Y, X),
}


def fmt(p):
    return '( %s %s %s )' % tuple(
        ('%d' % v) if abs(v - round(v)) < 1e-9 else ('%.6f' % v) for v in p)


def box(mins, maxs, tex, rot=None):
    """Axis aligned box, optionally rotated about Z through its own centre."""
    cx = (mins[0] + maxs[0]) / 2.0
    cy = (mins[1] + maxs[1]) / 2.0

    def xf(p):
        if rot is None:
            return p
        c, s = math.cos(rot), math.sin(rot)
        dx, dy = p[0] - cx, p[1] - cy
        return (cx + dx * c - dy * s, cy + dx * s + dy * c, p[2])

    corners = [xf((mins[0] if i & 1 else maxs[0],
                   mins[1] if i & 2 else maxs[1],
                   mins[2] if i & 4 else maxs[2])) for i in range(8)]
    out = []
    for axis, n in ((0, X), (0, neg(X)), (1, Y), (1, neg(Y)), (2, Z), (2, neg(Z))):
        p0 = list(mins)
        p0[axis] = maxs[axis] if sum(n) > 0 else mins[axis]
        u, v = BASIS[n]
        pts = [xf(tuple(p0)), xf(add(tuple(p0), mul(v, 64))), xf(add(tuple(p0), mul(u, 64)))]
        # q3map2: normal = cross(p2 - p0, p1 - p0); every corner must be behind it
        nrm = norm(cross(sub(pts[2], pts[0]), sub(pts[1], pts[0])))
        d = dot(pts[0], nrm)
        for c in corners:
            assert dot(c, nrm) <= d + 1e-6, 'corner in front of plane'
        out.append('%s %s %s %s 0 0 0 0.500000 0.500000 0 0 0'
                   % (fmt(pts[0]), fmt(pts[1]), fmt(pts[2]), tex))
    return '{\n' + '\n'.join(out) + '\n}\n'


WALL = 'test/wall'
N = int(sys.argv[1]) if len(sys.argv) > 1 else 40
OUT = sys.argv[2] if len(sys.argv) > 2 else 'tj.map'
ROT_FRACTION = 0.25

random.seed(12345)

S = 64
R = max(2048, N * S // 2 + 256)
T = 2048
brushes = []

for mins, maxs in (
    ((-R - 64, -R - 64, -64 - 64), (R + 64, R + 64, -64)),
    ((-R - 64, -R - 64, T), (R + 64, R + 64, T + 64)),
    ((-R - 64, -R - 64, -64), (-R, R + 64, T)),
    ((R, -R - 64, -64), (R + 64, R + 64, T)),
    ((-R, -R - 64, -64), (R, -R, T)),
    ((-R, R, -64), (R, R + 64, T)),
):
    brushes.append(box(mins, maxs, WALL))

# columns of differing heights, packed edge to edge: each neighbour pair meets
# at a T-junction because the heights do not line up
for ix in range(N):
    for iy in range(N):
        x = -R + 128 + ix * S
        y = -R + 128 + iy * S
        h = random.randrange(2, 14) * 16          # varying top
        z = random.randrange(0, 3) * 16           # varying bottom
        rot = math.radians(random.uniform(5, 85)) if random.random() < ROT_FRACTION else None
        brushes.append(box((x, y, z), (x + S, y + S, z + h), WALL, rot))

with open(OUT, 'w') as f:
    f.write('// Game: Quake 3\n// Format: Quake3\n')
    f.write('// entity 0\n{\n"classname" "worldspawn"\n')
    for i, b in enumerate(brushes):
        f.write('// brush %d\n' % i)
        f.write(b)
    f.write('}\n')
    f.write('// entity 1\n{\n"classname" "info_player_start"\n"origin" "0 0 1024"\n}\n')

print('%d brushes' % len(brushes))
