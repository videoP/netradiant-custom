"""Generate a test map that gives CullSides real work: a sealed room packed with
a solid block of adjacent cubes, so there are many coincident and many buried
faces. Plane winding is derived from q3map2's own PlaneFromPoints and asserted."""
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


# for outward normal n, in-plane u,v with cross(u,v) == n
BASIS = {
    X: (Y, Z), neg(X): (Z, Y),
    Y: (Z, X), neg(Y): (X, Z),
    Z: (X, Y), neg(Z): (Y, X),
}


def box(mins, maxs, tex):
    """Six planes of an axis aligned box, normals pointing out."""
    corners = [(mins[0] if i & 1 else maxs[0],
                mins[1] if i & 2 else maxs[1],
                mins[2] if i & 4 else maxs[2]) for i in range(8)]
    out = []
    for axis, n in ((0, X), (0, neg(X)), (1, Y), (1, neg(Y)), (2, Z), (2, neg(Z))):
        # a corner lying on this face
        p0 = list(mins)
        p0[axis] = maxs[axis] if sum(n) > 0 else mins[axis]
        p0 = tuple(p0)
        u, v = BASIS[n]
        p1 = add(p0, mul(v, 64))
        p2 = add(p0, mul(u, 64))
        # q3map2: normal = cross(p2 - p0, p1 - p0), brush is behind every plane
        normal = cross(sub(p2, p0), sub(p1, p0))
        assert normal == mul(n, 64 * 64), (n, normal)
        dist = dot(p0, n)
        for c in corners:
            assert dot(c, n) <= dist + 1e-9, 'corner in front of plane'
        t = tex[1] if ( isinstance( tex, tuple ) and n == neg(Z) ) else ( tex[0] if isinstance( tex, tuple ) else tex )
        out.append('( %d %d %d ) ( %d %d %d ) ( %d %d %d ) %s 0 0 0 0.500000 0.500000 0 0 0'
                   % (p0 + p1 + p2 + (t,)))
    return '{\n' + '\n'.join(out) + '\n}\n'


WALL = 'test/wall'
CUBE = ( 'test/wall', 'test/nodraw' )
brushes = []

# sealed room
R, T = max(2048, (int(sys.argv[1]) if len(sys.argv)>1 else 20)*64//2 + 192), 1024
for mins, maxs in (
    ((-R - 64, -R - 64, -64 - 64), (R + 64, R + 64, -64)),   # floor
    ((-R - 64, -R - 64, T), (R + 64, R + 64, T + 64)),       # ceiling
    ((-R - 64, -R - 64, -64), (-R, R + 64, T)),              # -x
    ((R, -R - 64, -64), (R + 64, R + 64, T)),                # +x
    ((-R, -R - 64, -64), (R, -R, T)),                        # -y
    ((-R, R, -64), (R, R + 64, T)),                          # +y
):
    brushes.append(box(mins, maxs, WALL))

# a solid block of touching cubes: adjacent faces are coincident, inner cubes
# are completely buried, which is exactly what CullSides looks for
S = 64
NX = NY = int(sys.argv[1]) if len(sys.argv) > 1 else 20
NZ = 8
for ix in range(NX):
    for iy in range(NY):
        for iz in range(NZ):
            x = -R + 128 + ix * S
            y = -R + 128 + iy * S
            z = iz * S
            brushes.append(box((x, y, z), (x + S, y + S, z + S), CUBE))

with open(sys.argv[2] if len(sys.argv) > 2 else 'test.map', 'w') as f:
    f.write('// Game: Quake 3\n// Format: Quake3\n')
    f.write('// entity 0\n{\n"classname" "worldspawn"\n')
    for i, b in enumerate(brushes):
        f.write('// brush %d\n' % i)
        f.write(b)
    f.write('}\n')
    f.write('// entity 1\n{\n"classname" "info_player_start"\n"origin" "0 0 %d"\n}\n' % (NZ * S + 64))

print('%d brushes' % len(brushes))
