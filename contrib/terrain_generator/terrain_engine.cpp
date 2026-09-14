#include "terrain_engine.h"
#include "noise.h"

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <numbers>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double round2( double v ){
	return std::round( v * 100.0 ) / 100.0;
}

static double random_double(){
	return (double)std::rand() / (double)RAND_MAX;
}

static double sample_noise( NoiseType noise_type, double x, double y ){
	switch ( noise_type ) {
	case NoiseType::Perlin:  return Perlin::noise( x, y );
	case NoiseType::Simplex: return Simplex::noise( x, y );
	default:                 return random_double() * 2.0 - 1.0;
	}
}

// A too-thin jittered triangle makes the engine's corner-plane-intersection
// math ill-conditioned, occasionally producing a wildly-off vertex that
// corrupts selection bounds on regeneration. Fix: snap any cell whose
// triangles fall under a minimum area back to its exact grid positions —
// always safe. One forward pass suffices since a revert only ever moves a
// vertex toward its nominal (safer) position, never away from it.
static void repair_thin_triangles( TerrainMap& height_map, const BrushData& target,
                                    double step_x, double step_y ){
	constexpr double MIN_AREA_FRACTION = 0.15; // of a normal half-cell triangle
	const double min_area = MIN_AREA_FRACTION * 0.5 * step_x * step_y;

	auto tri_area = []( const GridPoint& a, const GridPoint& b, const GridPoint& c ) -> double {
		return 0.5 * std::abs( ( b.x - a.x ) * ( c.y - a.y ) - ( c.x - a.x ) * ( b.y - a.y ) );
	};
	auto lookup = [&]( double kx, double ky ) -> GridPoint* {
		auto it = height_map.find( { round2( kx ), round2( ky ) } );
		return it != height_map.end() ? &it->second : nullptr;
	};
	auto revert = [&]( double gx, double gy ){
		if ( GridPoint* p = lookup( gx, gy ) ) {
			p->x = gx;
			p->y = gy;
		}
	};

	int x_index = 0;
	for ( double x = target.min_x; x < target.max_x - 0.01; x += step_x, ++x_index ) {
		int y_index = 0;
		for ( double y = target.min_y; y < target.max_y - 0.01; y += step_y, ++y_index ) {
			double mx = x + step_x < target.max_x ? x + step_x : target.max_x;
			double my = y + step_y < target.max_y ? y + step_y : target.max_y;

			GridPoint* bl = lookup( x,  y  );
			GridPoint* tl = lookup( x,  my );
			GridPoint* br = lookup( mx, y  );
			GridPoint* tr = lookup( mx, my );
			if ( !bl || !tl || !br || !tr )
				continue;

			// Matches the triangle split insert_brush_into actually builds.
			bool alt_dir = ( ( x_index + y_index ) % 2 ) != 0;
			bool bad = !alt_dir
			    ? ( tri_area( *bl, *tl, *br ) < min_area || tri_area( *tr, *br, *tl ) < min_area )
			    : ( tri_area( *tl, *tr, *bl ) < min_area || tri_area( *tr, *br, *bl ) < min_area );

			if ( bad ) {
				revert( x, y ); revert( x, my ); revert( mx, y ); revert( mx, my );
			}
		}
	}
}

// ---------------------------------------------------------------------------

BrushData make_manual_brush_data( double width, double length, double height ){
	BrushData b;
	b.min_x   = -width  / 2.0;
	b.max_x   =  width  / 2.0;
	b.min_y   = -length / 2.0;
	b.max_y   =  length / 2.0;
	b.min_z   = 0.0;
	b.max_z   = height;
	b.width_x  = width;
	b.length_y = length;
	b.height_z = height;
	return b;
}

void adjust_bounds_to_fit_grid( BrushData& target, double step_x, double step_y ){
	double new_width  = std::max( step_x, std::round( target.width_x  / step_x ) * step_x );
	double new_length = std::max( step_y, std::round( target.length_y / step_y ) * step_y );

	if ( std::abs( target.width_x  - new_width  ) > 0.001 ||
	     std::abs( target.length_y - new_length ) > 0.001 ) {
		double diff_x = new_width  - target.width_x;
		double diff_y = new_length - target.length_y;

		target.min_x   = std::round( target.min_x - diff_x / 2.0 );
		target.max_x   = target.min_x + new_width;
		target.min_y   = std::round( target.min_y - diff_y / 2.0 );
		target.max_y   = target.min_y + new_length;
		target.width_x  = new_width;
		target.length_y = new_length;
	}
}

// ---------------------------------------------------------------------------
// Standard heightmap
// ---------------------------------------------------------------------------

TerrainMap generate_height_map( const BrushData& target, double step_x, double step_y,
                                ShapeType shape_type, double shape_height,
                                double variance, double frequency,
                                NoiseType noise_type, double terrace_step,
                                Axis axis, bool jitter_grid, double grid_step ){
	TerrainMap height_map;

	double seed_x = random_double() * 10000.0;
	double seed_y = random_double() * 10000.0;

	// Max fraction of a cell a vertex may drift by. Kept under 0.5 so two
	// neighbors jittering toward each other can never cross.
	constexpr double JITTER_FRACTION = 0.35;

	for ( double x = target.min_x; x <= target.max_x + 0.01; x += step_x ) {
		for ( double y = target.min_y; y <= target.max_y + 0.01; y += step_y ) {
			double nx = target.width_x  > 0 ? ( x - target.min_x ) / target.width_x  : 0.0;
			double ny = target.length_y > 0 ? ( y - target.min_y ) / target.length_y : 0.0;

			// Position along the axis Slope / Ridge / Valley run along.
			double na = ( axis == Axis::X ) ? nx : ny;

			double center_dist = std::min( 1.0, std::sqrt(
				( nx - 0.5 ) * ( nx - 0.5 ) + ( ny - 0.5 ) * ( ny - 0.5 ) ) / 0.5 );

			double base_z = 0.0;
			switch ( shape_type ) {
			case ShapeType::Hill: {
				base_z = shape_height * 0.5 * ( 1.0 + std::cos( center_dist * std::numbers::pi ) );
				break;
			}
			case ShapeType::Crater: {
				base_z = shape_height * 0.5 * ( 1.0 - std::cos( center_dist * std::numbers::pi ) );
				break;
			}
			case ShapeType::Ridge: {
				double dist = std::min( 1.0, std::abs( na - 0.5 ) / 0.5 );
				base_z = shape_height * 0.5 * ( 1.0 + std::cos( dist * std::numbers::pi ) );
				break;
			}
			case ShapeType::Slope: {
				base_z = shape_height * na;
				break;
			}
			case ShapeType::Volcano: {
				double mountain   = shape_height * 0.5 * ( 1.0 + std::cos( center_dist * std::numbers::pi ) );
				double crater_dist = std::min( 1.0, center_dist / 0.35 );
				double crater     = ( shape_height * 0.7 ) * 0.5 * ( 1.0 + std::cos( crater_dist * std::numbers::pi ) );
				base_z = mountain - crater;
				break;
			}
			case ShapeType::Valley: {
				double dist = std::min( 1.0, std::abs( na - 0.5 ) / 0.5 );
				base_z = shape_height * 0.5 * ( 1.0 - std::cos( dist * std::numbers::pi ) );
				break;
			}
			default:
				break;
			}

			// Terrace before noise is added, so terrace bands stay flat strips
			// instead of being broken up by per-vertex noise.
			double shape_z = target.max_z + base_z;
			if ( shape_type != ShapeType::Flat && terrace_step > 0.0 )
				shape_z = std::floor( shape_z / terrace_step ) * terrace_step;

			double noise_z = 0.0;
			if ( variance > 0.0 ) {
				if ( noise_type == NoiseType::Random ) {
					noise_z = ( random_double() * ( variance * 2.0 ) ) - variance;
				} else {
					noise_z = sample_noise( noise_type,
					                        ( x + seed_x ) * frequency,
					                        ( y + seed_y ) * frequency ) * variance;
				}
			}

			// Snap only the noise to grid_step, anchored at shape_z rather than
			// absolute world Z — anchoring at world Z made a large step's effect
			// depend on where the brush happened to sit in the map (sometimes
			// flattening it, sometimes shifting its whole height).
			double snapped_noise = std::round( noise_z / grid_step ) * grid_step;
			double final_z = std::round( shape_z + snapped_noise );

			// High variance on a thin target can push the surface at/below the
			// floor. Clamping to exactly min_z isn't enough — a whole clamped
			// neighborhood would give a cell zero thickness (still a degenerate,
			// hole-causing brush) — so clamp to a small positive margin instead.
			constexpr double MIN_THICKNESS = 4.0;
			final_z = std::max( final_z, target.min_z + MIN_THICKNESS );

			// Jitter the vertex's world position (not its grid key) so triangles
			// vary in size instead of tiling identical rectangles. Boundary
			// vertices are left unjittered so the footprint matches target bounds.
			// Offset is a random whole number of grid_step increments, picked
			// directly within [-max_steps, max_steps] so it can never overshoot
			// JITTER_FRACTION's safety margin the way round-then-clamp could.
			auto grid_jitter = [&]( double step ) -> double {
				int max_steps = (int)std::floor( ( JITTER_FRACTION * step ) / grid_step );
				if ( max_steps <= 0 )
					return 0.0;
				int steps = (int)( random_double() * ( 2 * max_steps + 1 ) ) - max_steps;
				return std::clamp( steps, -max_steps, max_steps ) * grid_step; // guards random_double()==1.0
			};

			double jx = x, jy = y;
			if ( jitter_grid ) {
				const bool interior_x = ( x > target.min_x + 0.01 && x < target.max_x - 0.01 );
				const bool interior_y = ( y > target.min_y + 0.01 && y < target.max_y - 0.01 );
				if ( interior_x )
					jx = std::round( x + grid_jitter( step_x ) );
				if ( interior_y )
					jy = std::round( y + grid_jitter( step_y ) );
			}

			height_map[{ round2( x ), round2( y ) }] = GridPoint{ jx, jy, final_z };
		}
	}

	if ( jitter_grid )
		repair_thin_triangles( height_map, target, step_x, step_y );

	return height_map;
}

// ---------------------------------------------------------------------------
// Tunnel heightmaps
// ---------------------------------------------------------------------------

TunnelMaps generate_tunnel_height_maps( const BrushData& target, double step_x, double step_y,
                                        double cave_height, double slope_height,
                                        double variance, double frequency,
                                        NoiseType noise_type, double terrace_step,
                                        Axis axis, double grid_step ){
	TunnelMaps result;

	double seed_floor_x = random_double() * 10000.0;
	double seed_floor_y = random_double() * 10000.0;
	double seed_ceil_x  = random_double() * 10000.0;
	double seed_ceil_y  = random_double() * 10000.0;
	double seed_wall_l  = random_double() * 10000.0;
	double seed_wall_r  = random_double() * 10000.0;

	// Cross-section (rounding) axis: X for a Y-running tunnel, Y for an
	// X-running tunnel. Reproduces the original formulas exactly when axis == Y.
	const bool   along_y   = ( axis == Axis::Y );
	const double center_cs = along_y ? ( target.min_x + target.max_x ) / 2.0
	                                  : ( target.min_y + target.max_y ) / 2.0;
	const double half_cs   = along_y ? target.width_x  / 2.0
	                                  : target.length_y / 2.0;

	// Floor and ceiling
	for ( double x = target.min_x; x <= target.max_x + 0.01; x += step_x ) {
		for ( double y = target.min_y; y <= target.max_y + 0.01; y += step_y ) {
			double cs_pos = along_y ? x : y;
			double t = half_cs > 0 ? std::min( 1.0, std::abs( cs_pos - center_cs ) / half_cs ) : 0.0;
			double blend = 1.0 - std::sqrt( std::max( 0.0, 1.0 - t * t ) );

			double along_pos = along_y
			    ? ( target.length_y > 0 ? ( y - target.min_y ) / target.length_y : 0.0 )
			    : ( target.width_x  > 0 ? ( x - target.min_x ) / target.width_x  : 0.0 );
			double base_z = target.max_z + slope_height * along_pos;
			double floor_base = base_z + blend * ( cave_height * 0.25 );
			double ceil_base  = base_z + cave_height - blend * ( cave_height * 0.25 );

			// Terrace before noise, as above, so bands stay flat.
			if ( terrace_step > 0.0 ) {
				floor_base = std::floor( floor_base / terrace_step ) * terrace_step;
				ceil_base  = std::ceil(  ceil_base  / terrace_step ) * terrace_step;
			}

			double floor_noise = 0.0, ceil_noise = 0.0;
			if ( variance > 0.0 ) {
				if ( noise_type == NoiseType::Random ) {
					floor_noise = random_double() * variance;
					ceil_noise  = random_double() * variance;
				} else {
					floor_noise = std::abs( sample_noise( noise_type,
					    ( x + seed_floor_x ) * frequency, ( y + seed_floor_y ) * frequency ) ) * variance;
					ceil_noise  = std::abs( sample_noise( noise_type,
					    ( x + seed_ceil_x  ) * frequency, ( y + seed_ceil_y  ) * frequency ) ) * variance;
				}
			}

			// Snap only the noise, anchored at floor_base/ceil_base, as above.
			double snapped_floor_noise = std::round( floor_noise / grid_step ) * grid_step;
			double snapped_ceil_noise  = std::round( ceil_noise  / grid_step ) * grid_step;

			double floor_z = floor_base + snapped_floor_noise;
			double ceil_z  = ceil_base  - snapped_ceil_noise;

			if ( floor_z > ceil_z ) {
				double mid = ( floor_z + ceil_z ) / 2.0;
				floor_z = mid;
				ceil_z  = mid;
			}

			result.floor_map[   { round2( x ), round2( y ) }] = std::round( floor_z );
			result.ceiling_map[ { round2( x ), round2( y ) }] = std::round( ceil_z  );
		}
	}

	// Wall step in Z — walls must cover the full slope range regardless of direction.
	// slope_height may be negative (downward slope), so use abs for the span.
	double total_wall_height = cave_height + std::abs( slope_height );
	int    num_z_steps = std::max( 1, (int)std::round( total_wall_height / step_x ) );
	double step_z      = total_wall_height / num_z_steps;
	double wall_min_z  = target.max_z + std::min( 0.0, slope_height );
	double wall_max_z  = wall_min_z + total_wall_height;
	result.step_z      = step_z;

	// Walls — grid over (along-tunnel coordinate, Z). For a Y-running tunnel
	// this is (Y, Z) with walls at fixed X (left/right); for an X-running
	// tunnel it's (X, Z) with walls at fixed Y (near/far).
	double along_min  = along_y ? target.min_y : target.min_x;
	double along_max  = along_y ? target.max_y : target.max_x;
	double along_step = along_y ? step_y       : step_x;
	double wall_lo    = along_y ? target.min_x : target.min_y;
	double wall_hi    = along_y ? target.max_x : target.max_y;

	for ( double u = along_min; u <= along_max + 0.01; u += along_step ) {
		for ( double z = wall_min_z; z <= wall_max_z + 0.01; z += step_z ) {
			double ru = round2( u );
			double rz = round2( z );

			double wall_noise = 0.0;
			if ( variance > 0.0 ) {
				if ( noise_type == NoiseType::Random ) {
					wall_noise = random_double() * variance;
				} else {
					wall_noise = std::abs( sample_noise( noise_type,
					    ( u + seed_wall_l ) * frequency, ( z + seed_wall_l ) * frequency ) ) * variance;
				}
			}
			// Snap only the noise, anchored at wall_lo/wall_hi, as above.
			result.left_wall_map[{ ru, rz }] = std::round( wall_lo + std::round( wall_noise / grid_step ) * grid_step );

			wall_noise = 0.0;
			if ( variance > 0.0 ) {
				if ( noise_type == NoiseType::Random ) {
					wall_noise = random_double() * variance;
				} else {
					wall_noise = std::abs( sample_noise( noise_type,
					    ( u + seed_wall_r ) * frequency, ( z + seed_wall_r ) * frequency ) ) * variance;
				}
			}
			result.right_wall_map[{ ru, rz }] = std::round( wall_hi - std::round( wall_noise / grid_step ) * grid_step );
		}
	}

	return result;
}
