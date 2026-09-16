/*
   river_shim.h

   Just enough of jaPRO's q_shared/bg_sailing to compile river_solver.inc, which
   is the engine's shallow water solver lifted verbatim.

   This header must never meet q3map2.h.  The two codebases disagree about
   several names that both consider fundamental - q3map2's qboolean is bool
   while the engine's is an enum, its vec3_t is vec_t[3], and Q_stricmp is a
   macro there and a function here - so the solver is compiled as its own
   translation unit and reaches the rest of the stage through river_solve.h,
   which mentions none of these types.
 */

#pragma once

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ---- engine primitives ---- */

/* int rather than the engine's enum: this is compiled as C++, where a bool
   expression will not implicitly convert to an enum, and the solver returns
   comparisons directly in a dozen places.  Both are int sized and the type is
   never serialised, so nothing downstream can tell the difference. */
typedef int qboolean;
#define qfalse 0
#define qtrue 1

typedef float vec_t;
typedef vec_t vec2_t[ 2 ];
typedef vec_t vec3_t[ 3 ];

#define MAX_QPATH 64

#define Q_min( x, y ) ( ( x ) < ( y ) ? ( x ) : ( y ) )
#define Q_max( x, y ) ( ( x ) > ( y ) ? ( x ) : ( y ) )

static inline float Com_Clamp( float min, float max, float value ){
	if ( value < min ) {
		return min;
	}
	if ( value > max ) {
		return max;
	}
	return value;
}

static inline int Com_Clampi( int min, int max, int value ){
	if ( value < min ) {
		return min;
	}
	if ( value > max ) {
		return max;
	}
	return value;
}

/* Bit test rather than a comparison, matching the engine: the compilers this
   is built with are free to assume no NaN under fast math and fold x != x. */
static inline qboolean Q_isnan( float f ){
	unsigned int bits;
	memcpy( &bits, &f, 4 );
	return ( ( bits & 0x7f800000u ) == 0x7f800000u
	         && ( bits & 0x007fffffu ) != 0 ) ? qtrue : qfalse;
}

static inline int Q_stricmp( const char *a, const char *b ){
#ifdef _WIN32
	return _stricmp( a, b );
#else
	return strcasecmp( a, b );
#endif
}

static inline void VectorClear2( vec2_t v ){
	v[ 0 ] = v[ 1 ] = 0.0f;
}

static inline void VectorCopy2( const vec2_t in, vec2_t out ){
	out[ 0 ] = in[ 0 ];
	out[ 1 ] = in[ 1 ];
}

static inline void VectorScale2( const vec2_t in, float scale, vec2_t out ){
	out[ 0 ] = in[ 0 ] * scale;
	out[ 1 ] = in[ 1 ] * scale;
}

static inline void VectorSubtract2( const vec2_t a, const vec2_t b, vec2_t out ){
	out[ 0 ] = a[ 0 ] - b[ 0 ];
	out[ 1 ] = a[ 1 ] - b[ 1 ];
}

static inline float VectorLengthSquared2( const vec2_t v ){
	return v[ 0 ] * v[ 0 ] + v[ 1 ] * v[ 1 ];
}

#define VectorSet( v, x, y, z ) ( ( v )[ 0 ] = ( x ), ( v )[ 1 ] = ( y ), ( v )[ 2 ] = ( z ) )

/* ---- bg_sailing constants ---- */

#define MAX_SAILING_RIVERS 16
#define MAX_SAILING_RIVER_SOURCES 32
#define MAX_SAILING_RIVER_SINKS 32
#define MAX_SAILING_RIVER_BASE_SHADERS 8
#define SAILING_RIVER_MAX_CELLS 6000000

#define SAILING_MAP_UNITS_PER_FOOT 12.0f
#define SAILING_CUBIC_MAP_UNITS_PER_CUBIC_FOOT \
	( SAILING_MAP_UNITS_PER_FOOT * SAILING_MAP_UNITS_PER_FOOT \
	  * SAILING_MAP_UNITS_PER_FOOT )

#define SAILING_RIVER_CELL_ACTIVE 0x00000001u
#define SAILING_RIVER_CELL_SOURCE 0x00000002u
#define SAILING_RIVER_CELL_SINK 0x00000004u

#define SAILING_GRAVITY 800.0f

#define SAILING_RIVER_BAKE_MAGIC ( ( 'R' << 24 ) | ( 'I' << 16 ) \
	| ( 'V' << 8 ) | 'R' )
#define SAILING_RIVER_BAKE_VERSION 2

/* ---- bg_sailing types ---- */

typedef enum {
	SAILING_RIVER_SOURCE_DISCHARGE,
	SAILING_RIVER_SOURCE_VELOCITY
} sailingRiverSourceType_t;

typedef enum {
	SAILING_RIVER_SINK_OPEN,
	SAILING_RIVER_SINK_FIXED_STAGE,
	SAILING_RIVER_SINK_NORMAL_DEPTH,
	SAILING_RIVER_SINK_OVERFALL,
	SAILING_RIVER_SINK_DRAIN
} sailingRiverSinkType_t;

typedef struct sailingRiver_s {
	vec3_t mins;
	vec3_t maxs;
	char name[ MAX_QPATH ];
	char bakeFile[ MAX_QPATH ];
	char baseShaders[ MAX_SAILING_RIVER_BASE_SHADERS ][ MAX_QPATH ];
	int baseShaderCount;
	char meshShader[ MAX_QPATH ];
	float cellSize;
	float buoyancy;
	float buoyancyDamping;
	float buoyancyDepth;
	float diveDrag;
	float friction;
	float captureHeadroom;
	int zoneModel;
	vec3_t zoneOrigin;
	vec3_t zoneAngles;
} sailingRiver_t;

typedef struct sailingRiverSource_s {
	vec3_t mins;
	vec3_t maxs;
	char riverName[ MAX_QPATH ];
	vec2_t direction;
	sailingRiverSourceType_t type;
	float discharge;
	float speed;
	float surfaceHeight;
	int zoneModel;
	vec3_t zoneOrigin;
} sailingRiverSource_t;

typedef struct sailingRiverSink_s {
	vec3_t mins;
	vec3_t maxs;
	char riverName[ MAX_QPATH ];
	vec2_t direction;
	sailingRiverSinkType_t type;
	int zoneModel;
	vec3_t zoneOrigin;
	float surfaceHeight;
	float drainRate;
} sailingRiverSink_t;

typedef struct sailingRiverCell_s {
	float bedHeight;
	float ceilingHeight;
	float depth;
	vec2_t momentum;
	unsigned int flags;
} sailingRiverCell_t;

typedef struct sailingRiverWater_s {
	float depth;
	vec2_t momentum;
} sailingRiverWater_t;

typedef struct sailingRiverBoundaryLinks_s {
	int sourceNext;
	int sinkNext;
} sailingRiverBoundaryLinks_t;

typedef struct sailingRiverField_s {
	vec2_t origin;
	float cellSize;
	int width;
	int height;
	int cellCount;
	int *cellIndex;
	int *gridIndexOf;
	int gridCellCount;
	float outsideBedHeight;
	int stepCount;
	float simulationTime;
	int clampedCells;
	int resetCells;
	float depthResidual;
	float meanDepth;
	float volume;
	float volumeRate;
	sailingRiverCell_t *cells;
	sailingRiverWater_t *scratch;
	sailingRiverBoundaryLinks_t *boundaryLinks;
	int sourceHead[ MAX_SAILING_RIVER_SOURCES ];
	int sinkHead[ MAX_SAILING_RIVER_SINKS ];
	int workCount;
	qboolean workDirty;
	qboolean workDense;
	int *captureQueue;
	int *activeCells;
	int activeCount;
	short *sourceOwner;
	short *sinkOwner;
	int capacity;
} sailingRiverField_t;

typedef struct sailingRiverSample_s {
	float bedHeight;
	float ceilingHeight;
	float surfaceHeight;
	float depth;
	vec2_t velocity;
	unsigned int flags;
} sailingRiverSample_t;

/* Supplied by the caller for the same reason the engine supplies it: the two
   sides reach collision through different APIs.  Kinds match the engine's. */
enum {
	SAILING_RIVER_BRUSH_ZONE,
	SAILING_RIVER_BRUSH_SOURCE,
	SAILING_RIVER_BRUSH_SINK
};
typedef qboolean ( *sailingRiverBrushTest_t )( int kind, int index,
                                               const vec3_t mins, const vec3_t maxs );

/* One work item of the face pass, and the optional dispatcher for it. Gather
   makes every item independent, so the pass can be split across threads without
   changing the answer; the engine leaves the hook NULL, the compiler sets it. */
typedef struct sailingRiverFaceJob_s {
	sailingRiverField_t *field;
	float gravity;
	float scale;
} sailingRiverFaceJob_t;

typedef void ( *sailingRiverParallelFor_t )( const sailingRiverFaceJob_t *job,
                                             int count );
extern sailingRiverParallelFor_t bgSailingRiverParallelFor;

/* ---- the entity tables the boundary code reads ---- */

extern sailingRiverSource_t bgSailingRiverSources[ MAX_SAILING_RIVER_SOURCES ];
extern int bgNumSailingRiverSources;
extern sailingRiverSink_t bgSailingRiverSinks[ MAX_SAILING_RIVER_SINKS ];
extern int bgNumSailingRiverSinks;
