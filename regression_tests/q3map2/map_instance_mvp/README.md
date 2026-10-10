# Linked map regression fixtures

`maps/master.map` places `maps/room.map` twice, at X = 2048 and X = -2048.
`room.map` is copied from the coarse normal regression fixture. Both placements
should show real geometry in NRC and compile into one ordinary Jedi Academy BSP.

Run `python regression_tests/q3map2/map_instance_mvp/test_instances.py` from the
repository root after building Q3Map2. An optional first argument selects a
different Q3Map2 executable. Generated compilation files use a temporary folder.

For the editor regression harness and mapper instructions, see
[the map instancing guide](../../../docs/map-instancing.md).
