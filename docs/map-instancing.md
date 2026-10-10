# Map instancing: first milestone

A master map can link separate `.map` files. NRC shows their brushes, patches,
and entity representations in its regular 2D and 3D views. Each link is one
selectable object. The bundled Q3Map2 expands the links before BSP construction,
so the result is an ordinary Jedi Academy BSP.

## Working with a team

Keep the master and source files in the same shared project tree, for example:

```text
maps/
  arena.map
  sections/
    courtyard.map
    entrance.map
```

Each mapper edits their own source file. Share saved revisions through your
existing version control or file sharing workflow. Coordinate boundaries and
entity names with the other mappers. This feature does not synchronize two
people editing the same file.

1. Save the master map.
2. Choose **File / Add Map Instance...** and select one or more existing source
   `.map` files. Use Ctrl or Shift in the file picker to select several at once.
3. Click its geometry to select the whole instance. Use the regular move tool
   or edit its `origin` in the entity inspector. Duplicating it preserves the link.
4. Choose **File / Open Instance Source** to edit the selected source. NRC uses
   its normal prompt for unsaved parent edits. Return through the recent maps
   menu after saving the source. Opening the master loads current sources.
5. While keeping a master open, choose **File / Reload Map Instances** after a
   collaborator saves or you receive their new revision. All its placements
   refresh together. Their positions and the parent's undo history are preserved.
6. Compile through the ordinary Build menu, using this build's bundled Q3Map2.
   Direct Q3Map2 invocations work with the same master filename and normal options.

Adding multiple files is one undoable operation. All new instances are selected
and use the same insertion offset, preserving their source coordinates relative
to one another. Sections already laid out in world coordinates line up when
added at `0 0 0`. Sources built around local zero may overlap initially; select
and move each placement as needed.

Build source sections around a useful local origin. Existing maps retain their
original coordinates: an instance with `origin` set to `0 0 0` places those
coordinates unchanged. **Save Selected** can create a source file, but it keeps
the original coordinates and selection. Converting a selection into a linked
instance with a new pivot is a later workflow.

## Instances panel

Open **View / Map Instances** for a dockable list of all placements in the
current master. The panel can be moved, resized, or floated; its layout is
remembered with the rest of the editor.

- **Visible** hides or shows a placement in the editor. This does not unload
  its geometry, modify the master, or exclude it from compilation. Visibility
  resets when the map is reopened.
- Each row shows its source path, placement name/origin, source modification
  date in local time, and status: **Loaded**, **Changed**, **Missing**,
  **Failed**, or **Not loaded**. Changes are detected while the panel is visible;
  geometry refresh remains manual. Hover a failed status for the error.
- Click a row to select the placement. Ctrl/Shift selects several. Double-click
  a row or use **Focus** to center the views on the selection.
- **Add…** accepts multiple existing maps. **Open Source** opens the one selected
  placement's source through the normal unsaved-edits prompt.
- The row's **Refresh**, **Refresh Selected**, and **Refresh All** reload sources.
  Refreshing a source updates every placement using it. Failed refreshes retain
  the last valid preview. A restored preview without a tracked
  source revision shows **Changed** until refreshed.
- **Remove** removes the selected references from the master in one undoable
  operation. It leaves the source files on disk.

## File representation

Only the reference is saved in the master:

```text
{
"classname" "func_instance"
"file" "sections/courtyard.map"
"origin" "1024 512 0"
}
```

Paths resolve relative to the map containing the reference. Forward and backward
slashes, nested directories, and `..` are accepted. The Add command stores a
relative path. Keep the project layout intact when sharing or moving it.

Source geometry is protected from selection and component editing through the
master, including through the entity list. Saving the master excludes preview
contents and does not write the source. Edit the source itself to change them.

The master owns worldspawn settings. Sources contribute world geometry and
their entities. Translation preserves texture projection and patch control
points. Source brush entities retain their compiler and runtime behavior.

## Current limits

- Translation only. Rotation, scaling and skew are rejected by the instance
  move implementation; nonzero instance angles stop compilation.
- Refresh is manual. Changes are also read when opening the master and compiling.
- Entity names are preserved. Use globally unique `targetname` and script names
  across sections. Repeated sources containing named gameplay entities can
  create collisions. Automatic name fixups and parameter replacement are later
  milestones; unsupported fixup and replacement keys stop compilation.
- Source maps cannot contain nested instances or Source instance metadata.
- Editing with the surrounding parent visible, collapse to ordinary geometry,
  and Create Instance from Selection are later workflows.
- Editor previews have their own geometry copies per placement. Each source is
  parsed once per reload; compiler caching and larger scene optimization remain.

Missing, malformed or unsupported sources are reported in the console. Reload
keeps the last valid preview when a source cannot be read completely. Compilation
stops for invalid sources. A source that fails on the first load has no geometry
preview; its reference remains available in the entity list.

## Example and validation

`regression_tests/q3map2/map_instance_mvp/maps/master.map` references two
placements of `room.map`. The room reuses the coarse normal regression fixture;
its regression textures may display as missing without the regression assets.

Compiler checks:

```text
python regression_tests/q3map2/map_instance_mvp/test_instances.py
```

These inspect compiled JA geometry and UVs against independently compiled
sources, including axial and brush primitive projections, patches, decals,
origin brushes in door entities, areaportals, and mixed source formats. They also exercise
BSP/VIS/LIGHT, repeated source lights and spawns, and invalid references.

The editor regression harness in `editor_check.h` is enabled only when
`radiant/main.cpp` is compiled with `RADIANT_MAP_INSTANCE_TEST`. Build that
executable into a separate portable runtime with the normal modules and assets,
and open disposable copies of `master.map` and `room.map` there. The test edits
its disposable source and writes `editor-results.txt` next to the master.
Use native Windows OpenGL; Qt's offscreen platform cannot initialize NRC's font
rendering service. Production builds omit the harness.

On the Windows/MSYS2 development setup, build a separate test executable:

```sh
make OS=Win32 DEPENDENCIES_CHECK=off ASSIMP_INTERNAL=yes \
  INSTALLDIR=.build-tmp/instance-test-bin CPPFLAGS=-DRADIANT_MAP_INSTANCE_TEST \
  -W radiant/main.cpp .build-tmp/instance-test-bin/radiant.exe
```

Then run the isolated Windows runner from PowerShell:

```powershell
./regression_tests/q3map2/map_instance_mvp/run_editor_check.ps1 `
  -TestEditor .build-tmp/instance-test-bin/radiant.exe
```

The runner creates its own settings and disposable maps, uses the normal
installed modules, and places test windows outside the visible desktop. It
checks axial, patch, brush primitive, and geometry-only sources. Because make
shares object files between builds, rebuild `radiant/main.cpp` with the normal
flags afterward before producing a production executable:

```sh
make OS=Win32 DEPENDENCIES_CHECK=off ASSIMP_INTERNAL=yes \
  -W radiant/main.cpp install/radiant.exe
```

The editor harness covers rendered previews, click selection, protected contents,
move/undo/redo, duplication, save/reopen, source changes, missing and malformed
reloads, and preservation of the parent's undo history. It also checks panel
rows, selection and visibility controls, hidden reference saving, source status,
refresh scope, bulk removal/undo/redo, and map switches. In-game play testing is
still needed for a particular project and its custom entities or shaders.
