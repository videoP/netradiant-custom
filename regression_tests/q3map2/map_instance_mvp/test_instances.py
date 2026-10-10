"""End-to-end map instance checks against compiled Jedi Academy BSP data.

Run: python regression_tests/q3map2/map_instance_mvp/test_instances.py [q3map2]
The fixtures require no game installation; missing fixture texture warnings are
expected. All generated maps and BSPs live in a temporary directory.
"""

from collections import Counter
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
EXE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "install" / (
    "q3map2.exe" if sys.platform == "win32" else "q3map2"
)


def run(project, *args, success=True):
    result = subprocess.run(
        [str(EXE), "-game", "ja", "-threads", "1", "-fs_basepath", str(project), *map(str, args)],
        capture_output=True, text=True, errors="replace", timeout=60,
    )
    output = result.stdout + result.stderr
    if (result.returncode == 0) != success:
        raise AssertionError(output)
    return output


def bsp_data(path):
    data = path.read_bytes()
    assert data[:4] == b"RBSP", "Expected an ordinary Jedi Academy RBSP"
    lumps = [struct.unpack_from("<2i", data, 8 + i * 8) for i in range(18)]

    def lump(index):
        start, size = lumps[index]
        return data[start:start + size]

    text = lump(0).decode("ascii", errors="replace")
    entities = [dict(re.findall(r'"([^"\n]*)"\s*"([^"\n]*)"', body))
                for body in re.findall(r"\{([^{}]*)\}", text)]
    assert all(e.get("classname") != "func_instance" for e in entities)
    return lump, entities


def vertices(path, offset=(0, 0, 0)):
    """Compare visible geometry and repeating UVs, including entity models."""
    lump, entities = bsp_data(path)
    origins = {0: (0, 0, 0)}
    for ent in entities:
        if ent.get("model", "").startswith("*"):
            origins[int(ent["model"][1:])] = tuple(map(float, ent.get("origin", "0 0 0").split()))
    shaders = [lump(1)[i:i + 64].split(b"\0", 1)[0]
               for i in range(0, len(lump(1)), 72)]
    models, surfaces, drawverts = lump(7), lump(13), lump(10)
    result = Counter()
    for model in range(len(models) // 40):
        _, _, _, _, _, _, first, count, _, _ = struct.unpack_from("<6f4i", models, model * 40)
        origin = origins.get(model, (0, 0, 0))
        for surface in range(first, first + count):
            shader, _, kind, first_vert, num_verts = struct.unpack_from("<5i", surfaces, surface * 148)
            for vert in range(first_vert, first_vert + num_verts):
                x, y, z, s, t = struct.unpack_from("<5f", drawverts, vert * 80)
                xyz = tuple(round(v + o - shift, 3) for v, o, shift in zip((x, y, z), origin, offset))
                uv = tuple(round(v % 1, 3) % 1 for v in (s, t))
                result[(shaders[shader], kind, xyz, uv)] += 1
    return result


def master(file, offset=(2048, -1024, 256), extra=""):
    return ('{\n"classname" "worldspawn"\n"message" "master owns settings"\n}\n'
            '{\n"classname" "func_instance"\n'
            f'"file" "{file}"\n"origin" "{" ".join(map(str, offset))}"\n{extra}}}\n')


def box_brush(mins, maxs, shader):
    basis = ((0, (0, 1, 0), (0, 0, 1)),
             (1, (0, 0, 1), (1, 0, 0)),
             (2, (1, 0, 0), (0, 1, 0)))
    lines = []
    for axis, u, v in basis:
        for positive in (True, False):
            p = list(mins)
            p[axis] = maxs[axis] if positive else mins[axis]
            a, b = (v, u) if positive else (u, v)
            points = (p, [x + y * 64 for x, y in zip(p, a)],
                      [x + y * 64 for x, y in zip(p, b)])
            line = " ".join("( " + " ".join(map(str, point)) + " )" for point in points)
            lines.append(f"{line} {shader} 0 0 0 0.5 0.5 0 0 0")
    return "{\n" + "\n".join(lines) + "\n}\n"


def main():
    assert EXE.is_file(), f"Build q3map2 first: {EXE}"
    test_root = ROOT / ".cache" / "map-instance-tests"
    test_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="nrc-instances-", dir=test_root) as temp:
        project = Path(temp).resolve()
        assert project.parent == test_root.resolve(), "Temporary project must stay in the test directory"
        maps = project / "maps"
        maps.mkdir()
        shaders = project / "base" / "shaders"
        shaders.mkdir(parents=True)
        (shaders / "instance_test.shader").write_text(
            "textures/common/origin\n{\nsurfaceparm origin\nsurfaceparm nodraw\n}\n"
            "textures/common/areaportal\n{\nsurfaceparm areaportal\nsurfaceparm nodraw\n}\n", encoding="ascii")
        door_source = maps / "door.map"
        door_source.write_text((HERE / "maps" / "room.map").read_text() +
                              '{\n"classname" "func_door"\n"targetname" "door"\n' +
                              box_brush((-48, -8, 0), (48, 8, 192), "radiant_regression_tests/red") +
                              box_brush((-8, -8, 88), (8, 8, 104), "common/origin") + "}\n",
                              encoding="ascii")
        room_text = (HERE / "maps" / "room.map").read_text()
        depth = 0
        for world_end, char in enumerate(room_text):
            depth += (char == "{") - (char == "}")
            if depth == 0 and char == "}":
                break
        portal_source = maps / "portal.map"
        portal_source.write_text(room_text[:world_end] +
                                 box_brush((-512, -8, 0), (512, 8, 512), "common/areaportal") +
                                 room_text[world_end:], encoding="ascii")
        cases = {
            "axial": HERE / "maps" / "room.map",
            "patch": HERE.parent / "patch_seam" / "maps" / "patch_seam.map",
            "brush_primitives": HERE.parent / "tiny_structural_brush" / "maps" / "tiny_structural_brush.map",
            "decal": HERE.parent / "decal_misalignment" / "maps" / "decal_misalignment.map",
            "door": door_source,
            "portal": portal_source,
        }
        for name, source in cases.items():
            child = maps / f"{name}.map"
            if source != child:
                shutil.copyfile(source, child)
            parent = maps / f"{name}_master.map"
            parent.write_text(master(child.name), encoding="ascii")
            source_bytes = child.read_bytes()
            run(project, "-meta", child)
            compile_output = run(project, "-meta", parent)
            if name == "portal":
                assert re.search(r"\b1 areaportals", compile_output), compile_output
            original = vertices(child.with_suffix(".bsp"))
            expanded = vertices(parent.with_suffix(".bsp"), (2048, -1024, 256))
            assert original and original == expanded, f"{name}: geometry or UVs changed"
            _, entities = bsp_data(parent.with_suffix(".bsp"))
            assert entities[0]["message"] == "master owns settings"
            assert child.read_bytes() == source_bytes
            print(f"PASS: {name} geometry and UVs match the independently compiled source")

        mixed = maps / "mixed.map"
        mixed.write_text(master("axial.map", (6000, 0, 0)) +
                         master("brush_primitives.map", (-6000, 0, 0)).split("}\n", 1)[1], encoding="ascii")
        run(project, "-meta", mixed)
        expected = Counter()
        for name, x in (("axial", 6000), ("brush_primitives", -6000)):
            for (shader, kind, xyz, uv), count in vertices(maps / f"{name}.bsp").items():
                expected[(shader, kind, (xyz[0] + x, xyz[1], xyz[2]), uv)] += count
        assert vertices(mixed.with_suffix(".bsp")) == expected
        print("PASS: mixed source brush formats preserve geometry and UVs")

        shutil.copyfile(HERE / "maps" / "master.map", maps / "master.map")
        shutil.copyfile(HERE / "maps" / "room.map", maps / "room.map")
        parent = maps / "master.map"
        run(project, "-meta", parent)
        run(project, "-vis", "-fast", parent.with_suffix(".bsp"))
        light_output = run(project, "-light", "-fast", "-faster", parent.with_suffix(".bsp"))
        assert re.search(r"\b2 total lights", light_output), light_output
        _, entities = bsp_data(parent.with_suffix(".bsp"))
        spawns = [tuple(map(float, e["origin"].split())) for e in entities
                  if e.get("classname") == "info_player_deathmatch"]
        assert sorted(spawns) == [(-2048, -64, 384), (2048, -64, 384)]
        print("PASS: two placements, normal BSP/VIS/LIGHT output, translated entities, no placeholders")

        for name, body, diagnostic in (
            ("missing", master("missing.map"), "missing.map"),
            ("rotation", master("room.map", extra='"angles" "0 90 0"\n'), "rotation"),
            ("cycle", master("cycle.map"), "own map"),
            ("fixup", master("room.map", extra='"fixup_style" "0"\n'), "name fixups"),
            ("parameters", master("room.map", extra='"replace01" "$brightness 400"\n'), "parameter overrides"),
            ("bad_origin", master("room.map").replace('"origin" "2048 -1024 256"', '"origin" "bad value"'), "Invalid func_instance origin"),
        ):
            path = maps / f"{name}.map"
            path.write_text(body, encoding="ascii")
            output = run(project, "-meta", path, success=False)
            assert diagnostic in output, output
            assert not path.with_suffix(".bsp").exists()
        (maps / "nested.map").write_text(master("room.map"), encoding="ascii")
        path = maps / "bad_nested.map"
        path.write_text(master("nested.map"), encoding="ascii")
        assert "Nested func_instance" in run(project, "-meta", path, success=False)
        (maps / "malformed.map").write_text((HERE / "maps" / "room.map").read_text() +
                                           '\n{\n"classname" "info_null"\n', encoding="ascii")
        path = maps / "bad_source.map"
        path.write_text(master("malformed.map"), encoding="ascii")
        assert "EOF without closing" in run(project, "-meta", path, success=False)
        print("PASS: invalid sources and unsupported instance options stop compilation")


if __name__ == "__main__":
    main()
