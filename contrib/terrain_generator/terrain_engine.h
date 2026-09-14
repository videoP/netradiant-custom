#pragma once

#include "terrain_math.h"

#include <map>
#include <utility>
#include <tuple>

using HeightMap   = std::map<std::pair<double, double>, double>;
using WallMap     = std::map<std::pair<double, double>, double>;

// A standard-terrain grid vertex. x/y may be jittered away from the grid's
// exact (rounded) key position when jitter_grid is enabled — z is the height.
struct GridPoint
{
	double x, y, z;
};
using TerrainMap = std::map<std::pair<double, double>, GridPoint>;

struct TunnelMaps
{
	HeightMap floor_map;
	HeightMap ceiling_map;
	WallMap   left_wall_map;
	WallMap   right_wall_map;
	double    step_z;
};

enum class ShapeType {
	Flat        = 0,
	Hill        = 1,
	Crater      = 2,
	Ridge       = 3,
	Slope       = 4,
	Volcano     = 5,
	Valley      = 6,
	Tunnel      = 7,
	SlopeTunnel = 8
};

enum class NoiseType {
	Perlin  = 0,
	Simplex = 1,
	Random  = 2
};

// Direction Slope / Ridge / Valley / Tunnel / SlopeTunnel run along. Ignored
// by radially symmetric shapes (Hill, Crater, Volcano).
enum class Axis {
	X = 0,
	Y = 1
};

BrushData make_manual_brush_data( double width, double length, double height );

void adjust_bounds_to_fit_grid( BrushData& target, double step_x, double step_y );

TerrainMap generate_height_map( const BrushData& target, double step_x, double step_y,
                                ShapeType shape_type, double shape_height,
                                double variance, double frequency,
                                NoiseType noise_type, double terrace_step,
                                Axis axis, bool jitter_grid, double grid_step );

TunnelMaps generate_tunnel_height_maps( const BrushData& target, double step_x, double step_y,
                                        double cave_height, double slope_height,
                                        double variance, double frequency,
                                        NoiseType noise_type, double terrace_step,
                                        Axis axis, double grid_step );
