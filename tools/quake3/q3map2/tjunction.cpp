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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

   ----------------------------------------------------------------------------------

   This code has been altered significantly from its original form, to support
   several games based on the Quake III Arena engine, in the form of "Q3Map2."

   ------------------------------------------------------------------------------- */



/* dependencies */
#include "q3map2.h"
#include "tjunction.h"
#include "timer.h"

#include <ranges>
#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>




struct edgePoint_t
{
	float intercept;
	Vector3 xyz;
};

struct edgeLine_t
{
	Vector3 normal1;
	float dist1;

	Vector3 normal2;
	float dist2;

	Vector3 origin;
	Vector3 dir;

	std::list<edgePoint_t> points;
};

struct originalEdge_t
{
	float length;
	bspDrawVert_t *dv1;
	bspDrawVert_t *dv2;
};

namespace
{
std::vector<originalEdge_t> originalEdges;

std::vector<edgeLine_t> edgeLines;

int c_degenerateEdges;
int c_addedVerts;
int c_totalVerts;

int c_natural, c_rotate, c_cant;
int c_broken;
size_t c_lineTests;
size_t c_gridMissed, c_gridOther;
}


/*
   -tjgrid: finding the edge line an edge belongs to
   ------------------------------------------------

   AddEdge() below compares an edge with every edge line found so far. Both the
   number of edges and the number of lines grow with the map, so the work grows
   with the square of it; on a few hundred thousand brushes this is the most
   expensive thing in the compile by a wide margin.

   What the comparison actually asks is whether both ends of the edge lie within
   POINT_ON_LINE_EPSILON of the line, so only lines running close to the edge can
   ever match. Two indexes narrow it down, and whatever they turn up is then put
   back into creation order and tested exactly as before - so an edge still joins
   the first line, in creation order, that it lies on.

   Axial lines are indexed by their two perpendicular coordinates and nothing
   else. A line along x is fixed by its y and z, so every edge on it hashes to
   the same place no matter where along the map it is. This matters: collinear
   geometry is regularly split across the map with gaps in between - two columns
   sharing a floor edge, say - and an index with any notion of extent would file
   those as separate lines. That is not harmless. A longer edge crossing both
   would then find only one of them and the T-junctions against the other would
   go unwelded, which is a crack in the map.

   Everything else is indexed by the cells its edges pass through. A run of
   collinear angled geometry is in practice contiguous, so the cells overlap and
   the line is found.
 */

namespace
{
/* powers of two; the key need not be recoverable from the bucket, as a
   collision only costs an extra exact test */
constexpr size_t TJGRID_BUCKETS = 1 << 20;
constexpr float TJGRID_CELL = 64.f;         /* cells for the segment index */
constexpr float TJGRID_AXIAL_CELL = 4.f;    /* finer: an axial key has only two coordinates in it */
constexpr int TJGRID_MAX_CELLS = 4096;      /* bound on the cells one edge may cover */
constexpr float TJGRID_MARGIN = 0.5f;       /* comfortably over POINT_ON_LINE_EPSILON, applied when filing */

std::vector<std::vector<uint32_t>> tjSegBuckets;
std::vector<std::vector<uint32_t>> tjAxialBuckets;
std::vector<size_t> tjUsedSeg, tjUsedAxial;     /* so an entity clears only what it filled */
std::vector<uint32_t> tjSeen;                   /* edge line -> the query that last gathered it */
std::vector<uint32_t> tjCandidates;
uint32_t tjQueryId;
MinMax tjWorld;     /* the lookup follows the edge's line only as far as there is map to follow it through */

inline size_t tjHash( int x, int y, int z ){
	uint32_t h = uint32_t( x ) * 0x8da6b343u ^ uint32_t( y ) * 0xd8163841u ^ uint32_t( z ) * 0xcb1ab31fu;
	h ^= h >> 15;
	return h & ( TJGRID_BUCKETS - 1 );
}

inline int tjCellOf( float v ){
	return int( std::floor( v / TJGRID_CELL ) );
}

inline int tjAxialCellOf( float v ){
	return int( std::floor( v / TJGRID_AXIAL_CELL ) );
}

/* which axis a direction runs along, or -1. Normalising an axial difference
   gives exactly +-1, so this is an exact test - note it is stricter than the
   one AddEdge() uses to sort edges into the two passes, which also lets through
   directions such as ( 2/3, 2/3, -1/3 ) whose components merely sum to one */
inline int tjAxialAxis( const Vector3& dir ){
	for ( int i = 0; i < 3; ++i )
		if ( dir[i] == 1.f || dir[i] == -1.f ) {
			return i;
		}
	return -1;
}

void tjAdd( std::vector<std::vector<uint32_t>>& buckets, std::vector<size_t>& used, size_t b, uint32_t line ){
	std::vector<uint32_t>& bucket = buckets[ b ];
	if ( bucket.empty() ) {
		used.push_back( b );
	}
	if ( std::find( bucket.cbegin(), bucket.cend(), line ) == bucket.cend() ) {
		bucket.push_back( line );
	}
}

/*
   The cells a segment passes through (Amanatides & Woo). Walking the cells
   rather than the bounding box matters: a long diagonal edge passes through a
   few dozen cells but its bounding box spans thousands.
 */
template<typename Visitor>
void tjSegmentCells( const Vector3& v1, const Vector3& v2, Visitor visit ){
	const Vector3 d = v2 - v1;
	int cell[3], last[3], step[3];
	double tMax[3], tDelta[3];

	for ( int i = 0; i < 3; ++i )
	{
		cell[i] = tjCellOf( v1[i] );
		last[i] = tjCellOf( v2[i] );
		if ( d[i] > 0 ) {
			step[i] = 1;
			tDelta[i] = TJGRID_CELL / d[i];
			tMax[i] = ( ( cell[i] + 1 ) * TJGRID_CELL - v1[i] ) / d[i];
		}
		else if ( d[i] < 0 ) {
			step[i] = -1;
			tDelta[i] = TJGRID_CELL / -d[i];
			tMax[i] = ( cell[i] * TJGRID_CELL - v1[i] ) / d[i];
		}
		else{
			step[i] = 0;
			tDelta[i] = std::numeric_limits<double>::max();
			tMax[i] = std::numeric_limits<double>::max();
		}
	}

	for ( int guard = 0; guard < TJGRID_MAX_CELLS; ++guard )
	{
		visit( cell[0], cell[1], cell[2] );
		if ( cell[0] == last[0] && cell[1] == last[1] && cell[2] == last[2] ) {
			break;
		}
		const int i = ( tMax[0] < tMax[1] )? ( ( tMax[0] < tMax[2] )? 0 : 2 )
		                                   : ( ( tMax[1] < tMax[2] )? 1 : 2 );
		if ( step[i] == 0 ) {
			break;  /* the far cell cannot be reached: give up rather than spin */
		}
		cell[i] += step[i];
		tMax[i] += tDelta[i];
	}
}

/*
   File a line under the edge just put on it.

   Two points within the match epsilon of each other still land in different
   cells when they fall either side of a boundary, so the edge is also filed
   under the cells it would occupy were it shifted by the epsilon - all eight
   corners of that, since the boundary it straddles may be on any axis. This is
   the same margin the lookup would otherwise have to apply to all 27 neighbours
   of every cell it visits, and the lookup visits far more cells than this does:
   it is much cheaper here, and it costs nearly nothing in space, because away
   from a boundary all eight shifts give the cell the edge is in already.
 */
void tjRegister( uint32_t line, const Vector3& v1, const Vector3& v2 ){
	const edgeLine_t& e = edgeLines[ line ];
	const int axis = tjAxialAxis( e.dir );
	if ( axis >= 0 ) {
		const int b = ( axis + 1 ) % 3, c = ( axis + 2 ) % 3;
		for ( int db = -1; db <= 1; ++db )
			for ( int dc = -1; dc <= 1; ++dc )
				tjAdd( tjAxialBuckets, tjUsedAxial,
				       tjHash( axis, tjAxialCellOf( e.origin[b] ) + db, tjAxialCellOf( e.origin[c] ) + dc ), line );
	}
	else{
		for ( int i = 0; i < 8; ++i )
		{
			const Vector3 shift( ( i & 1 )? TJGRID_MARGIN : -TJGRID_MARGIN,
			                     ( i & 2 )? TJGRID_MARGIN : -TJGRID_MARGIN,
			                     ( i & 4 )? TJGRID_MARGIN : -TJGRID_MARGIN );
			tjSegmentCells( v1 + shift, v2 + shift, [line]( int x, int y, int z ){
				tjAdd( tjSegBuckets, tjUsedSeg, tjHash( x, y, z ), line );
			} );
		}
	}
}

/* the lines that could hold this edge, in creation order */
void tjGather( const Vector3& v1, const Vector3& v2, const Vector3& dir ){
	tjCandidates.clear();
	tjSeen.resize( edgeLines.size(), UINT32_MAX );
	const uint32_t id = ++tjQueryId;

	const auto take = [id]( uint32_t line ){
		if ( tjSeen[ line ] != id ) {
			tjSeen[ line ] = id;
			tjCandidates.push_back( line );
		}
	};

	/* every axial line running near this end of the edge, from anywhere in the
	   map; a cell of margin each way covers the match epsilon */
	for ( int axis = 0; axis < 3; ++axis )
	{
		const int qb = tjAxialCellOf( v1[ ( axis + 1 ) % 3 ] );
		const int qc = tjAxialCellOf( v1[ ( axis + 2 ) % 3 ] );
		for ( const uint32_t line : tjAxialBuckets[ tjHash( axis, qb, qc ) ] )
			take( line );
	}

	/* and the rest, from the cells along the edge - carried on past both ends to
	   the edges of the map, because a line is regularly shared by two runs of
	   geometry a long way apart, with nothing filed in the cells between. A
	   surface cut up by the BSP tree does this constantly: the pieces meet the
	   cutting plane along one line however far apart they end up */
	Vector3 from = v1, to = v2;
	{
		const float length = float( vector3_length( v2 - v1 ) );
		float tMin = std::numeric_limits<float>::lowest(), tMax = std::numeric_limits<float>::max();
		for ( int i = 0; i < 3; ++i )
		{
			if ( fabs( dir[i] ) < 1e-6f ) {
				continue;   /* parallel to this pair of slabs */
			}
			const float t1 = ( tjWorld.mins[i] - v1[i] ) / dir[i];
			const float t2 = ( tjWorld.maxs[i] - v1[i] ) / dir[i];
			tMin = std::max( tMin, std::min( t1, t2 ) );
			tMax = std::min( tMax, std::max( t1, t2 ) );
		}
		/* never shorter than the edge itself */
		from = v1 + dir * std::min( tMin, 0.f );
		to = v1 + dir * std::max( tMax, length );
	}

	tjSegmentCells( from, to, [&take]( int x, int y, int z ){
		for ( const uint32_t line : tjSegBuckets[ tjHash( x, y, z ) ] )
			take( line );
	} );

	/* the stock scan takes the first line in creation order that matches */
	std::sort( tjCandidates.begin(), tjCandidates.end() );
}

void tjReset( const MinMax& world ){
	tjWorld = world;
	tjSegBuckets.resize( TJGRID_BUCKETS );
	tjAxialBuckets.resize( TJGRID_BUCKETS );
	for ( const size_t b : tjUsedSeg )
		tjSegBuckets[ b ].clear();
	for ( const size_t b : tjUsedAxial )
		tjAxialBuckets[ b ].clear();
	tjUsedSeg.clear();
	tjUsedAxial.clear();
	tjSeen.clear();
	tjQueryId = 0;
}
}

// these should be whatever epsilon we actually expect,
// plus SNAP_INT_TO_FLOAT
#define LINE_POSITION_EPSILON   0.25f
#define POINT_ON_LINE_EPSILON   0.25

/*
   ====================
   InsertPointOnEdge
   ====================
 */
static void InsertPointOnEdge( const Vector3 &v, edgeLine_t& e ) {
	const edgePoint_t p = { .intercept = static_cast<float>( vector3_dot( v - e.origin, e.dir ) ), .xyz = v };

	for ( auto it = e.points.cbegin(); it != e.points.cend(); ++it ) {
		if ( float_equal_epsilon( p.intercept, it->intercept, LINE_POSITION_EPSILON ) ) {
			return;     // the point is already set
		}

		if ( p.intercept < it->intercept ) {
			// insert here
			e.points.insert( it, p );
			return;
		}
	}

	// add at the end if empty list or greatest new point
	e.points.push_back( p );
}


/*
   ====================
   AddEdge
   ====================
 */
static int AddEdge( bspDrawVert_t& dv1, bspDrawVert_t& dv2, bool createNonAxial ) {
	const Vector3& v1 = dv1.xyz;
	const Vector3& v2 = dv2.xyz;

	Vector3 dir = v2 - v1;
	const float d = VectorNormalize( dir );
	if ( d < 0.1 ) {
		// if we added a 0 length vector, it would make degenerate planes
		c_degenerateEdges++;
		return -1;
	}

	if ( !createNonAxial ) {
		if ( std::fabs( dir[0] + dir[1] + dir[2] ) != 1 ) {
			originalEdges.push_back( originalEdge_t{ .length = d, .dv1 = &dv1, .dv2 = &dv2 } );
			return -1;
		}
	}

	const auto onLine = [&v1, &v2]( const edgeLine_t& e ){
		++c_lineTests;
		return float_equal_epsilon( vector3_dot( v1, e.normal1 ), e.dist1, POINT_ON_LINE_EPSILON )
		    && float_equal_epsilon( vector3_dot( v1, e.normal2 ), e.dist2, POINT_ON_LINE_EPSILON )
		    && float_equal_epsilon( vector3_dot( v2, e.normal1 ), e.dist1, POINT_ON_LINE_EPSILON )
		    && float_equal_epsilon( vector3_dot( v2, e.normal2 ), e.dist2, POINT_ON_LINE_EPSILON );
	};

	if ( tjGrid ) {
		tjGather( v1, v2, dir );
		int found = -1;
		for ( const uint32_t i : tjCandidates )
			if ( onLine( edgeLines[ i ] ) ) {
				found = int( i );
				break;
			}

		/* -tjverify: what the scan this replaces would have picked */
		if ( tjVerify ) {
			int scanned = -1;
			for ( size_t i = 0; i < edgeLines.size(); ++i )
				if ( onLine( edgeLines[ i ] ) ) {
					scanned = int( i );
					break;
				}
			if ( scanned != found ) {
				if ( scanned >= 0 && found < 0 ) {
					++c_gridMissed;     /* a line the index did not offer at all */
				}
				else{
					++c_gridOther;      /* a different, but still valid, line */
				}
			}
		}

		if ( found >= 0 ) {
			edgeLine_t& e = edgeLines[ found ];
			// this is the edge
			InsertPointOnEdge( v1, e );
			InsertPointOnEdge( v2, e );
			tjRegister( uint32_t( found ), v1, v2 );
			return found;
		}
	}
	else
	{
		for ( edgeLine_t& e : edgeLines ) {
			if ( !onLine( e ) ) {
				continue;
			}

			// this is the edge
			InsertPointOnEdge( v1, e );
			InsertPointOnEdge( v2, e );
			return &e - &edgeLines[0];
		}
	}

	// create a new edge
	edgeLine_t& e = edgeLines.emplace_back();

	e.origin = v1;
	e.dir = dir;

	MakeNormalVectors( e.dir, e.normal1, e.normal2 );
	e.dist1 = vector3_dot( e.origin, e.normal1 );
	e.dist2 = vector3_dot( e.origin, e.normal2 );

	InsertPointOnEdge( v1, e );
	InsertPointOnEdge( v2, e );

	if ( tjGrid ) {
		tjRegister( edgeLines.size() - 1, v1, v2 );
	}

	return edgeLines.size() - 1;
}



/*
   AddSurfaceEdges()
   adds a surface's edges
 */

static void AddSurfaceEdges( mapDrawSurface_t& ds ){
	for ( auto prev = ds.verts.end() - 1, next = ds.verts.begin(); next != ds.verts.end(); prev = next++ )
	{
		/* save the edge number in the lightmap field so we don't need to look it up again */
		bspDrawVert_edge_index_write( *prev, AddEdge( *prev, *next, false ) );
	}
}



/*
   ColinearEdge()
   determines if an edge is colinear
 */

static bool ColinearEdge( const Vector3& v1, const Vector3& v2, const Vector3& v3 ){
	const Vector3 midpoint = v2 - v1;
	Vector3 dir = v3 - v1;
	if ( VectorNormalize( dir ) == 0 ) {
		return false;  // degenerate
	}

	const float d = vector3_dot( midpoint, dir );
	const Vector3 on = dir * d;
	const Vector3 offset = midpoint - on;

	return vector3_length( offset ) < 0.1;
}



/*
   ====================
   AddPatchEdges

   Add colinear border edges, which will fix some classes of patch to
   brush tjunctions
   ====================
 */
static void AddPatchEdges( mapDrawSurface_t& ds ) {
	for ( int i = 0; i < ds.patchWidth - 2; i += 2 ) {
		{
			bspDrawVert_t& v1 = ds.verts[ i + 0 ];
			bspDrawVert_t& v2 = ds.verts[ i + 1 ];
			bspDrawVert_t& v3 = ds.verts[ i + 2 ];

			// if v2 is the midpoint of v1 to v3, add an edge from v1 to v3
			if ( ColinearEdge( v1.xyz, v2.xyz, v3.xyz ) ) {
				AddEdge( v1, v3, false );
			}
		}
		{
			bspDrawVert_t& v1 = ds.verts[ ( ds.patchHeight - 1 ) * ds.patchWidth + i + 0 ];
			bspDrawVert_t& v2 = ds.verts[ ( ds.patchHeight - 1 ) * ds.patchWidth + i + 1 ];
			bspDrawVert_t& v3 = ds.verts[ ( ds.patchHeight - 1 ) * ds.patchWidth + i + 2 ];

			// if v2 is on the v1 to v3 line, add an edge from v1 to v3
			if ( ColinearEdge( v1.xyz, v2.xyz, v3.xyz ) ) {
				AddEdge( v1, v3, false );
			}
		}
	}

	for ( int i = 0; i < ds.patchHeight - 2; i += 2 ) {
		{
			bspDrawVert_t& v1 = ds.verts[ ( i + 0 ) * ds.patchWidth ];
			bspDrawVert_t& v2 = ds.verts[ ( i + 1 ) * ds.patchWidth ];
			bspDrawVert_t& v3 = ds.verts[ ( i + 2 ) * ds.patchWidth ];

			// if v2 is the midpoint of v1 to v3, add an edge from v1 to v3
			if ( ColinearEdge( v1.xyz, v2.xyz, v3.xyz ) ) {
				AddEdge( v1, v3, false );
			}
		}
		{
			bspDrawVert_t& v1 = ds.verts[ ( ds.patchWidth - 1 ) + ( i + 0 ) * ds.patchWidth ];
			bspDrawVert_t& v2 = ds.verts[ ( ds.patchWidth - 1 ) + ( i + 1 ) * ds.patchWidth ];
			bspDrawVert_t& v3 = ds.verts[ ( ds.patchWidth - 1 ) + ( i + 2 ) * ds.patchWidth ];

			// if v2 is the midpoint of v1 to v3, add an edge from v1 to v3
			if ( ColinearEdge( v1.xyz, v2.xyz, v3.xyz ) ) {
				AddEdge( v1, v3, false );
			}
		}
	}
}


/*
   ====================
   FixSurfaceJunctions
   ====================
 */
#define MAX_SURFACE_VERTS   256
static void FixSurfaceJunctions( mapDrawSurface_t& ds ) {
	int counts[MAX_SURFACE_VERTS];
	int originals[MAX_SURFACE_VERTS];
	bspDrawVert_t verts[MAX_SURFACE_VERTS];
	int numVerts = 0;


	for ( size_t i = 0; i < ds.verts.size(); ++i )
	{
		counts[i] = 0;

		// copy first vert
		if ( numVerts == MAX_SURFACE_VERTS ) {
			Error( "MAX_SURFACE_VERTS" );
		}
		verts[numVerts] = ds.verts[i];
		originals[numVerts] = i;
		numVerts++;

		// check to see if there are any t junctions before the next vert
		const bspDrawVert_t& v1 = ds.verts[i];
		const bspDrawVert_t& v2 = ds.verts[ ( i + 1 ) % ds.verts.size() ];

		const int j = bspDrawVert_edge_index_read( ds.verts[ i ] );
		if ( j == -1 ) {
			continue;       // degenerate edge
		}
		const edgeLine_t& e = edgeLines[ j ];

		const float start = vector3_dot( v1.xyz - e.origin, e.dir );

		const float end = vector3_dot( v2.xyz - e.origin, e.dir );

		const auto insert_this_point = [&]( const edgePoint_t& p ){
			// insert this point
			if ( numVerts == MAX_SURFACE_VERTS ) {
				Error( "MAX_SURFACE_VERTS" );
			}
			bspDrawVert_t& v = verts[ numVerts ];

			/* take the exact intercept point */
			v.xyz = p.xyz;

			/* interpolate the texture coordinates */
			const float frac = ( p.intercept - start ) / ( end - start );
			v.st = v1.st + ( v2.st - v1.st ) * frac;

			/* copy the normal (FIXME: what about nonplanar surfaces? */
			v.normal = v1.normal;

			/* ydnar: interpolate the color */
			for ( int k = 0; k < MAX_LIGHTMAPS; ++k )
			{
				for ( int j = 0; j < 4; ++j )
				{
					const float c = v1.color[ k ][ j ] + frac * ( v2.color[ k ][ j ] - v1.color[ k ][ j ] );
					v.color[ k ][ j ] = color_to_byte( c );
				}
			}
			v.lightmap = { vector2_mid( v1.lightmap[ 0 ], v2.lightmap[ 0 ] ), Vector2( 0 ), Vector2( 0 ), Vector2( 0 ) };
			bspDrawVert_mark_tjunc( v );

			/* next... */
			originals[ numVerts ] = i;
			numVerts++;
			counts[ i ]++;
		};

		if( start < end ){
			for( const auto& p : e.points ){
				if( p.intercept > start + ON_EPSILON ){
					if ( p.intercept > end - ON_EPSILON )
						break;
					else
						insert_this_point( p );
				}
			}
		}
		else{
			for( const auto& p : std::ranges::reverse_view( e.points ) ){
				if( p.intercept < start - ON_EPSILON ){
					if( p.intercept < end + ON_EPSILON )
						break;
					else
						insert_this_point( p );
				}
			}
		}
	}

	c_addedVerts += numVerts - ds.verts.size();
	c_totalVerts += numVerts;


	// FIXME: check to see if the entire surface degenerated
	// after snapping

	// rotate the points so that the initial vertex is between
	// two non-subdivided edges
	int i;
	for ( i = 0; i < numVerts; ++i ) {
		if ( originals[ ( i + 1 ) % numVerts ] == originals[ i ] ) {
			continue;
		}
		const int j = ( i + numVerts - 1 ) % numVerts;
		const int k = ( i + numVerts - 2 ) % numVerts;
		if ( originals[ j ] == originals[ k ] ) {
			continue;
		}
		break;
	}

	if ( i == 0 ) {
		// fine the way it is
		c_natural++;

		ds.verts.assign( verts, verts + numVerts );

		return;
	}
	if ( i == numVerts ) {
		// create a vertex in the middle to start the fan
		c_cant++;

/*
		memset ( &verts[numVerts], 0, sizeof( verts[numVerts] ) );
		for ( i = 0; i < numVerts; ++i ) {
			for ( j = 0; j < 10; ++j ) {
				verts[numVerts].xyz[j] += verts[i].xyz[j];
			}
		}
		for ( j = 0; j < 10; ++j ) {
			verts[numVerts].xyz[j] /= numVerts;
		}

		i = numVerts;
		numVerts++;
 */
	}
	else {
		// just rotate the vertexes
		c_rotate++;
	}

	ds.verts.reserve( numVerts );
	ds.verts.assign( verts + 1, verts + numVerts );
	ds.verts.push_back( verts[ 0 ] );
}





/*
   FixBrokenSurface() - ydnar
   removes nearly coincident verts from a planar winding surface
   returns false if the surface is broken
 */

#define DEGENERATE_EPSILON  0.1

static bool FixBrokenSurface( mapDrawSurface_t& ds ){
	/* dummy check */
	if ( ds.type != ESurfaceType::Face ) {
		return false;
	}

	/* check all verts */
	for ( size_t i = 0; i < ds.verts.size(); ++i )
	{
		/* get verts */
		bspDrawVert_t& dv1 = ds.verts[ i ];
		bspDrawVert_t& dv2 = ds.verts[ ( i + 1 ) % ds.verts.size() ];
		bspDrawVert_t avg;

		/* degenerate edge? */
		avg.xyz = dv1.xyz - dv2.xyz;
		if ( vector3_length( avg.xyz ) < DEGENERATE_EPSILON ) {
			Sys_FPrintf( SYS_WRN | SYS_VRBflag, "WARNING: Degenerate T-junction edge found, fixing...\n" );

			/* create an average drawvert */
			/* ydnar 2002-01-26: added nearest-integer welding preference */
			avg.xyz = SnapWeldVector( dv1.xyz, dv2.xyz );
			avg.normal = VectorNormalized( dv1.normal + dv2.normal );
			avg.st = vector2_mid( dv1.st, dv2.st );

			/* lightmap st/colors */
			for ( int k = 0; k < MAX_LIGHTMAPS; ++k )
			{
				for ( int j = 0; j < 4; ++j )
					avg.color[ k ][ j ] = ( dv1.color[ k ][ j ] + dv2.color[ k ][ j ] ) >> 1;
			}
			avg.lightmap = { vector2_mid( dv1.lightmap[ 0 ], dv2.lightmap[ 0 ] ), Vector2( 0 ), Vector2( 0 ), Vector2( 0 ) };

			if( bspDrawVert_is_tjunc( dv1 ) && bspDrawVert_is_tjunc( dv2 ) )
				bspDrawVert_mark_tjunc( avg );

			/* ydnar: der... */
			dv1 = avg;

			/* move the remaining verts */
			ds.verts.erase( ds.verts.cbegin() + ( i + 1 ) % ds.verts.size() );

			/* after welding, we have to consider the same vertex again, as it now has a new neighbor dv2 */
			--i;

			/* should ds.numVerts have become 0, then i is now -1. In the next iteration, the loop will abort. */
		}
	}

	/* one last check and return */
	return ds.verts.size() >= 3;
}






/*
   FixTJunctions
   call after the surface list has been pruned
 */

void FixTJunctions( const entity_t& ent ){
	/* meta mode has its own t-junction code (currently not as good as this code) */
	//%	if( meta )
	//%		return;

	/* note it */
	Sys_FPrintf( SYS_VRB, "--- FixTJunctions ---\n" );
	const Timer timer;

	if ( tjGrid ) {
		MinMax world;
		for ( int i = ent.firstDrawSurf; i < numMapDrawSurfs; ++i )
		{
			const mapDrawSurface_t& ds = mapDrawSurfs[ i ];
			for ( int j = 0; j < ds.numVerts(); ++j )
				world.extend( ds.verts[ j ].xyz );
		}
		tjReset( world );
	}

	// add all the edges
	// this actually creates axial edges, but it
	// only creates originalEdge_t structures
	// for non-axial edges
	for ( mapDrawSurface_t& ds : Span( mapDrawSurfs + ent.firstDrawSurf, mapDrawSurfs + numMapDrawSurfs ) )
	{
		/* early out if possible */
		const shaderInfo_t *si = ds.shaderInfo;
		if ( ( si->compileFlags & C_NODRAW ) || si->autosprite || si->notjunc || ds.verts.empty() ) {
			continue;
		}

		/* ydnar: gs mods: handle the various types of surfaces */
		switch ( ds.type )
		{
		/* handle brush faces */
		case ESurfaceType::Face:
			AddSurfaceEdges( ds );
			break;

		/* handle patches */
		case ESurfaceType::Patch:
			AddPatchEdges( ds );
			break;

		/* fixme: make triangle surfaces t-junction */
		default:
			break;
		}
	}

	const size_t axialEdgeLines = edgeLines.size();

	// sort the non-axial edges by length
	std::ranges::sort( originalEdges, {}, &originalEdge_t::length );

	// add the non-axial edges, longest first
	// this gives the most accurate edge description
	for ( originalEdge_t& e : originalEdges ) { // originalEdges might not change during AddEdge( true )
		bspDrawVert_edge_index_write( *e.dv1, AddEdge( *e.dv1, *e.dv2, true ) );
	}
	originalEdges.clear();

	Sys_FPrintf( SYS_VRB, "%9zu axial edge lines\n", axialEdgeLines );
	Sys_FPrintf( SYS_VRB, "%9zu non-axial edge lines\n", edgeLines.size() - axialEdgeLines );
	Sys_FPrintf( SYS_VRB, "%9d degenerate edges\n", c_degenerateEdges );

	// insert any needed vertexes
	for ( mapDrawSurface_t& ds : Span( mapDrawSurfs + ent.firstDrawSurf, mapDrawSurfs + numMapDrawSurfs ) )
	{
		/* early out if possible */
		const shaderInfo_t *si = ds.shaderInfo;
		if ( ( si->compileFlags & C_NODRAW ) || si->autosprite || si->notjunc || ds.verts.empty() || ds.type != ESurfaceType::Face ) {
			continue;
		}

		/* ydnar: gs mods: handle the various types of surfaces */
		switch ( ds.type )
		{
		/* handle brush faces */
		case ESurfaceType::Face:
			FixSurfaceJunctions( ds );
			if ( !FixBrokenSurface( ds ) ) {
				c_broken++;
				ClearSurface( ds );
			}
			break;

		/* fixme: t-junction triangle models and patches */
		default:
			break;
		}
	}

	edgeLines.clear();

	/* emit some statistics */
	Sys_FPrintf( SYS_VRB, "%9d verts added for T-junctions\n", c_addedVerts );
	Sys_FPrintf( SYS_VRB, "%9d total verts\n", c_totalVerts );
	Sys_FPrintf( SYS_VRB, "%9d naturally ordered\n", c_natural );
	Sys_FPrintf( SYS_VRB, "%9d rotated orders\n", c_rotate );
	Sys_FPrintf( SYS_VRB, "%9d can't order\n", c_cant );
	Sys_FPrintf( SYS_VRB, "%9d broken (degenerate) surfaces removed\n", c_broken );
	Sys_FPrintf( SYS_VRB, "%9zu edge line tests\n", c_lineTests );
	if ( tjVerify ) {
		Sys_FPrintf( SYS_VRB, "%9zu edges the index missed a line for\n", c_gridMissed );
		Sys_FPrintf( SYS_VRB, "%9zu edges put on a different line\n", c_gridOther );
	}
	Sys_FPrintf( SYS_VRB, "%9.1f seconds elapsed\n", timer.elapsed_sec() );
}
