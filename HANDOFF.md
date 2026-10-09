# netradiant-custom — large maps fork

Working notes for adding features to this repo. Written for someone picking it
up cold.

## What this is

`videoP/netradiant-custom`, a fork of `Garux/netradiant-custom`, carrying a
body of work aimed at maps with hundreds of thousands of brushes: load time,
frame rate, memory, and two q3map2 compile stages.

- `origin` → your fork, push here
- `upstream` → `Garux/netradiant-custom`, **push deliberately disabled**
  (`git remote set-url --push upstream DISABLE`)

`master` holds the work and is what is published. Tag `largemaps-20260914` is
the first release.

To take upstream changes:

```sh
git fetch upstream
git merge upstream/master
```

## Layout — read this before anything else

The repo root **is** the source tree: `radiant/`, `libs/`, `tools/`, `plugins/`.

Build output goes to `install/`, which is gitignored and is also a runnable
editor (runtime DLLs and data live there, not at the repo root). `radiant.bat`
and `q3map2.bat` at the root just forward into it.

Do not put binaries at the repo root. `plugins/` there is plugin *source*, and
radiant looks for `plugins/`, `modules/` next to the exe — an exe at the root
would load the wrong thing.

If you ever see an older tree where the source sits in a nested
`netradiant-custom-20251023/` folder with the exe at the top level, that is the
predecessor to this repo and is not this layout.

## Build

```sh
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
export MSYSTEM=MINGW64
export TMPDIR=/d/Code/tmp TMP=/d/Code/tmp TEMP=/d/Code/tmp   # see gotcha 1
mkdir -p /d/Code/tmp

cd /d/Code/netradiant-custom
make OS=Win32 DEPENDENCIES_CHECK=off ASSIMP_INTERNAL=yes \
     binaries-radiant binaries-q3map2 -j8
```

All three make flags are required.

**Gotcha 1 — GCC temp files.** If the compiler says
`Cannot create temporary file in C:\WINDOWS\: Permission denied`, the `TMPDIR`
export above did not reach it. Wrap the compiler instead:

```sh
cat > /d/Code/tmp/cxx.sh <<'EOF'
#!/bin/sh
export TMPDIR=/d/Code/tmp TMP=/d/Code/tmp TEMP=/d/Code/tmp
exec /c/msys64/mingw64/bin/g++.exe "$@"
EOF
# same for gcc.exe as cc.sh, chmod +x both, then add:
#   CXX=/d/Code/tmp/cxx.sh CC=/d/Code/tmp/cc.sh
```

**Gotcha 2 — `CC` must be gcc, not g++.** assimp's `contrib/zip/src/zip.c` is C
and fails to compile as C++.

**Gotcha 3 — build both targets.** `binaries-radiant` alone will not catch a
q3map2 compile error, and vice versa. A whole q3map2 break went unnoticed here
because only the radiant target was being built.

After a source-only change, `install/` already has the DLLs and data, so the
rebuilt exes are immediately runnable. After changing anything under
`setup/data/tools/` you also need the data step:

```sh
echo 6 > install/RADIANT_MAJOR && echo 0 > install/RADIANT_MINOR
cp -r setup/data/tools/* install/
```

`RADIANT_MAJOR`/`RADIANT_MINOR` must exist next to the exe or radiant refuses to
start with "doesn't match what the latest setup has configured" — see
`check_version()` in `radiant/main.cpp`.

## Adding a feature

### The rule this codebase runs on

Every performance change is **opt-in, default off, and latched**, so it can be
A/B'd against stock behaviour without a rebuild. With everything off the code
paths are the stock ones.

Measure before choosing what to fix. The recurring lesson here is that guessing
between two plausible causes has a poor record: the map writer looked like an
I/O problem and was mostly `strtod`; a frame that drew almost nothing was
spending all its time recomputing container bounds; `FilterDrawsurfsIntoTree`
looked like the next quadratic and turned out to be O(n log n) and not worth
touching.

### An editor option

All of it lives in `radiant/largemap.{h,cpp}`. Four places:

1. **Declare** in `largemap.h` — `extern LatchedBool g_largemap_yourThing;`
   with a doc comment saying what it does *and what it costs*.
2. **Define** in `largemap.cpp` —
   `LatchedBool g_largemap_yourThing( false, "Your thing" );`
3. **Checkbox** in `LargeMap_constructPreferences()`. First argument is the
   group: `Load`, `View`, `Editing`, `Compile`. Give it a `setToolTip()` with
   the measured numbers — those tooltips are the real documentation.
4. **Register** in `LargeMap_Construct()` so it persists, and add it to the
   array in `LargeMap_reportActive()`.

Then gate the code with `g_largemap_yourThing.m_value`.

**Crossing into module DLLs.** `libs/` headers and `brush.h` are compiled into
the module DLLs (`entity.dll`, `mapq3.dll`, …), which cannot see radiant's
preference code. Mirror the flag as a plain `extern bool` and push the latched
value to it once in `LargeMap_Construct()` — see `g_scriptTokeniser_fastPath`
and `g_brush_lazyComponents`. If even that will not reach (the value is needed
inside a module that constructs its own objects), use a compile-time switch
instead — see `RADIANT_FAST_NUMBER_PARSE` in `libs/stringio.h`.

**When not to add an option.** A saving that comes from a struct being
physically smaller cannot be runtime-switched — keeping the old fields for the
"off" path forfeits the whole saving. The face tangent basis work
(`WindingVertex`) is the example. Compile-time switch or nothing.

### A q3map2 compile flag

Five places, following `-cullgrid` / `-tjgrid`:

1. `tools/quake3/q3map2/q3map2.h` — `inline bool yourFlag;` next to `cullGrid`.
2. `tools/quake3/q3map2/main.cpp` — parse it in the **general options** block,
   not in `BSPMain`. General options are accepted by every stage, so appending
   the switch to `-vis` and `-light` commands does not produce warnings.
3. `tools/quake3/q3map2/help.cpp` — one line in `HelpCommon()`.
4. `radiant/largemap.{h,cpp}` — a **non-latched** `bool` (not `LatchedBool`) and
   a checkbox in the `Compile` group. It is a command-line switch, not editor
   state, so it takes effect on the next compile with no restart.
5. `radiant/qe3.cpp` — append it to the `LargeMapFlags` variable in
   `build_init_variables()`.

`radiant/build.cpp` already injects `[LargeMapFlags]` into any build command
that runs q3map2, the same way `[ExtraResourcePaths]` is injected.

## Testing a compile-stage change

The bar for the two existing compile options is a **byte-identical .bsp**. Both
meet it. Hold new ones to the same standard.

`tools/largemap-testing/` has two generators. They emit valid Quake 3 `.map`
files with plane winding derived from q3map2's own `PlaneFromPoints`, asserted
per face:

```sh
python tools/largemap-testing/gen_cullsides_map.py 70 out.map   # dense cube grid
python tools/largemap-testing/gen_tjunction_map.py 80 out.map   # varied heights,
                                                                # 25% rotated
```

The first stresses `CullSides` (many coincident and buried faces). The second
stresses `FixTJunctions` — a regular grid collapses to very few distinct edge
lines and will not exercise it, which is why it exists separately.

They need a `test/wall` and `test/nodraw` shader; drop this in
`<basepath>/scripts/test.shader`:

```
textures/test/nodraw { surfaceparm nodraw surfaceparm nomarks surfaceparm nolightmap }
textures/test/wall   { { map $lightmap } }
```

Then compile the **same file** twice and compare:

```sh
q3map2 -v -bsp -game quake3 -fs_basepath <dir> <dir>/baseq3/maps/t.map
cp t.bsp A.bsp
q3map2 -v -yourflag -bsp -game quake3 -fs_basepath <dir> <dir>/baseq3/maps/t.map
cmp -l A.bsp t.bsp
```

Compile the same filename both times — the map name is embedded in the BSP.
Expect **1–3 differing bytes**: the compile timestamp in the entity lump. More
than that means a real difference.

`-tjverify` is the model for verifying an index: it runs the new lookup *and*
the scan it replaces on every query and counts disagreements. It reports zero.
Something equivalent is worth building for any structure that replaces a scan.

Known ceiling: the generators hit `max_map_draw_surfs` around 40k brushes. Use
`-maxmapdrawsurfs` to push past it, or accept the range.

## Release

```sh
.\package-build.ps1 -Standalone -DeployedDirectory "<an install you have run>"
```

Produces a ~47 MB standalone zip, or a ~3 MB patch zip without `-Standalone`.
`-DeployedDirectory` hashes every packaged file against a copy you have
actually run, so a zip cannot contain a build that was never started.

Zips are named after the commit, with `-dirty` and a minute stamp if the tree
was modified. Then:

```sh
git tag -a largemaps-YYYYMMDD -m "..."
git push origin master
git push origin largemaps-YYYYMMDD
```

GitHub → Releases → Draft a new release → pick the tag → attach the zip.
`gh` is not installed here, so the release itself is done in the browser.

## Conventions

- Match surrounding style: `ydnar`-era C++ in q3map2 (`/* */`, tabs,
  `lowerCamelCase` locals); radiant is similar.
- Comments explain *why*, with the measurement that motivated it. Follow what
  is already in `staticbatch.h`, `framestats.h`, `brushmemory.h`.
- q3map2: `C_LIQUID` means a liquid volume; `C_TRANSLUCENT` includes clips and
  triggers and is rarely what you want. Compile flags are in `games.cpp`.
- **Do not add AI attribution to commits, and do not commit AI-authored
  documentation into this repo.** Author is `videoP <videoprofess@gmail.com>`;
  the repo's own git config already sets this, so do not pass `--author`.

## Traps hit while building this

- **Heredoc escape mangling.** Writing C++ through a shell heredoc into Python
  silently corrupts `\n` (becomes a real newline) and `\b` (becomes a 0x08
  byte). Seven headers ended up with `rief` instead of `\brief`. If you edit
  source through a script, grep the result for control characters afterwards.
- **Upstream drift.** This fork's base moved ~11 months between the original
  work and the port. `mapDrawSurface_t::numVerts` became an accessor;
  `setKeyValue()` and `TraceLine()` changed shape. A patch applying cleanly
  proves nothing — only a build does.
- **`install/` is gitignored**, so a clean clone has no editor until you build
  and run the data step above.

## Current state

21 commits on `master`, tagged `largemaps-20260914`, pushed.

Ten options under Settings → Large Maps: eight editor-side (latched, need a
restart) and two compile flags (immediate). The console prints which are
actually in force on every map load, which matters because what is ticked and
what is running differ until a restart.

Not everything got an option — improvements inside an existing option, pure
wins with no downside, diagnostics (console output and View → Show Stats), and
one bug fix (a save that failed partway through was reported as successful) are
unconditional by design.

Open thread: on a 4x larger stress map the dominant cost moved to memory
pressure and page faults rather than any single stage. The memory reporting
(`brushmemory.h`) and frame timing (`framestats.h`) exist to answer that, and
it needs a second machine with different RAM as a control.

The `oldrepo` remote points at the predecessor repo on local disk. Remove it
once you no longer need to diff against it:

```sh
git remote remove oldrepo
```
