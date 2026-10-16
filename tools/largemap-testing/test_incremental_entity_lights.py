"""Verify source edits to BSP-stored lights and their cached 3D previews.

Usage: python tools/largemap-testing/test_incremental_entity_lights.py install/q3map2.exe
"""
import argparse
import pathlib
import re
import struct
import subprocess
import sys
import zlib

sys.dont_write_bytecode = True
from test_incremental_light import fixture, point


def lumps(data):
    count = 18 if data[:4] == b"RBSP" else 17
    assert data[:4] in (b"RBSP", b"IBSP")
    return [data[offset:offset + size] for offset, size in
            struct.iter_unpack("<ii", data[8:8 + count * 8])]


def preview_data(data):
    header = struct.unpack_from("<13Q", data)
    offset, size = header[10:12]
    section = data[offset:offset + size]
    assert section[:8] == b"QLZLIB01"
    return zlib.decompress(section[24:])


def preview(cache):
    return preview_data(cache.read_bytes())


def lighting_samples(data):
    """Compare receiver lighting independently of styled atlas placement."""
    header = struct.unpack_from("<13Q", data)
    samples = zlib.decompress(data[128:header[10]])
    snap = preview_data(data)
    page_size = struct.unpack_from("<I", snap, 12)[0]
    page_bytes = page_size * page_size * 3
    charts, offset = [], 0
    for _ in range(header[7]):
        w, h, sw, sh, used = struct.unpack_from("<5i", samples, offset + 8)
        styles = samples[offset + 28:offset + 32]
        solid = samples[offset + 32:offset + 36]
        pages = struct.unpack_from("<4i", samples, offset + 36)
        xs = struct.unpack_from("<4i", samples, offset + 52)
        ys = struct.unpack_from("<4i", samples, offset + 68)
        end = offset + 84 + sw * sh * (16 * sum(style != 255 for style in styles) + 4)
        pixels = []
        for page, x, y, is_solid in zip(pages, xs, ys, solid):
            if page < 0:
                pixels.append(None)
                continue
            width, height = (1, 1) if is_solid else (w, h)
            pixels.append(b"".join(snap[28 + page * page_bytes + ((y + row) * page_size + x) * 3:
                                        28 + page * page_bytes + ((y + row) * page_size + x + width) * 3]
                                   for row in range(height)))
        charts.append((samples[offset:offset + 36], samples[offset + 84:end], pixels))
        offset = end
    # The remaining data contains raw lightgrid samples and vertex colors.
    return charts, samples[offset:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path(".cache/incremental-entity-light-test").resolve()
    for game, keep in [("ja", False), ("ja", True), ("quake3", True)]:
        base = root / (game + ("-keep" if keep else "")) / ("base" if game == "ja" else "baseq3")
        maps, scripts = base / "maps", base / ("shaders" if game == "ja" else "scripts")
        maps.mkdir(parents=True, exist_ok=True)
        scripts.mkdir(exist_ok=True)
        (scripts / "shaderlist.txt").write_text("test\n")
        (scripts / "test.shader").write_text("textures/test/wall { { map $lightmap } }\n"
                                             "textures/test/stone { { map $lightmap } }\n")
        source = maps / "stored.map"
        bsp, cache = source.with_suffix(".bsp"), source.with_suffix(".lightcache")
        snapshot = maps / "stored.lsn"
        counter = 0

        def run(stage, *options, fail=False):
            nonlocal counter
            counter += 1
            result = subprocess.run([str(args.compiler.resolve()), "-threads", "1", "-game", game,
                                     "-fs_basepath", str(base.parent), stage, *options, str(source)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            output = result.stdout.decode(errors="replace")
            (base.parent / ("%02d.log" % counter)).write_text(output)
            assert bool(result.returncode) == fail, output
            return output

        def write(light):
            text = fixture(light)
            if keep:
                text = text.replace('"classname" "worldspawn"',
                                    '"classname" "worldspawn"\n"_keepLights" "1"')
            source.write_text(text)

        extra = '"targetname" "switch"' if game == "ja" else ""
        write(point(extra=extra))
        run("-bsp", "-meta")
        run("-vis", "-fast")
        run("-light", "-fast", "-lightcache")
        original_preview = preview(cache)
        original_style = re.search(rb'"style" "(\d+)"', lumps(bsp.read_bytes())[0])

        for name, light in [("move", point("64 0 96", extra=extra)),
                            ("color/intensity", point("64 0 96", "1 0.2 0.1", 350, extra))]:
            old = bsp.read_bytes()
            write(light)
            output = run("-light", "-fast", "-incremental")
            assert "Cache: valid" in output and "Trace cache: reused" in output, output
            counts = re.search(r"Lightmaps: (\d+) reused .* (\d+) relit", output)
            assert counts and all(int(n) > 0 for n in counts.groups()), output
            assert "BSP-stored light entities from source" in output, output
            incremental, sidecar = bsp.read_bytes(), cache.read_bytes()
            assert preview(cache) != original_preview, "moved BSP-stored light left the preview unchanged"
            if game == "ja":
                assert re.search(rb'"style" "(\d+)"', lumps(incremental)[0]).group(1) == original_style.group(1)
            # Styled channels can appear/disappear on newly reached surfaces;
            # collision geometry and visibility must stay unchanged.
            for index in (2, 3, 4, 5, 6, 7, 8, 9, 11, 16):
                assert lumps(old)[index] == lumps(incremental)[index], (name, index)
            run("-lightsnapbsp", "-lightsnap", str(snapshot))
            assert snapshot.read_bytes() == preview(cache), "cache preview differs from the updated BSP"
            # Independently refresh BSP entities through the existing onlyents
            # stage, then compare geometry and receiver lighting with a full pass.
            bsp.write_bytes(old)
            run("-bsp", "-onlyents")
            updated = bsp.read_bytes()
            assert lumps(updated)[1:] == lumps(old)[1:], "onlyents changed geometry or lighting"
            run("-light", "-fast", "-lightcache")
            reference = bsp.read_bytes()
            # A full styled build may pack atlas channels differently. Compare
            # every chart's raw samples and final pixels, grid and vertex colors.
            assert lighting_samples(sidecar) == lighting_samples(cache.read_bytes()), name
            for index in (1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 15, 16, 17):
                if index < len(lumps(reference)):
                    assert lumps(incremental)[index] == lumps(reference)[index], (name, index)
            bsp.write_bytes(incremental)
            cache.write_bytes(sidecar)
            print("PASS", game, "keepLights=" + str(keep), name, "and refreshed preview", flush=True)

        if game == "ja":
            before = bsp.read_bytes()
            write(point("64 0 96", extra='"targetname" "renamed"'))
            output = run("-light", "-fast", "-incremental", fail=True)
            assert "-bsp -onlyents" in output, output
            assert bsp.read_bytes() == before
            run("-bsp", "-onlyents")
            run("-light", "-fast", "-incremental")
            write(point("64 0 96", extra='"targetname" "renamed"\n"style" "3"'))
            output = run("-light", "-fast", "-incremental", fail=True)
            assert "-bsp -onlyents" in output, output
            run("-bsp", "-onlyents")
            run("-light", "-fast", "-incremental")
            write(point("32 0 96", extra='"targetname" "renamed"\n"style" "3"'))
            output = run("-light", "-fast", "-incremental")
            assert "Cache: valid" in output, output
            assert b'"switch_style" "3"' in lumps(bsp.read_bytes())[0]
            before = bsp.read_bytes()
            write("")
            output = run("-light", "-fast", "-incremental", fail=True)
            assert "-bsp -onlyents" in output, output
            assert bsp.read_bytes() == before
            print("PASS", game, "keepLights=" + str(keep), "style-group changes require onlyents", flush=True)
    print("All BSP-stored light regressions passed. Logs:", root)


if __name__ == "__main__":
    main()
