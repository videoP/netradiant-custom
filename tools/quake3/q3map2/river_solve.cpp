/*
   river_solve.cpp

   Runs the engine's shallow water solver offline.

   The physics in river_solver.inc is jaPRO's, extracted verbatim by
   river_extract.py.  That is not tidiness: the engine recomputes this bake's
   bed and settings hashes and refuses to load a file whose numbers disagree, so
   a reimplementation that was merely equivalent would be rejected.  Fix bugs in
   jaPRO and re-run the extractor; do not edit the .inc.

   This translation unit deliberately never includes q3map2.h - see river_shim.h
   for why.
 */

#include "river_shim.h"
#include "river_solve.h"

#include <stdlib.h>

/* the tables the extracted boundary code reads */
sailingRiverSource_t bgSailingRiverSources[ MAX_SAILING_RIVER_SOURCES ];
int bgNumSailingRiverSources;
sailingRiverSink_t bgSailingRiverSinks[ MAX_SAILING_RIVER_SINKS ];
int bgNumSailingRiverSinks;

/* progress reporting, kept out of the .inc */
static void ( *s_report )( const char *text );

static void RiverPrint( const char *format, ... ){
	char buffer[ 1024 ];
	va_list args;
	va_start( args, format );
	vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	if ( s_report != NULL ) {
		s_report( buffer );
	}
}

#include "river_solver.inc"

/* ------------------------------------------------------------------------- */

static RiverBrushTestFn s_brushTest;

static qboolean RiverSolveBrushAdapter( int kind, int index,
                                        const vec3_t mins, const vec3_t maxs ){
	if ( s_brushTest == NULL ) {
		return qtrue;
	}
	return s_brushTest( kind, index, mins, maxs ) ? qtrue : qfalse;
}

void RiverSolveSetReporter( void ( *reporter )( const char *text ) ){
	s_report = reporter;
}

bool RiverSolve( const RiverSolveInput& in, RiverBrushTestFn brushTest,
                 RiverSolveOutput& out ){
	sailingRiver_t river;
	sailingRiverField_t field;
	int i;

	if ( in.cellCount <= 0 || in.width <= 0 || in.height <= 0 ) {
		return false;
	}

	s_brushTest = brushTest;

	/* --- river metadata, as the entity keys give it --- */
	memset( &river, 0, sizeof( river ) );
	for ( i = 0; i < 3; ++i ) {
		river.mins[ i ] = in.mins[ i ];
		river.maxs[ i ] = in.maxs[ i ];
	}
	strncpy( river.name, in.name, sizeof( river.name ) - 1 );
	river.cellSize = in.cellSize;
	river.friction = in.friction;
	river.captureHeadroom = in.captureHeadroom;
	river.zoneModel = in.zoneModel;

	/* --- the inlet and outlet tables --- */
	bgNumSailingRiverSources = 0;
	for ( i = 0; i < in.numSources && i < MAX_SAILING_RIVER_SOURCES; ++i )
	{
		sailingRiverSource_t& dst = bgSailingRiverSources[ bgNumSailingRiverSources++ ];
		const RiverSolveSource& src = in.sources[ i ];
		memset( &dst, 0, sizeof( dst ) );
		for ( int a = 0; a < 3; ++a ) {
			dst.mins[ a ] = src.mins[ a ];
			dst.maxs[ a ] = src.maxs[ a ];
		}
		dst.direction[ 0 ] = src.direction[ 0 ];
		dst.direction[ 1 ] = src.direction[ 1 ];
		dst.type = (sailingRiverSourceType_t)src.type;
		dst.discharge = src.discharge;
		dst.speed = src.speed;
		dst.surfaceHeight = src.surfaceHeight;
		strncpy( dst.riverName, in.name, sizeof( dst.riverName ) - 1 );
	}

	bgNumSailingRiverSinks = 0;
	for ( i = 0; i < in.numSinks && i < MAX_SAILING_RIVER_SINKS; ++i )
	{
		sailingRiverSink_t& dst = bgSailingRiverSinks[ bgNumSailingRiverSinks++ ];
		const RiverSolveSink& src = in.sinks[ i ];
		memset( &dst, 0, sizeof( dst ) );
		for ( int a = 0; a < 3; ++a ) {
			dst.mins[ a ] = src.mins[ a ];
			dst.maxs[ a ] = src.maxs[ a ];
		}
		dst.direction[ 0 ] = src.direction[ 0 ];
		dst.direction[ 1 ] = src.direction[ 1 ];
		dst.type = (sailingRiverSinkType_t)src.type;
		dst.surfaceHeight = src.surfaceHeight;
		dst.drainRate = src.drainRate;
		strncpy( dst.riverName, in.name, sizeof( dst.riverName ) - 1 );
	}

	/* --- the field, laid out exactly as the engine lays it out --- */
	const int gridCells = in.width * in.height;

	sailingRiverCell_t *cells = (sailingRiverCell_t *)calloc( in.cellCount, sizeof( sailingRiverCell_t ) );
	sailingRiverWater_t *scratch = (sailingRiverWater_t *)calloc( in.cellCount, sizeof( sailingRiverWater_t ) );
	sailingRiverBoundaryLinks_t *links = (sailingRiverBoundaryLinks_t *)calloc( in.cellCount, sizeof( sailingRiverBoundaryLinks_t ) );
	int *cellIndex = (int *)calloc( gridCells, sizeof( int ) );
	int *gridIndexOf = (int *)calloc( in.cellCount, sizeof( int ) );
	int *captureQueue = (int *)calloc( in.cellCount, sizeof( int ) );
	int *activeCells = (int *)calloc( in.cellCount, sizeof( int ) );
	short *sourceOwner = (short *)calloc( in.cellCount, sizeof( short ) );
	short *sinkOwner = (short *)calloc( in.cellCount, sizeof( short ) );

	if ( !cells || !scratch || !links || !cellIndex || !gridIndexOf
	     || !captureQueue || !activeCells || !sourceOwner || !sinkOwner ) {
		free( cells ); free( scratch ); free( links ); free( cellIndex );
		free( gridIndexOf ); free( captureQueue ); free( activeCells );
		free( sourceOwner ); free( sinkOwner );
		return false;
	}

	memcpy( cellIndex, in.cellIndex, gridCells * sizeof( int ) );

	memset( &field, 0, sizeof( field ) );
	if ( !BG_SailingRiverFieldInit( &field, &river, cells, scratch,
	                                captureQueue, activeCells, links,
	                                sourceOwner, sinkOwner, cellIndex,
	                                gridIndexOf, gridCells, in.cellCount,
	                                in.cellCount ) ) {
		free( cells ); free( scratch ); free( links ); free( cellIndex );
		free( gridIndexOf ); free( captureQueue ); free( activeCells );
		free( sourceOwner ); free( sinkOwner );
		return false;
	}

	/* the captured bed */
	for ( i = 0; i < in.cellCount; ++i )
	{
		cells[ i ].bedHeight = in.bedHeight[ i ];
		cells[ i ].ceilingHeight = in.ceilingHeight[ i ];
		cells[ i ].flags = in.flags[ i ] & SAILING_RIVER_CELL_ACTIVE;
		cells[ i ].depth = 0.0f;
		VectorClear2( cells[ i ].momentum );
	}
	BG_SailingRiverFieldInvalidateActive( &field );

	/* which inlet or outlet owns each cell, brush exact, plus the linked
	   lists the boundary passes walk */
	BG_SailingRiverFieldMarkBoundaries( &field, &river, RiverSolveBrushAdapter );

	/* dry start, exactly as the engine does it: a blanket prefill invents a
	   lake the boundaries never supplied */
	BG_SailingRiverFieldInitializeWater( &field, &river, qfalse );

	/* --- solve --- */
	const float gravity = SAILING_GRAVITY;
	const float cfl = 0.20f;
	const float maxDt = 0.02f;
	const float tolerance = 0.01f;
	const int convergeChecks = 16;

	const float inflow = BG_SailingRiverInflowDischarge( &river, in.dischargeOverride );
	int converged = 0;
	int steps = 0;

	while ( in.maxSteps <= 0 || steps < in.maxSteps )
	{
		const float timeStep = BG_SailingRiverFieldTimeStep( &field, gravity, cfl, maxDt );

		if ( timeStep <= 0.0f ) {
			break;
		}
		if ( !BG_SailingRiverFieldStep( &field, timeStep, gravity, river.friction ) ) {
			break;
		}
		BG_SailingRiverFieldApplyBoundariesOverride( &field, &river, timeStep,
		                                             in.dischargeOverride, river.friction );
		++steps;

		/* The engine requires the volume rate to stay settled for several
		   consecutive checks, not merely touch the tolerance once: a reach
		   that is sloshing crosses zero twice a cycle. */
		if ( BG_SailingRiverFieldConverged( &field, inflow, tolerance ) ) {
			if ( ++converged >= convergeChecks ) {
				break;
			}
		}
		else{
			converged = 0;
		}

		if ( in.reportEvery > 0 && ( steps % in.reportEvery ) == 0 ) {
			RiverPrint( "%9d steps  %8.1fs simulated  volume %.3e  rate %+.3e",
			            steps, field.simulationTime, field.volume, field.volumeRate );
		}
	}

	/* --- results --- */
	out.wetCells = 0;
	for ( i = 0; i < in.cellCount; ++i )
	{
		out.depth[ i ] = cells[ i ].depth;
		out.momentum[ i * 2 + 0 ] = cells[ i ].momentum[ 0 ];
		out.momentum[ i * 2 + 1 ] = cells[ i ].momentum[ 1 ];
		if ( cells[ i ].depth > 0.0f ) {
			++out.wetCells;
		}
	}

	out.bedHash = BG_SailingRiverBedHash( &field );
	out.settingsHash = BG_SailingRiverSettingsHash( &river, in.dischargeOverride );
	out.simulationTime = field.simulationTime;
	out.stepCount = field.stepCount;
	out.volume = field.volume;
	out.volumeRate = field.volumeRate;
	out.clampedCells = field.clampedCells;
	out.resetCells = field.resetCells;
	out.converged = converged >= convergeChecks;

	free( cells ); free( scratch ); free( links ); free( cellIndex );
	free( gridIndexOf ); free( captureQueue ); free( activeCells );
	free( sourceOwner ); free( sinkOwner );
	return true;
}
