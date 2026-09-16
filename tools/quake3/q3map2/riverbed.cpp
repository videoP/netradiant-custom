/* -------------------------------------------------------------------------------

   Copyright (C) 1999-2007 id Software, Inc. and contributors.
   For a list of contributors, see the accompanying CONTRIBUTORS file.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

   ----------------------------------------------------------------------------------

   -riverbed: precompute a shallow-water river domain from the compiled BSP.

   The engine's river solver needs a bed height under every cell of a grid laid
   over the river, and the only way to learn it from a BSP is to fire traces at
   the world.  Doing that at map load costs several traces per cell against the
   whole tree, and both the client and the server pay it independently - minutes
   on a map whose channel runs for 175,000 units.  None of it depends on
   anything but geometry, so it belongs in the compiler.

   Two things are cheaper here than they can ever be in the engine:

   - The corridor brushes are real geometry, not a yes/no overlap oracle, so the
     domain is an exact plane test instead of a recursive box probe that has to
     stop guessing at some arbitrary resolution.
   - The column queries are trivially parallel, and this tool already has a
     thread pool.

   The emitted .riverbed is authoritative: it carries the grid, which cells
   exist, and the captured floor and ceiling.  The engine validates the header
   against the entity keys and loads the rest verbatim, so it neither traces nor
   rasterizes anything.

   ------------------------------------------------------------------------------- */



/* dependencies */
#include "q3map2.h"
#include "riverbed.h"



/* -------------------------------------------------------------------------------

   file format

   ------------------------------------------------------------------------------- */

#define RIVERBED_MAGIC      ( ( 'R' << 24 ) | ( 'B' << 16 ) | ( 'E' << 8 ) | 'D' )
#define RIVERBED_VERSION    1

/* every field is four bytes, so the struct has no padding to disagree about */
struct riverbedHeader_t
{
	int magic;
	int version;
	int width;                      /* grid columns */
	int height;                     /* grid rows */
	int cellCount;                  /* cells the zone occupies, not width * height */
	float cellSize;
	float origin[ 2 ];              /* world position of grid cell (0,0)'s low corner */
	float mins[ 3 ];                /* the river brush bounds the grid was laid over */
	float maxs[ 3 ];
	float captureHeadroom;
	int zoneModel;                  /* inline model index of the corridor brushes */
	char river[ 64 ];               /* targetname, so the engine can match it up */
};

/* CONTENTS_SOLID is bit 0 in every id-derived game, and the BSP stores the
   game's raw content flags rather than q3map2's internal C_* set */
#define RIVERBED_CONTENTS_SOLID 1



/* -------------------------------------------------------------------------------

   solid geometry, indexed by column

   The capture walk asks the same question over and over: standing at (x,y),
   where is the floor below me, and where is the ceiling above.  Rather than
   walk the BSP tree per query, every solid brush is reduced once to the
   vertical span it occupies at a given column, which is exact for a convex
   brush and needs no trace epsilons.

   ------------------------------------------------------------------------------- */

struct riverbedSpan_t
{
	float bottom;
	float top;
};

std::vector<riverbedBrush_t> s_solidBrushes;

/* uniform XY buckets over the world, each holding the brushes that overlap it */
#define RIVERBED_BUCKET_SIZE    1024.0f

static std::vector<std::vector<int> > s_buckets;
static int s_bucketsX;
static int s_bucketsY;
static MinMax s_worldMinMax;

/*
   RiverbedBrushBounds()
   Exact bounds of a convex brush, from the vertices where each triple of its
   planes meets.

   The BSP keeps brushes as half-spaces and stores no bounds or windings, and
   deriving them from the axial sides alone is wrong for any brush that has
   none.  Triples are cheap at the six to eight sides a brush normally has.
 */

#define RIVERBED_MAX_BRUSH_SIDES    64

static bool RiverbedBrushBounds( int firstSide, int numSides, MinMax& minmax ){
	if ( numSides < 4 || numSides > RIVERBED_MAX_BRUSH_SIDES ) {
		return false;
	}

	minmax.clear();

	for ( int i = 0; i < numSides - 2; ++i )
	{
		const Plane3f& a = bspPlanes[ bspBrushSides[ firstSide + i ].planeNum ];
		for ( int j = i + 1; j < numSides - 1; ++j )
		{
			const Plane3f& b = bspPlanes[ bspBrushSides[ firstSide + j ].planeNum ];
			for ( int k = j + 1; k < numSides; ++k )
			{
				const Plane3f& c = bspPlanes[ bspBrushSides[ firstSide + k ].planeNum ];

				/* solve the three planes by Cramer's rule */
				const DoubleVector3 na( a.normal() ), nb( b.normal() ), nc( c.normal() );
				const DoubleVector3 bc = vector3_cross( nb, nc );
				const double det = vector3_dot( na, bc );
				if ( fabs( det ) < 1e-6 ) {
					continue;   /* parallel or nearly so, no single vertex */
				}

				const DoubleVector3 point =
					( bc * a.dist()
					  + vector3_cross( nc, na ) * b.dist()
					  + vector3_cross( na, nb ) * c.dist() ) / det;

				/* keep it only if it lies inside every other side */
				bool inside = true;
				for ( int s = 0; s < numSides; ++s )
				{
					const Plane3f& p = bspPlanes[ bspBrushSides[ firstSide + s ].planeNum ];
					if ( vector3_dot( p.normal(), point ) - p.dist() > 0.1 ) {
						inside = false;
						break;
					}
				}
				if ( inside ) {
					minmax.extend( Vector3( point ) );
				}
			}
		}
	}

	return minmax.valid();
}

/*
   RiverbedBuildSolidIndex()
   Collects the solid brushes of the world model and buckets them by XY.
 */

void RiverbedBuildSolidIndex(){
	s_solidBrushes.clear();
	s_buckets.clear();

	const bspModel_t& world = bspModels[ 0 ];

	for ( int b = 0; b < world.numBSPBrushes; ++b )
	{
		const bspBrush_t& brush = bspBrushes[ world.firstBSPBrush + b ];
		if ( brush.shaderNum < 0 || brush.shaderNum >= int( bspShaders.size() ) ) {
			continue;
		}
		if ( !( bspShaders[ brush.shaderNum ].contentFlags & RIVERBED_CONTENTS_SOLID ) ) {
			continue;
		}

		riverbedBrush_t solid;
		solid.firstSide = brush.firstSide;
		solid.numSides = brush.numSides;
		if ( !RiverbedBrushBounds( brush.firstSide, brush.numSides, solid.minmax ) ) {
			continue;
		}
		s_solidBrushes.push_back( solid );
	}

	/* world extent, from the brushes themselves rather than the tree */
	s_worldMinMax.clear();
	for ( const riverbedBrush_t& brush : s_solidBrushes )
		s_worldMinMax.extend( brush.minmax );

	if ( !s_worldMinMax.valid() ) {
		s_bucketsX = s_bucketsY = 0;
		return;
	}

	s_bucketsX = std::max( 1, int( ( s_worldMinMax.maxs[ 0 ] - s_worldMinMax.mins[ 0 ] ) / RIVERBED_BUCKET_SIZE ) + 1 );
	s_bucketsY = std::max( 1, int( ( s_worldMinMax.maxs[ 1 ] - s_worldMinMax.mins[ 1 ] ) / RIVERBED_BUCKET_SIZE ) + 1 );
	s_buckets.resize( size_t( s_bucketsX ) * s_bucketsY );

	for ( size_t i = 0; i < s_solidBrushes.size(); ++i )
	{
		const MinMax& mm = s_solidBrushes[ i ].minmax;
		const int x0 = std::max( 0, int( ( mm.mins[ 0 ] - s_worldMinMax.mins[ 0 ] ) / RIVERBED_BUCKET_SIZE ) );
		const int x1 = std::min( s_bucketsX - 1, int( ( mm.maxs[ 0 ] - s_worldMinMax.mins[ 0 ] ) / RIVERBED_BUCKET_SIZE ) );
		const int y0 = std::max( 0, int( ( mm.mins[ 1 ] - s_worldMinMax.mins[ 1 ] ) / RIVERBED_BUCKET_SIZE ) );
		const int y1 = std::min( s_bucketsY - 1, int( ( mm.maxs[ 1 ] - s_worldMinMax.mins[ 1 ] ) / RIVERBED_BUCKET_SIZE ) );

		for ( int y = y0; y <= y1; ++y )
			for ( int x = x0; x <= x1; ++x )
				s_buckets[ size_t( y ) * s_bucketsX + x ].push_back( int( i ) );
	}

	Sys_Printf( "%9zu solid brushes indexed into %d x %d buckets\n",
	            s_solidBrushes.size(), s_bucketsX, s_bucketsY );
}

/*
   RiverbedColumnSpans()
   Vertical extents of solid at one column, sorted bottom up.

   A vertical line through a convex brush is bounded by that brush's own planes:
   a side facing up caps the span, a side facing down floors it, and a vertical
   side either contains the column or rules the brush out entirely.
 */

static void RiverbedColumnSpans( float x, float y, std::vector<riverbedSpan_t>& spans ){
	spans.clear();

	if ( s_buckets.empty() ) {
		return;
	}

	const int bx = std::min( s_bucketsX - 1, std::max( 0, int( ( x - s_worldMinMax.mins[ 0 ] ) / RIVERBED_BUCKET_SIZE ) ) );
	const int by = std::min( s_bucketsY - 1, std::max( 0, int( ( y - s_worldMinMax.mins[ 1 ] ) / RIVERBED_BUCKET_SIZE ) ) );

	for ( int index : s_buckets[ size_t( by ) * s_bucketsX + bx ] )
	{
		const riverbedBrush_t& brush = s_solidBrushes[ index ];
		if ( x < brush.minmax.mins[ 0 ] || x > brush.minmax.maxs[ 0 ]
		  || y < brush.minmax.mins[ 1 ] || y > brush.minmax.maxs[ 1 ] ) {
			continue;
		}

		float bottom = -MAX_WORLD_COORD;
		float top = MAX_WORLD_COORD;
		bool misses = false;

		for ( int s = 0; s < brush.numSides; ++s )
		{
			const Plane3f& plane = bspPlanes[ bspBrushSides[ brush.firstSide + s ].planeNum ];
			/* inside is dot( n, p ) - d <= 0, so n.z * t <= d - n.x * x - n.y * y */
			const float rhs = plane.dist() - plane.normal()[ 0 ] * x - plane.normal()[ 1 ] * y;
			const float nz = plane.normal()[ 2 ];

			if ( fabs( nz ) < 1e-6f ) {
				if ( rhs < 0.0f ) {
					misses = true;   /* the column is outside a vertical side */
					break;
				}
				continue;
			}
			if ( nz > 0.0f ) {
				top = std::min( top, rhs / nz );
			}
			else{
				bottom = std::max( bottom, rhs / nz );
			}
		}

		if ( !misses && top > bottom ) {
			spans.push_back( riverbedSpan_t{ bottom, top } );
		}
	}

	std::sort( spans.begin(), spans.end(),
	           []( const riverbedSpan_t& a, const riverbedSpan_t& b ){ return a.bottom < b.bottom; } );
}



/* -------------------------------------------------------------------------------

   the river being built

   ------------------------------------------------------------------------------- */

riverbedJob_t s_job;

/*
   RiverbedCellCenter()
   Must agree exactly with the engine, which lays the grid out the same way and
   validates rather than recomputes.
 */

inline float RiverbedCellCenterX( int x ){
	return s_job.origin[ 0 ] + ( x + 0.5f ) * s_job.cellSize;
}

inline float RiverbedCellCenterY( int y ){
	return s_job.origin[ 1 ] + ( y + 0.5f ) * s_job.cellSize;
}

/*
   RiverbedBoxHitsModel()
   Exact overlap between an axis aligned box and any brush of an inline model.

   A convex brush and a box are disjoint exactly when some brush plane has the
   whole box on its outer side, which is the plane offset by the box's support
   in the normal direction.
 */

bool RiverbedBoxHitsModel( int modelNum, const Vector3& translation, const MinMax& box ){
	if ( modelNum <= 0 || modelNum >= int( bspModels.size() ) ) {
		return false;
	}

	const bspModel_t& model = bspModels[ modelNum ];

	for ( int b = 0; b < model.numBSPBrushes; ++b )
	{
		const bspBrush_t& brush = bspBrushes[ model.firstBSPBrush + b ];
		bool separated = false;

		for ( int s = 0; s < brush.numSides && !separated; ++s )
		{
			const Plane3f& plane = bspPlanes[ bspBrushSides[ brush.firstSide + s ].planeNum ];
			const Vector3 n = plane.normal();
			const float d = plane.dist() + vector3_dot( n, translation );

			/* The box lies entirely outside this side only if even its nearest
			   corner does, so this is the corner that MINIMISES dot( n, p ) -
			   the one opposite the normal.  Taking the furthest corner instead
			   finds a separating side for almost every brush and reports the
			   whole map as outside the corridor. */
			const Vector3 support(
				n[ 0 ] >= 0.0f ? box.mins[ 0 ] : box.maxs[ 0 ],
				n[ 1 ] >= 0.0f ? box.mins[ 1 ] : box.maxs[ 1 ],
				n[ 2 ] >= 0.0f ? box.mins[ 2 ] : box.maxs[ 2 ] );

			if ( vector3_dot( n, support ) - d > 0.0f ) {
				separated = true;
			}
		}

		if ( !separated ) {
			return true;
		}
	}

	return false;
}

/* one row per thread, since rows are independent and the row is a natural unit */
static void RiverbedRasterizeRow( int y ){
	const float half = 0.5f * s_job.cellSize;
	const float centerY = RiverbedCellCenterY( y );

	for ( int x = 0; x < s_job.width; ++x )
	{
		const float centerX = RiverbedCellCenterX( x );
		const MinMax box( Vector3( centerX - half, centerY - half, s_job.minmax.mins[ 2 ] ),
		                  Vector3( centerX + half, centerY + half, s_job.minmax.maxs[ 2 ] ) );

		/* the whole column, not a slice at some assumed water height: the reach
		   drops thousands of units and can run through tunnels, so a single
		   test height would cut the channel wherever the corridor and the
		   surface disagree */
		s_job.cellIndex[ size_t( y ) * s_job.width + x ] =
			RiverbedBoxHitsModel( s_job.zoneModel, s_job.zoneOrigin, box ) ? 0 : -1;
	}
}



/* -------------------------------------------------------------------------------

   capture

   A breadth first walk outward from the inlet, exactly as the engine does it.
   Tracing every column independently from the top of the corridor would take
   the first solid it met, so any roof, bridge or overhang would be recorded as
   riverbed and the channel beneath it would never exist.  Seeding at the source
   - which the mapper draws inside the channel - and stepping each neighbour
   from just above its predecessor's floor keeps every query inside the open
   space the water actually occupies.

   ------------------------------------------------------------------------------- */

/*
   RiverbedFindFloor()
   Highest solid surface below fromZ, without passing ceilingLimit.

   Climbing matters where the floor rises into the start point: the query begins
   as low as the caller allows and only lifts when it starts inside solid, which
   is what stops it stepping through a low roof into the space above.
 */

static bool RiverbedFindFloor( std::vector<riverbedSpan_t>& spans, float x, float y,
                               float fromZ, float ceilingLimit, float headroomStep, float *floorZ ){
	const float bottom = s_job.minmax.mins[ 2 ] - 1.0f;
	const float top = std::min( s_job.minmax.maxs[ 2 ] + 1.0f, ceilingLimit );
	float startZ = std::min( fromZ, top );

	RiverbedColumnSpans( x, y, spans );

	for ( int attempt = 0; attempt < 12; ++attempt )
	{
		bool startSolid = false;
		float best = bottom - 1.0f;

		for ( const riverbedSpan_t& span : spans )
		{
			if ( startZ > span.bottom && startZ < span.top ) {
				startSolid = true;
				break;
			}
			if ( span.top <= startZ && span.top >= bottom && span.top > best ) {
				best = span.top;
			}
		}

		if ( !startSolid && best >= bottom && best <= s_job.minmax.maxs[ 2 ] + 1.0f ) {
			*floorZ = best;
			return true;
		}
		if ( startZ >= top || headroomStep <= 0.0f ) {
			break;
		}
		startZ = std::min( startZ + headroomStep, top );
	}

	return false;
}

/*
   RiverbedFindCeiling()
   Solid above a captured floor, or the top of the corridor where it is open.
   The walk uses this to keep a neighbour search inside the space it is already
   in, so a wall in a tunnel cannot become a staircase onto the roof.
 */

static float RiverbedFindCeiling( std::vector<riverbedSpan_t>& spans, float x, float y, float floorZ ){
	const float top = s_job.minmax.maxs[ 2 ] + 1.0f;
	float best = top;

	RiverbedColumnSpans( x, y, spans );

	for ( const riverbedSpan_t& span : spans )
	{
		if ( span.bottom >= floorZ + 2.0f && span.bottom < best ) {
			best = span.bottom;
		}
	}

	return best;
}

/* a neighbour the frontier wants to reach, evaluated in parallel and claimed
   afterwards in a fixed order so the result does not depend on thread timing */
struct riverbedCandidate_t
{
	int gridIndex;
	float x;
	float y;
	float fromZ;
	float ceilingLimit;
	float floorZ;
	float ceilingZ;
	bool found;
};

static std::vector<riverbedCandidate_t> s_candidates;

static void RiverbedEvaluateCandidate( int i ){
	static thread_local std::vector<riverbedSpan_t> spans;

	riverbedCandidate_t& candidate = s_candidates[ i ];
	candidate.found = RiverbedFindFloor( spans, candidate.x, candidate.y,
	                                     candidate.fromZ, candidate.ceilingLimit,
	                                     s_job.captureHeadroom, &candidate.floorZ );
	if ( candidate.found ) {
		candidate.ceilingZ = RiverbedFindCeiling( spans, candidate.x, candidate.y, candidate.floorZ );
	}
}

/*
   RiverbedCapture()
   Seeds from the inlets and floods the channel.

   Level synchronous rather than a plain queue: every cell on the frontier is
   evaluated at once, then claimed serially in frontier order.  That is the same
   answer a single threaded first in first out walk gives - the first parent to
   reach a cell still wins - while the expensive part, the column queries, runs
   across every core.
 */

static int RiverbedCapture(){
	std::vector<riverbedSpan_t> spans;
	std::vector<int> frontier;
	int captured = 0;

	for ( riverbedCell_t& cell : s_job.cells )
	{
		cell.bedHeight = s_job.minmax.mins[ 2 ];
		cell.ceilingHeight = s_job.minmax.maxs[ 2 ] + 1.0f;
		cell.flags = 0;
	}

	/* seeds, in entity order then row major, matching the engine */
	for ( size_t s = 0; s < s_job.sources.size(); ++s )
	{
		const MinMax& source = s_job.sources[ s ].minmax;
		const float half = 0.5f * s_job.cellSize;

		for ( int y = 0; y < s_job.height; ++y )
		{
			const float centerY = RiverbedCellCenterY( y );
			if ( centerY + half < source.mins[ 1 ] || centerY - half > source.maxs[ 1 ] ) {
				continue;
			}
			for ( int x = 0; x < s_job.width; ++x )
			{
				const int compact = s_job.cellIndex[ size_t( y ) * s_job.width + x ];
				if ( compact < 0 || ( s_job.cells[ compact ].flags & RIVERBED_CELL_ACTIVE ) ) {
					continue;
				}

				const float centerX = RiverbedCellCenterX( x );
				if ( centerX + half < source.mins[ 0 ] || centerX - half > source.maxs[ 0 ] ) {
					continue;
				}

				/* exact against the inlet brush, not its bounding box: a thin
				   brush crossing the channel diagonally has a bounding box
				   several times its own area */
				const MinMax box( Vector3( centerX - half, centerY - half, source.mins[ 2 ] ),
				                  Vector3( centerX + half, centerY + half, source.maxs[ 2 ] ) );
				if ( s_job.sources[ s ].model > 0
				  && !RiverbedBoxHitsModel( s_job.sources[ s ].model,
				                            s_job.sources[ s ].origin, box ) ) {
					continue;
				}

				float floorZ;
				if ( !RiverbedFindFloor( spans, centerX, centerY, source.mins[ 2 ] + 1.0f,
				                         s_job.minmax.maxs[ 2 ] + 1.0f, s_job.captureHeadroom, &floorZ ) ) {
					continue;
				}

				s_job.cells[ compact ].bedHeight = floorZ;
				s_job.cells[ compact ].ceilingHeight = RiverbedFindCeiling( spans, centerX, centerY, floorZ );
				s_job.cells[ compact ].flags |= RIVERBED_CELL_ACTIVE;
				frontier.push_back( size_t( y ) * s_job.width + x );
				++captured;
			}
		}
	}

	if ( frontier.empty() ) {
		Sys_Warning( "no inlet cell could be seeded; check that a misc_sailing_river_source targets this river and sits over solid ground\n" );
		return 0;
	}

	static const int offsetX[ 4 ] = { 1, -1, 0, 0 };
	static const int offsetY[ 4 ] = { 0, 0, 1, -1 };

	while ( !frontier.empty() )
	{
		s_candidates.clear();

		for ( int gridIndex : frontier )
		{
			const int cellX = gridIndex % s_job.width;
			const int cellY = gridIndex / s_job.width;
			const riverbedCell_t& cell = s_job.cells[ s_job.cellIndex[ gridIndex ] ];

			for ( int side = 0; side < 4; ++side )
			{
				const int nx = cellX + offsetX[ side ];
				const int ny = cellY + offsetY[ side ];
				if ( nx < 0 || nx >= s_job.width || ny < 0 || ny >= s_job.height ) {
					continue;
				}

				const int neighbour = size_t( ny ) * s_job.width + nx;
				const int compact = s_job.cellIndex[ neighbour ];
				if ( compact < 0 || ( s_job.cells[ compact ].flags & RIVERBED_CELL_ACTIVE ) ) {
					continue;
				}

				riverbedCandidate_t candidate;
				candidate.gridIndex = neighbour;
				candidate.x = RiverbedCellCenterX( nx );
				candidate.y = RiverbedCellCenterY( ny );
				candidate.fromZ = cell.bedHeight + 2.0f;
				candidate.ceilingLimit = cell.ceilingHeight;
				candidate.floorZ = 0.0f;
				candidate.ceilingZ = 0.0f;
				candidate.found = false;
				s_candidates.push_back( candidate );
			}
		}

		if ( s_candidates.empty() ) {
			break;
		}

		RunThreadsOnIndividual( int( s_candidates.size() ), false, RiverbedEvaluateCandidate );

		frontier.clear();
		for ( const riverbedCandidate_t& candidate : s_candidates )
		{
			const int compact = s_job.cellIndex[ candidate.gridIndex ];
			/* another candidate this round may already have taken it; first in
			   frontier order wins, which is what the serial walk does */
			if ( !candidate.found || ( s_job.cells[ compact ].flags & RIVERBED_CELL_ACTIVE ) ) {
				continue;
			}
			s_job.cells[ compact ].bedHeight = candidate.floorZ;
			s_job.cells[ compact ].ceilingHeight = candidate.ceilingZ;
			s_job.cells[ compact ].flags |= RIVERBED_CELL_ACTIVE;
			frontier.push_back( candidate.gridIndex );
			++captured;
		}
	}

	return captured;
}



/* -------------------------------------------------------------------------------

   entity reading and output

   ------------------------------------------------------------------------------- */

/* "*37" -> 37, and 0 for anything else */
static int RiverbedInlineModel( const char *model ){
	return ( model != NULL && model[ 0 ] == '*' ) ? atoi( model + 1 ) : 0;
}

static bool RiverbedModelBounds( int modelNum, const Vector3& translation, MinMax& minmax ){
	if ( modelNum <= 0 || modelNum >= int( bspModels.size() ) ) {
		return false;
	}
	minmax = bspModels[ modelNum ].minmax;
	minmax.mins += translation;
	minmax.maxs += translation;
	return minmax.valid();
}

/*
   RiverbedReadBoundaries()
   Collects this river's inlets and outlets, in entity order.

   The order is load bearing rather than cosmetic: the engine's settings hash
   walks its own tables in registration order, which is the order the entities
   appear in the bsp, so gathering them any other way produces a hash the engine
   will not accept.  Every key is read the way the engine reads it, defaults
   included, for the same reason.
 */

static void RiverbedReadBoundaries(){
	for ( const entity_t& ent : entities )
	{
		const bool isSource = ent.classname_is( "misc_sailing_river_source" );
		const bool isSink = ent.classname_is( "misc_sailing_river_sink" );
		if ( !isSource && !isSink ) {
			continue;
		}
		if ( !striEqual( ent.valueForKey( "target" ), s_job.name.c_str() ) ) {
			continue;
		}

		const int model = RiverbedInlineModel( ent.valueForKey( "model" ) );
		const Vector3 origin = ent.vectorForKey( "origin" );
		MinMax bounds;
		if ( !RiverbedModelBounds( model, origin, bounds ) ) {
			continue;
		}

		/* "angle" is the flow direction here, not a rotation of the brush */
		const float yaw = ent.floatForKey( "angle" ) * float( c_pi ) / 180.0f;
		const float dirX = cosf( yaw );
		const float dirY = sinf( yaw );

		if ( isSource ) {
			riverbedSourceEnt_t source;
			source.minmax = bounds;
			source.model = model;
			source.origin = origin;
			source.direction[ 0 ] = dirX;
			source.direction[ 1 ] = dirY;

			const char *type = ent.valueForKey( "type" );
			if ( striEqual( type, "velocity" ) ) {
				source.type = 1;
			}
			else if ( strEmptyOrNull( type ) || striEqual( type, "discharge" ) ) {
				source.type = 0;
			}
			else{
				Sys_Warning( "river source has unknown type '%s', skipping\n", type );
				continue;
			}

			source.discharge = ent.floatForKey( "discharge" ) * 12.0f * 12.0f * 12.0f;
			source.speed = ent.floatForKey( "speed" );
			/* defaults to the brush top, as the engine does */
			source.surfaceHeight = bounds.maxs[ 2 ];
			ent.read_keyvalue( source.surfaceHeight, "surfaceHeight" );
			s_job.sources.push_back( source );
		}
		else{
			riverbedSinkEnt_t sink;
			sink.minmax = bounds;
			sink.model = model;
			sink.origin = origin;
			sink.direction[ 0 ] = dirX;
			sink.direction[ 1 ] = dirY;

			const char *type = ent.valueForKey( "type" );
			if ( striEqual( type, "fixed" ) ) {
				sink.type = 1;
			}
			else if ( striEqual( type, "normal" ) ) {
				sink.type = 2;
			}
			else if ( striEqual( type, "overfall" ) ) {
				sink.type = 3;
			}
			else if ( striEqual( type, "drain" ) ) {
				sink.type = 4;
			}
			else if ( strEmptyOrNull( type ) || striEqual( type, "open" ) ) {
				sink.type = 0;
			}
			else{
				Sys_Warning( "river sink has unknown type '%s', skipping\n", type );
				continue;
			}

			/* a drain's invert defaults to the brush FLOOR; everything else
			   defaults to its top */
			sink.surfaceHeight = sink.type == 4 ? bounds.mins[ 2 ] : bounds.maxs[ 2 ];
			ent.read_keyvalue( sink.surfaceHeight, "surfaceHeight" );
			sink.drainRate = 0.05f;
			ent.read_keyvalue( sink.drainRate, "drainRate" );
			s_job.sinks.push_back( sink );
		}
	}
}

/*
   RiverbedSetupGrid()
   Grid derivation, which has to match the engine exactly or every cell is
   offset.  The engine stores no copy of this - it recomputes it from the same
   entity keys and refuses the file if the two disagree.
 */

static bool RiverbedSetupGrid(){
	s_job.width = int( ceil( ( s_job.minmax.maxs[ 0 ] - s_job.minmax.mins[ 0 ] ) / s_job.cellSize ) );
	s_job.height = int( ceil( ( s_job.minmax.maxs[ 1 ] - s_job.minmax.mins[ 1 ] ) / s_job.cellSize ) );

	if ( s_job.width <= 0 || s_job.height <= 0 ) {
		Sys_Warning( "river '%s' has degenerate bounds\n", s_job.name.c_str() );
		return false;
	}

	/* centred so every cell centre stays inside the brush bounds */
	s_job.origin[ 0 ] = 0.5f * ( s_job.minmax.mins[ 0 ] + s_job.minmax.maxs[ 0 ]
	                             - s_job.width * s_job.cellSize );
	s_job.origin[ 1 ] = 0.5f * ( s_job.minmax.mins[ 1 ] + s_job.minmax.maxs[ 1 ]
	                             - s_job.height * s_job.cellSize );
	return true;
}

void RiverbedWrite( const char *bspPath ){
	riverbedHeader_t header;
	memset( &header, 0, sizeof( header ) );
	header.magic = RIVERBED_MAGIC;
	header.version = RIVERBED_VERSION;
	header.width = s_job.width;
	header.height = s_job.height;
	header.cellCount = int( s_job.cells.size() );
	header.cellSize = s_job.cellSize;
	header.origin[ 0 ] = s_job.origin[ 0 ];
	header.origin[ 1 ] = s_job.origin[ 1 ];
	for ( int i = 0; i < 3; ++i )
	{
		header.mins[ i ] = s_job.minmax.mins[ i ];
		header.maxs[ i ] = s_job.minmax.maxs[ i ];
	}
	header.captureHeadroom = s_job.captureHeadroom;
	header.zoneModel = s_job.zoneModel;
	strncpy( header.river, s_job.name.c_str(), sizeof( header.river ) - 1 );

	/* occupancy as one bit per grid cell, so the engine can rebuild the lookup
	   without the file carrying a four byte index for terrain it will never use */
	const size_t bitmapBytes = ( size_t( s_job.width ) * s_job.height + 7 ) / 8;
	std::vector<unsigned char> bitmap( bitmapBytes, 0 );
	for ( size_t i = 0; i < size_t( s_job.width ) * s_job.height; ++i )
	{
		if ( s_job.cellIndex[ i ] >= 0 ) {
			bitmap[ i >> 3 ] |= (unsigned char)( 1u << ( i & 7 ) );
		}
	}

	const auto filename = StringStream( PathExtensionless( bspPath ), "_", s_job.name.c_str(),
	                                    "_c", int( s_job.cellSize + 0.5f ), ".riverbed" );
	Sys_Printf( "Writing %s\n", filename.c_str() );

	FILE *file = SafeOpenWrite( filename, "wb" );
	SafeWrite( file, &header, sizeof( header ) );
	SafeWrite( file, bitmap.data(), int( bitmap.size() ) );
	SafeWrite( file, s_job.cells.data(), int( s_job.cells.size() * sizeof( riverbedCell_t ) ) );
	fclose( file );

	Sys_Printf( "%9d grid cells\n", s_job.width * s_job.height );
	Sys_Printf( "%9d cells in the corridor\n", int( s_job.cells.size() ) );
	Sys_Printf( "%9zu bytes\n", sizeof( header ) + bitmap.size()
	            + s_job.cells.size() * sizeof( riverbedCell_t ) );
}

/*
   RiverbedRiver()
   Builds one river, start to finish.
 */

bool RiverbedPrepare( const entity_t& river, float cellSizeOverride ){
	s_job = riverbedJob_t();

	s_job.name = river.valueForKey( "targetname" );
	if ( s_job.name.empty() ) {
		Sys_Warning( "misc_sailing_river has no targetname, skipping\n" );
		return false;
	}

	s_job.zoneModel = RiverbedInlineModel( river.valueForKey( "model" ) );
	s_job.zoneOrigin = river.vectorForKey( "origin" );
	if ( !RiverbedModelBounds( s_job.zoneModel, s_job.zoneOrigin, s_job.minmax ) ) {
		Sys_Warning( "river '%s' has no usable brush model, skipping\n", s_job.name.c_str() );
		return false;
	}

	s_job.cellSize = cellSizeOverride > 0.0f ? cellSizeOverride : river.floatForKey( "cellSize" );
	if ( s_job.cellSize <= 0.0f ) {
		s_job.cellSize = 16.0f;
	}
	s_job.cellSize = std::min( 256.0f, std::max( 4.0f, s_job.cellSize ) );

	s_job.captureHeadroom = river.floatForKey( "captureHeadroom" );
	if ( s_job.captureHeadroom <= 0.0f ) {
		s_job.captureHeadroom = 8.0f;
	}
	s_job.captureHeadroom = std::min( 256.0f, s_job.captureHeadroom );

	s_job.friction = 0.12f;         /* the engine's default for the key */
	river.read_keyvalue( s_job.friction, "friction" );
	s_job.friction = std::min( 2.0f, std::max( 0.001f, s_job.friction ) );

	if ( !RiverbedSetupGrid() ) {
		return false;
	}

	Sys_Printf( "--- Riverbed (%s) ---\n", s_job.name.c_str() );
	Sys_Printf( "%9.0f cell size\n", s_job.cellSize );
	Sys_Printf( "%9d x %d grid\n", s_job.width, s_job.height );

	RiverbedReadBoundaries();

	if ( s_job.sources.empty() ) {
		Sys_Warning( "river '%s' has no source, skipping\n", s_job.name.c_str() );
		return false;
	}

	/* rasterize the corridor */
	s_job.cellIndex.assign( size_t( s_job.width ) * s_job.height, -1 );
	RunThreadsOnIndividual( s_job.height, true, RiverbedRasterizeRow );

	int occupied = 0;
	for ( size_t i = 0; i < s_job.cellIndex.size(); ++i )
	{
		/* numbered row major so the engine's compact order matches the grid */
		s_job.cellIndex[ i ] = s_job.cellIndex[ i ] < 0 ? -1 : occupied++;
	}

	if ( occupied == 0 ) {
		Sys_Warning( "river '%s' corridor covers no cell, skipping\n", s_job.name.c_str() );
		return false;
	}
	s_job.cells.resize( occupied );

	const int captured = RiverbedCapture();

	Sys_Printf( "%9d cells captured of %d in the corridor\n", captured, occupied );
	return captured != 0;
}

/*
   RiverbedMain()
   -riverbed, run against a compiled bsp.
 */

int RiverbedMain( Args& args ){
	float cellSizeOverride = 0.0f;

	/* arg checking */
	if ( args.empty() ) {
		Sys_Printf( "Usage: q3map2 -riverbed [-v] [-cellsize <n>] <mapname>\n" );
		return 0;
	}

	{
		while ( args.takeArg( "-cellsize" ) ) {
			cellSizeOverride = atof( args.takeNext() );
			Sys_Printf( "Overriding cell size with %.0f\n", cellSizeOverride );
		}
	}

	/* do some path mangling */
	strcpy( source, ExpandArg( args.takeBack() ) );
	path_set_extension( source, ".bsp" );

	/* load the bsp */
	Sys_Printf( "Loading %s\n", source );
	LoadBSPFile( source );
	ParseEntities();

	RiverbedBuildSolidIndex();
	if ( s_solidBrushes.empty() ) {
		Sys_Warning( "no solid brushes in this bsp; nothing to stand a river on\n" );
		return 0;
	}

	int rivers = 0;
	for ( const entity_t& ent : entities )
	{
		if ( ent.classname_is( "misc_sailing_river" ) ) {
			if ( RiverbedPrepare( ent, cellSizeOverride ) ) {
				RiverbedWrite( source );
			}
			++rivers;
		}
	}

	if ( rivers == 0 ) {
		Sys_Warning( "no misc_sailing_river in this bsp\n" );
	}

	/* return to sender */
	return 0;
}
