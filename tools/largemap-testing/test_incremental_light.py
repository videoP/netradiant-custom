"""Compile real BSPs and compare incremental lighting with a fresh full pass.

Usage: python tools/largemap-testing/test_incremental_light.py install/q3map2.exe
Fixtures and compiler logs are kept under .cache/incremental-light-test.
"""
import argparse
import pathlib
import re
import struct
import subprocess
import zlib


def box(lo, hi):
    axes = [(1, 2), (2, 0), (0, 1)]
    faces = []
    for axis, (u, v) in enumerate(axes):
        for sign in (-1, 1):
            a = list(lo)
            a[axis] = hi[axis] if sign > 0 else lo[axis]
            b, c = a.copy(), a.copy()
            b[v] += 64 * sign
            c[u] += 64
            faces.append(" ".join("( %d %d %d )" % tuple(p) for p in (a, b, c))
                         + " test/wall 0 0 0 0.5 0.5 0 0 0")
    return "{\n" + "\n".join(faces) + "\n}\n"


def fixture(light):
    brushes = []
    for center in (-2048, 0, 2048):
        a, b = center - 256, center + 256
        for lo, hi in [((a-32, -288, -32), (b+32, 288, 0)),
                       ((a-32, -288, 256), (b+32, 288, 288)),
                       ((a-32, -288, 0), (a, 288, 256)),
                       ((b, -288, 0), (b+32, 288, 256)),
                       ((a, -288, 0), (b, -256, 256)),
                       ((a, 256, 0), (b, 288, 256))]:
            material = "test/stone" if center < 0 else "test/wall"
            brushes.append(box(lo, hi).replace("test/wall", material))
    # A pillar casts a shadow which moves when the central light moves.
    brushes.append(box((80, -32, 0), (112, 32, 192)))
    brushes.append('''{
patchDef2
{
test/wall
( 3 3 0 0 0 )
(
( ( -224 -160 32 0 0 ) ( -224 -80 64 0 0.5 ) ( -224 0 32 0 1 ) )
( ( -160 -160 32 0.5 0 ) ( -160 -80 96 0.5 0.5 ) ( -160 0 32 0.5 1 ) )
( ( -96 -160 32 1 0 ) ( -96 -80 64 1 0.5 ) ( -96 0 32 1 1 ) )
)
}
}
''')
    return ('{\n"classname" "worldspawn"\n' + "".join(brushes) + '}\n'
            '{\n"classname" "info_player_deathmatch"\n"origin" "0 0 48"\n}\n'
            '{\n"classname" "info_null"\n"targetname" "aim"\n"origin" "128 0 16"\n}\n'
            '{\n"classname" "light"\n"origin" "-2048 0 128"\n"light" "200"\n}\n'
            '{\n"classname" "light"\n"origin" "2048 0 128"\n"light" "200"\n}\n'
            + light)


def point(origin="0 0 128", color="1 1 1", intensity=200, extra=""):
    return ('{\n"classname" "light"\n"origin" "%s"\n"_color" "%s"\n'
            '"light" "%s"\n%s\n}\n' % (origin, color, intensity, extra))


def lumps(data):
    assert data[:4] == b"IBSP"
    return [data[offset:offset+size] for offset, size in
            struct.iter_unpack("<ii", data[8:8+17*8])]


def compare(a, b):
    # Entity command-line/timestamp metadata may differ; every other lump must match.
    aa, bb = lumps(a), lumps(b)
    assert aa[1:] == bb[1:], [i for i in range(1, 17) if aa[i] != bb[i]]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path(".cache/incremental-light-test").resolve()
    base = root / "baseq3"
    maps, scripts = base / "maps", base / "scripts"
    maps.mkdir(parents=True, exist_ok=True)
    scripts.mkdir(exist_ok=True)
    shader = scripts / "test.shader"
    stone_shader = "textures/test/stone { { map $lightmap } }\n"
    shader.write_text("textures/test/wall { { map $lightmap } }\n" + stone_shader)
    (scripts / "shaderlist.txt").write_text("test\n")
    source = maps / "incremental.map"
    bsp, cache = source.with_suffix(".bsp"), source.with_suffix(".lightcache")
    trace_cache = source.with_suffix(".lightcache.trace")
    group_cache = source.with_suffix(".lightcache.groups")
    dependency_cache = source.with_suffix(".lightcache.deps")
    counter = 0

    def run(stage, *options, fail=False, threads=1):
        nonlocal counter
        counter += 1
        command = [str(args.compiler.resolve()), "-v", "-threads", str(threads), "-game", "quake3",
                   "-fs_basepath", str(root), stage, *options, str(source)]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = result.stdout.decode(errors="replace")
        (root / ("%02d.log" % counter)).write_text(output)
        if "-incrementaldebug" not in options:
            assert not re.search(r"^chart \d+ dirty:", output, re.M), "verbose output floods the editor console"
        if fail:
            assert result.returncode, output
        else:
            assert result.returncode == 0, output
        return output

    cache.unlink(missing_ok=True)
    source.write_text(fixture(point()))
    run("-bsp", "-meta")
    run("-vis", "-fast")
    geometry = bsp.read_bytes()
    run("-light", "-fast", "-lightcache")
    assert cache.exists()
    assert trace_cache.exists()
    assert group_cache.exists()
    assert dependency_cache.exists()
    previous = bsp.read_bytes()
    output = run("-light", "-fast", "-incremental")
    assert "Cache: valid" in output, output
    assert "Trace cache: reused" in output, output
    assert "Surface cache: reused" in output, output
    assert "Dependency cache: 0 grid samples checked in changed light regions" in output, output
    assert "Dependency cache: 0 lightmaps checked in changed light regions" in output, output
    assert re.search(r"Lightmaps: \d+ reused .* 0 relit", output), output
    compare(previous, bsp.read_bytes())
    print("PASS unchanged build: all charts reused", flush=True)

    def checksum(data):
        value = 14695981039346656037
        for byte in data:
            value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
        return value

    def decompress_section(data):
        magic, raw_size, stored_size = struct.unpack_from("<3Q", data)
        assert data[:8] == b"QLZLIB01"
        assert stored_size + 24 == len(data)
        raw = zlib.decompress(data[24:])
        assert len(raw) == raw_size
        return raw

    def compress_section(data):
        stored = zlib.compress(data, 1)
        return b"QLZLIB01" + struct.pack("<2Q", len(data), len(stored)) + stored

    def replace_trace_payload(data, payload, compressed=True):
        header = list(struct.unpack_from("<9Q4i", data))
        header[0] = int.from_bytes(b"QLTRACE2" if compressed else b"QLTRACE1", "little")
        header[3] = checksum(payload)
        header[4] = 0
        header[4] = checksum(struct.pack("<9Q4i", *header))
        return struct.pack("<9Q4i", *header) + (compress_section(payload) if compressed else payload)

    assert trace_cache.read_bytes()[:8] == b"QLTRACE2"
    assert len(trace_cache.read_bytes()) < 88 + len(decompress_section(trace_cache.read_bytes()[88:]))
    assert cache.read_bytes()[:8] == b"QLTCACH4"

    # Read legacy uncompressed traces and rebuild legacy lighting sample layouts.
    trace_data = trace_cache.read_bytes()
    trace_cache.write_bytes(replace_trace_payload(trace_data, decompress_section(trace_data[88:]), compressed=False))
    before = bsp.read_bytes()
    output = run("-light", "-fast", "-incremental")
    assert "Cache: valid" in output and "Trace cache: reused" in output, output
    compare(before, bsp.read_bytes())
    cache_data = cache.read_bytes()
    header = list(struct.unpack_from("<13Q", cache_data))
    samples = decompress_section(cache_data[104:header[10]])
    preview = decompress_section(cache_data[header[10]:])
    assert len(cache_data) < 104 + len(samples) + len(preview)
    header[0] = int.from_bytes(b"QLTCACH2", "little")
    header[10], header[11] = 104 + len(samples), len(preview)
    header[6] = 0
    header[6] = checksum(struct.pack("<13Q", *header))
    cache.write_bytes(struct.pack("<13Q", *header) + samples + preview)
    before = bsp.read_bytes()
    output = run("-light", "-fast", "-incremental")
    # Legacy sample layouts must rebuild; their previews remain readable.
    assert "Cache: full lighting" in output, output
    compare(before, bsp.read_bytes())
    assert cache.read_bytes()[:8] == b"QLTCACH4", "legacy lighting cache was not upgraded"
    trace_cache.write_bytes(trace_data)
    print("PASS compressed containers and legacy uncompressed loading", flush=True)

    def cyclic_trace(data):
        header = list(struct.unpack_from("<9Q4i", data))
        damaged = bytearray(decompress_section(data[88:]))
        # Serialized infos are 16 bytes and triangles 64 bytes. Make the
        # root's first child point back to the root, with valid checksums.
        first_node = header[5] * 16 + header[6] * 64
        struct.pack_into("<i", damaged, first_node + 20, 0)
        return replace_trace_payload(data, damaged)

    def bad_section(data, offset, field):
        damaged = bytearray(data)
        value = struct.unpack_from("<Q", data, offset + field * 8)[0]
        struct.pack_into("<Q", damaged, offset + field * 8, value + 1)
        return damaged

    def invalid_groups(data, semantic=False):
        header = list(struct.unpack_from("<6Q", data))
        values = list(struct.unpack_from("<%di" % ((len(data) - 48) // 4), data, 48))
        if semantic:
            groups, position = [], 0
            for _ in range(header[2]):
                count = values[position]
                groups.append(values[position + 1:position + count + 1])
                position += count + 1
            # These groups could not merge in the normal setup. Combine them
            # with valid IDs, complete coverage and valid container checksums.
            groups = [groups[0] + groups[1], *groups[2:]]
            header[2] = len(groups)
            values = [value for group in groups for value in [len(group), *group]]
        else:
            values[1] = -1
        payload = struct.pack("<%di" % len(values), *values)
        header[4] = checksum(payload)
        header[5] = 0
        header[5] = checksum(struct.pack("<6Q", *header))
        return struct.pack("<6Q", *header) + payload

    for name, transform in [("missing", lambda data: None),
                            ("truncated", lambda data: data[:50]),
                            ("bad header", lambda data: bytes([data[0] ^ 1]) + data[1:]),
                            ("bad payload", lambda data: data[:-1] + bytes([data[-1] ^ 1])),
                            ("invalid IDs", invalid_groups),
                            ("invalid merge", lambda data: invalid_groups(data, semantic=True))]:
        data = transform(group_cache.read_bytes())
        if data is None:
            group_cache.unlink()
        else:
            group_cache.write_bytes(data)
        before = bsp.read_bytes()
        output = run("-light", "-fast", "-incremental")
        assert "Cache: valid" in output, output
        assert "Surface cache: building" in output, output
        compare(before, bsp.read_bytes())
        output = run("-light", "-fast", "-incremental")
        assert "Surface cache: reused" in output, output
        print("PASS surface cache", name, "rebuilds safely", flush=True)

    def stale_dependencies(data):
        header = list(struct.unpack_from("<7Q", data))
        header[2] ^= 1  # bind to a different lighting payload
        header[6] = 0
        header[6] = checksum(struct.pack("<7Q", *header))
        return struct.pack("<7Q", *header) + data[56:]

    for name, transform in [("missing", lambda data: None),
                            ("truncated", lambda data: data[:60]),
                            ("bad header", lambda data: bytes([data[0] ^ 1]) + data[1:]),
                            ("bad payload", lambda data: data[:-1] + bytes([data[-1] ^ 1])),
                            ("stale lighting", stale_dependencies)]:
        data = transform(dependency_cache.read_bytes())
        if data is None:
            dependency_cache.unlink()
        else:
            dependency_cache.write_bytes(data)
        before = bsp.read_bytes()
        output = run("-light", "-fast", "-incremental")
        assert "Cache: valid" in output, output
        assert "in changed light regions" not in output, output
        compare(before, bsp.read_bytes())
        output = run("-light", "-fast", "-incremental")
        assert "Dependency cache: 0 grid samples checked in changed light regions" in output, output
        print("PASS dependency cache", name, "uses complete checks safely", flush=True)

    for name, transform in [("missing", lambda data: None),
                            ("truncated", lambda data: data[:100]),
                            ("bad header", lambda data: bytes([data[0] ^ 1]) + data[1:]),
                            ("bad payload", lambda data: data[:-1] + bytes([data[-1] ^ 1])),
                            ("bad raw size", lambda data: bad_section(data, 88, 1)),
                            ("bad stored size", lambda data: bad_section(data, 88, 2)),
                            ("trailing bytes", lambda data: data + b"extra"),
                            ("extra decoded bytes", lambda data: replace_trace_payload(data, decompress_section(data[88:]) + b"extra")),
                            ("cyclic nodes", cyclic_trace)]:
        data = transform(trace_cache.read_bytes())
        if data is None:
            trace_cache.unlink()
        else:
            trace_cache.write_bytes(data)
        before = bsp.read_bytes()
        output = run("-light", "-fast", "-incremental")
        assert "Cache: valid" in output, output
        assert "Trace cache: building" in output, output
        assert "Trace cache: reused" not in output, output
        compare(before, bsp.read_bytes())
        output = run("-light", "-fast", "-incremental")
        assert "Trace cache: reused" in output, output
        print("PASS trace cache", name, "rebuilds safely", flush=True)

    for name, transform in [("truncated", lambda data: data[:-1]),
                            ("bad sample stream", lambda data: data[:128] + bytes([data[128] ^ 1]) + data[129:]),
                            ("bad sample raw size", lambda data: bad_section(data, 104, 1)),
                            ("bad sample stored size", lambda data: bad_section(data, 104, 2)),
                            ("bad preview stream", lambda data: data[:-1] + bytes([data[-1] ^ 1])),
                            ("trailing bytes", lambda data: data + b"extra")]:
        cache.write_bytes(transform(cache.read_bytes()))
        before = bsp.read_bytes()
        output = run("-light", "-fast", "-incremental")
        assert "Cache: full lighting" in output, output
        compare(before, bsp.read_bytes())
        output = run("-light", "-fast", "-incremental")
        assert "Cache: valid" in output, output
        print("PASS compressed lighting cache", name, "rebuilds safely", flush=True)
    snapshot = maps / "preview.lsn"
    run("-lightsnapbsp", "-lightsnap", str(snapshot))
    old_preview = snapshot.read_bytes()
    assert old_preview[:4] == b"LSN1"
    def cached_preview():
        data = cache.read_bytes()
        header = struct.unpack_from("<13Q", data)
        assert data[:8] == b"QLTCACH4"
        offset, size = header[10:12]
        assert offset + size == len(data)
        return decompress_section(data[offset:offset + size])
    assert cached_preview() == old_preview, "cache preview differs from exported BSP"

    # Radiant saves classname first and uses shorter primitive decimals.
    # This is an entity/geometry serialization change, not a BSP change.
    source.write_text(fixture(point()).replace('"classname" "worldspawn"',
                      '"_lightmapscale" "1"\n"classname" "worldspawn"'))
    run("-bsp", "-meta")
    run("-vis", "-fast")
    geometry = bsp.read_bytes()
    source.write_text(source.read_text().replace('0.5', '0.500000'))
    run("-light", "-fast", "-lightcache")
    source.write_text(source.read_text().replace('0.500000', '0.5').replace(
                      '"_lightmapscale" "1"\n"classname" "worldspawn"',
                      '"classname" "worldspawn"\n"_lightmapscale" "1"'))
    # BSP/VIS rebuilds can also rewrite irrelevant surface-extra comments.
    srf = source.with_suffix(".srf")
    srf.write_text('// editor/rebuild formatting\n' + srf.read_text().replace(
                   'planar', 'changed comment').replace('0.000000', '0'))
    output = run("-light", "-fast", "-incremental")
    assert "Cache: valid" in output, output
    assert re.search(r"Lightmaps: \d+ reused .* 0 relit", output), output
    # Rebuild the same geometry with the new key order: compiled entity order
    # must not cause another rejection, nor should rewritten SRF comments.
    previous_geometry_key = struct.unpack_from("<13Q", cache.read_bytes())[3]
    run("-bsp", "-meta")
    run("-vis", "-fast")
    geometry = bsp.read_bytes()
    output = run("-light", "-fast", "-incremental")
    rebuilt_geometry_key = struct.unpack_from("<13Q", cache.read_bytes())[3]
    if previous_geometry_key == rebuilt_geometry_key:
        assert "Cache: valid" in output, output
    else:
        # The BSP stage can reorder material/surface groups across processes.
        # Changed surface indices require a full fallback even for the same map.
        assert "Cache: full lighting" in output, output
        incremental = bsp.read_bytes()
        bsp.write_bytes(geometry)
        run("-light", "-fast", "-lightcache")
        compare(incremental, bsp.read_bytes())
    original_srf = srf.read_text()
    srf.write_text(original_srf.replace('receiveShadows 1', 'receiveShadows 0'))
    output = run("-light", "-fast", "-incremental")
    assert "Cache: valid" not in output, "real surface-extra input changes must invalidate lighting"
    srf.write_text(original_srf)
    source.write_text(fixture(point()))
    run("-bsp", "-meta")
    run("-vis", "-fast")
    geometry = bsp.read_bytes()
    run("-light", "-fast", "-lightcache")
    print("PASS editor key/decimal formatting and equivalent BSP/VIS rebuild", flush=True)

    # Lighting is authoritative in the sidecar. Damaging output lighting in
    # the BSP must neither invalidate geometry nor contaminate reused samples.
    expected = bsp.read_bytes()
    damaged = bytearray(expected)
    for lump in (14, 15):
        offset, size = struct.unpack_from("<ii", damaged, 8 + lump * 8)
        damaged[offset:offset+size] = bytes(size)
    # IBSP drawverts are 44 bytes: overwrite RGB and output lightmap UVs,
    # preserving alpha because the compiler treats it as an input.
    offset, size = struct.unpack_from("<ii", damaged, 8 + 10 * 8)
    for vertex in range(offset, offset + size, 44):
        damaged[vertex + 20:vertex + 28] = bytes(8)
        damaged[vertex + 40:vertex + 43] = bytes(3)
    bsp.write_bytes(damaged)
    output = run("-light", "-fast", "-incremental")
    assert "Cache: valid" in output
    compare(expected, bsp.read_bytes())
    print("PASS sidecar is authoritative; BSP lighting regenerated from cache", flush=True)

    # Duplicate states and changed light ordering must preserve accumulation
    # semantics when choosing which old/new regions need checking.
    source.write_text(fixture(point() + point()))
    run("-light", "-fast", "-lightcache")
    source.write_text(fixture(point()))
    output = run("-light", "-fast", "-incremental")
    assert "Cache: valid" in output, output
    incremental = bsp.read_bytes()
    bsp.write_bytes(geometry)
    run("-light", "-fast", "-lightcache")
    compare(incremental, bsp.read_bytes())
    original = fixture(point())
    lights = re.findall(r'\{\s*"classname" "light"[^{}]*\}', original)
    reordered = original.replace(lights[0], "LIGHT_PLACEHOLDER").replace(lights[1], lights[0]).replace("LIGHT_PLACEHOLDER", lights[1])
    source.write_text(reordered)
    output = run("-light", "-fast", "-incremental")
    assert "Cache: valid" in output, output
    assert "in changed light regions" not in output, output
    incremental = bsp.read_bytes()
    bsp.write_bytes(geometry)
    run("-light", "-fast", "-lightcache")
    compare(incremental, bsp.read_bytes())
    source.write_text(original)
    run("-light", "-fast", "-lightcache")
    print("PASS duplicate light deletion and reordered accumulation", flush=True)

    # Saving a moved light may also rewrite other entities' spatial decimals.
    # Those values are parsed as floats, while target names remain exact strings.
    spot = point(extra='"target" "aim"')
    original = fixture(spot).replace('"origin" "0 0 48"',
                                    '"origin" "0 0 48"\n"angles" "0 90 0"\n"angle" "90"')
    source.write_text(original)
    run("-bsp", "-meta")
    run("-vis", "-fast")
    run("-light", "-fast", "-lightcache")
    source.write_text(original.replace('"origin" "0 0 48"', '"origin" "-0.000000  0.000000 48.000000 "')
                     .replace('"angles" "0 90 0"', '"angles" "0.000000 90.000000 0.000000"')
                     .replace('"angle" "90"', '"angle" "90.000000"')
                     .replace(spot, point("64 0 96", extra='"target" "aim"')))
    output = run("-light", "-fast", "-incremental")
    assert "Cache: valid" in output and "Trace cache: reused" in output, output
    incremental = bsp.read_bytes()
    run("-light", "-fast", "-lightcache")
    compare(incremental, bsp.read_bytes())
    # Formatting changes in the compiled entities must also retain the cache.
    run("-bsp", "-onlyents")
    output = run("-light", "-fast", "-incremental")
    assert "Cache: valid" in output and "Trace cache: reused" in output, output
    compare(incremental, bsp.read_bytes())
    print("PASS moved light with equivalent entity origin/angles formatting", flush=True)

    # A real spotlight target move needs refreshed BSP entities, not BSP/VIS.
    before = bsp.read_bytes()
    source.write_text(source.read_text().replace('"origin" "128 0 16"', '"origin" "96 0 16"'))
    output = run("-light", "-fast", "-incremental", fail=True)
    assert "-bsp -onlyents" in output, output
    assert bsp.read_bytes() == before
    run("-bsp", "-onlyents")
    updated = bsp.read_bytes()
    assert lumps(updated)[1:] == lumps(before)[1:], "onlyents changed geometry, VIS or lighting"
    run("-light", "-fast", "-incremental")
    incremental = bsp.read_bytes()
    bsp.write_bytes(updated)
    run("-light", "-fast", "-lightcache")
    compare(incremental, bsp.read_bytes())
    print("PASS entity-only edits use onlyents without rebuilding BSP/VIS", flush=True)

    # Numeric-looking target names must not acquire spatial normalization.
    source.write_text(source.read_text().replace('"targetname" "aim"', '"targetname" "01"'))
    run("-bsp", "-onlyents")
    run("-light", "-fast", "-lightcache")
    before = bsp.read_bytes()
    source.write_text(source.read_text().replace('"targetname" "01"', '"targetname" "1"'))
    output = run("-light", "-fast", "-incremental", fail=True)
    assert "-bsp -onlyents" in output, output
    assert bsp.read_bytes() == before
    source.write_text(fixture(point()))
    run("-bsp", "-meta")
    run("-vis", "-fast")
    geometry = bsp.read_bytes()
    run("-light", "-fast", "-lightcache")
    print("PASS numeric-looking entity names retain exact spelling", flush=True)

    for name, light in [("move", point("64 0 96")),
                        ("color/intensity", point("64 0 96", "1 0.2 0.1", 350)),
                        ("delete", ""), ("add", point()),
                        ("spot", point(extra='"target" "aim"')),
                        ("move spot", point("-96 0 160", extra='"target" "aim"')),
                        ("move between rooms", point("2048 64 96"))]:
        source.write_text(fixture(light))
        old = bsp.read_bytes()
        output = run("-light", "-fast", "-incremental")
        assert "Cache: valid" in output, output
        counts = re.search(r"Lightmaps: (\d+) reused .* (\d+) relit", output)
        assert counts and int(counts[1]) > 0 and int(counts[2]) > 0, output
        grid_counts = re.search(r"Lightgrid: (\d+) reused, (\d+) relit", output)
        assert grid_counts and all(int(n) > 0 for n in grid_counts.groups()), output
        incremental, sidecar = bsp.read_bytes(), cache.read_bytes()
        if name == "move":
            run("-lightsnapbsp", "-lightsnap", str(snapshot))
            assert snapshot.read_bytes() != old_preview, "BSP preview export did not reflect incremental lighting"
            assert cached_preview() == snapshot.read_bytes(), "sidecar preview did not update"
        assert lumps(old)[13] == lumps(incremental)[13], "atlas surface coordinates moved"
        bsp.write_bytes(geometry)
        run("-light", "-fast", "-lightcache")
        compare(incremental, bsp.read_bytes())
        bsp.write_bytes(incremental)
        cache.write_bytes(sidecar)
        print("PASS", name, counts.group(0), flush=True)

    for name, transform in [("truncated", lambda data: data[:120]),
                            ("bad header", lambda data: bytes([data[0] ^ 1]) + data[1:]),
                            ("bad payload", lambda data: data[:-1] + bytes([data[-1] ^ 1]))]:
        previous = bsp.read_bytes()
        cache.write_bytes(transform(cache.read_bytes()))
        output = run("-light", "-fast", "-incremental")
        assert "Cache: valid" not in output, output
        compare(previous, bsp.read_bytes())
        print("PASS", name, "falls back", flush=True)

    output = run("-light", "-fast", "-incremental", "-gamma", "1.2")
    assert "Cache: valid" not in output
    run("-light", "-fast", "-lightcache")
    shader.write_text("textures/test/wall { q3map_lightmapBrightness 0.5 { map $lightmap } }\n" + stone_shader)
    output = run("-light", "-fast", "-incremental")
    assert "Cache: valid" not in output
    incremental = bsp.read_bytes()
    bsp.write_bytes(geometry)
    run("-light", "-fast", "-lightcache")
    compare(incremental, bsp.read_bytes())
    print("PASS settings and shader invalidation", flush=True)

    # Supersampling and shading exercise the cached pre-subsampling buffers.
    source.write_text(fixture(point()))
    bsp.write_bytes(geometry)
    quality = ("-fast", "-super", "2", "-samples", "2", "-shade")
    run("-light", *quality, "-lightcache")
    source.write_text(fixture(point("64 0 96")))
    output = run("-light", *quality, "-incremental")
    assert "Cache: valid" in output
    incremental = bsp.read_bytes()
    bsp.write_bytes(geometry)
    run("-light", *quality, "-lightcache")
    compare(incremental, bsp.read_bytes())
    print("PASS supersampling/shading with shadow caster", flush=True)

    run("-light", "-fast", "-lightcache", threads=4)
    source.write_text(fixture(point("-96 32 96")))
    output = run("-light", "-fast", "-incremental", threads=4)
    assert "Cache: valid" in output
    incremental = bsp.read_bytes()
    bsp.write_bytes(geometry)
    run("-light", "-fast", "-lightcache", threads=4)
    compare(incremental, bsp.read_bytes())
    print("PASS four-worker build with curved patch", flush=True)

    # External casters are loaded before cache validation. A model changing
    # on disk under the same name must invalidate its tree and lighting.
    models = base / "models/test"
    models.mkdir(parents=True, exist_ok=True)
    model = models / "caster.obj"
    (models / "caster.mtl").write_text("newmtl textures/test/wall\nKd 1 1 1\n")
    def caster(height):
        return ("mtllib caster.mtl\nusemtl textures/test/wall\n"
                "v 0 -48 0\nv 0 48 0\nv 0 0 %d\nf 1 2 3\n" % height)
    model.write_text(caster(160))
    source.write_text(fixture(point()) + '{\n"classname" "misc_gamemodel"\n'
                      '"origin" "48 0 0"\n"_castShadows" "1"\n'
                      '"model" "models/test/caster.obj"\n}\n')
    run("-bsp", "-meta")
    run("-vis", "-fast")
    model_geometry = bsp.read_bytes()
    run("-light", "-fast", "-lightcache")
    output = run("-light", "-fast", "-incremental")
    assert "Trace cache: reused" in output, output
    model.write_text(caster(96))
    output = run("-light", "-fast", "-incremental")
    assert "Cache: valid" not in output, "changed model content must invalidate lighting"
    assert "Trace cache: building" in output, output
    incremental = bsp.read_bytes()
    bsp.write_bytes(model_geometry)
    run("-light", "-fast", "-lightcache")
    compare(incremental, bsp.read_bytes())
    print("PASS external caster content invalidation and fresh-full comparison", flush=True)

    # Lights normally skip the source-geometry gate. If a light also carries
    # a shadow model, moving it or disabling its shadows must rebuild the tree.
    caster_keys = '"_castShadows" "1"\n"model" "models/test/caster.obj"'
    source.write_text(fixture(point(extra=caster_keys)))
    run("-bsp", "-meta")
    run("-vis", "-fast")
    model_geometry = bsp.read_bytes()
    run("-light", "-fast", "-lightcache")
    for origin, extra in [("48 0 128", caster_keys),
                          ("48 0 128", caster_keys.replace('"1"', '"0"'))]:
        source.write_text(fixture(point(origin=origin, extra=extra)))
        output = run("-light", "-fast", "-incremental")
        assert "Cache: valid" not in output, output
        assert "Trace cache: building" in output, output
        incremental = bsp.read_bytes()
        bsp.write_bytes(model_geometry)
        run("-light", "-fast", "-lightcache")
        compare(incremental, bsp.read_bytes())
    print("PASS light caster transform/shadow invalidation and fresh-full comparisons", flush=True)
    source.write_text(fixture(point()))
    run("-bsp", "-meta")
    run("-vis", "-fast")
    geometry = bsp.read_bytes()
    run("-light", "-fast", "-lightcache")

    # Loaded image changes must invalidate shadow/normal/light dependencies.
    textures = base / "textures/test"
    textures.mkdir(parents=True, exist_ok=True)
    texture = textures / "wall.tga"
    tga_header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, 2, 2, 24, 0)
    texture.write_bytes(tga_header + bytes([0, 0, 255]) * 4)
    run("-light", "-fast", "-lightcache")
    texture.write_bytes(tga_header + bytes([255, 0, 0]) * 4)
    output = run("-light", "-fast", "-incremental")
    assert "Cache: valid" not in output
    print("PASS texture input invalidation", flush=True)

    source.write_text(fixture(point("32 96 128")))
    output = run("-light", "-fast", "-incremental", "-incrementaldebug")
    assert "Cache: valid" in output, "debug output must not change cache compatibility"
    assert re.search(r"^chart \d+ dirty:", output, re.M), output
    print("PASS chart diagnostics are opt-in, including with -v", flush=True)

    before = bsp.read_bytes()
    unchanged_source = source.read_text()
    source.write_text(unchanged_source.replace('( 80 ', '( 81 '))
    output = run("-light", "-fast", "-incremental", fail=True)
    assert "Rebuild BSP/VIS" in output
    assert bsp.read_bytes() == before
    source.write_text(unchanged_source)
    print("PASS real primitive edits still require BSP/VIS", flush=True)

    source.write_text(source.read_text().replace('"worldspawn"', '"worldspawn"\n"_ambient" "10"'))
    output = run("-light", "-fast", "-incremental", fail=True)
    assert "Rebuild BSP/VIS" in output
    assert bsp.read_bytes() == before
    run("-bsp", "-meta")
    run("-vis", "-fast")
    output = run("-light", "-fast", "-incremental")
    assert "Cache: valid" not in output
    print("PASS source changes require BSP/VIS; new BSP falls back", flush=True)

    output = run("-light", "-fast", "-incremental", "-bounce", "1")
    assert "bounce lighting requires a full build" in output
    print("PASS bounce fallback", flush=True)
    print("All incremental lighting regressions passed. Logs:", root)


if __name__ == "__main__":
    main()
