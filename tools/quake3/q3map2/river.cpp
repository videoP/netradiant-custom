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

   -river: solve a captured river to steady state and bake the water.

   -riverbed answers where the channel is; this answers where the water settles
   in it.  The engine can do the same solve, but it is told to produce a fixed
   slice of simulated time every frame whatever that costs, it competes with
   rendering, and the client and the server each solve the whole field
   independently.  None of that applies to a compiler.

   The solver is not reimplemented here.  river_solver.inc is jaPRO's own,
   extracted verbatim, because the engine recomputes this bake's bed and
   settings hashes and refuses a file whose numbers disagree - so an equivalent
   implementation would not be good enough, only an identical one.

   ------------------------------------------------------------------------------- */



/* dependencies */
#include "q3map2.h"
#include "riverbed.h"
#include "river_solve.h"



static void RiverReport( const char *text ){
	Sys_Printf( "%s\n", text );
}

/* The solver asks about brush membership through this, because only the stage
   can see the bsp.  Kinds and indices match the engine's. */
static bool RiverStageBrushTest( int kind, int index, const float *mins, const float *maxs ){
	const MinMax box( Vector3( mins[ 0 ], mins[ 1 ], mins[ 2 ] ),
	                  Vector3( maxs[ 0 ], maxs[ 1 ], maxs[ 2 ] ) );

	switch ( kind )
	{
	case 0:         /* zone */
		return RiverbedBoxHitsModel( s_job.zoneModel, s_job.zoneOrigin, box );

	case 1:         /* source */
		if ( index < 0 || index >= int( s_job.sources.size() ) ) {
			return false;
		}
		return s_job.sources[ index ].model <= 0
		       || RiverbedBoxHitsModel( s_job.sources[ index ].model,
		                                s_job.sources[ index ].origin, box );

	case 2:         /* sink */
		if ( index < 0 || index >= int( s_job.sinks.size() ) ) {
			return false;
		}
		return s_job.sinks[ index ].model <= 0
		       || RiverbedBoxHitsModel( s_job.sinks[ index ].model,
		                                s_job.sinks[ index ].origin, box );
	}
	return false;
}

/*
   RiverWrite()
   .river v2: a 48 byte header then one 12 byte water record per cell.

   Terrain is deliberately absent - it comes from the .riverbed, and repeating
   it would let the two disagree.  The hashes are what bind this water to that
   bed and to the entity settings it was solved under.
 */

static void RiverWrite( const char *bspPath, const RiverSolveOutput& solved ){
	int header[ 12 ];
	float *headerFloats = (float *)header;

	header[ 0 ] = ( 'R' << 24 ) | ( 'I' << 16 ) | ( 'V' << 8 ) | 'R';
	header[ 1 ] = 2;
	header[ 2 ] = s_job.width;
	header[ 3 ] = s_job.height;
	header[ 4 ] = int( s_job.cells.size() );
	headerFloats[ 5 ] = s_job.cellSize;
	headerFloats[ 6 ] = s_job.origin[ 0 ];
	headerFloats[ 7 ] = s_job.origin[ 1 ];
	headerFloats[ 8 ] = solved.simulationTime;
	header[ 9 ] = solved.stepCount;
	header[ 10 ] = int( solved.bedHash );
	header[ 11 ] = int( solved.settingsHash );

	std::vector<float> water( s_job.cells.size() * 3 );
	for ( size_t i = 0; i < s_job.cells.size(); ++i )
	{
		water[ i * 3 + 0 ] = solved.depth[ i ];
		water[ i * 3 + 1 ] = solved.momentum[ i * 2 + 0 ];
		water[ i * 3 + 2 ] = solved.momentum[ i * 2 + 1 ];
	}

	/*
	 * Named the way the engine derives it, cell size included, so several
	 * resolutions can sit side by side the way the .riverbed's already do.
	 * Without the size the engine finds a bake taken at some other cell size,
	 * loads it, and throws it out on a header mismatch - which reads like
	 * something is broken rather than like a file that was never made.  An
	 * authored 'bake' key still wins, as it does in the engine.
	 */
	StringOutputStream filename;
	if ( !s_job.bakeFile.empty() ) {
		if ( s_job.bakeFile.find_first_of( "/\\" ) != std::string::npos ) {
			filename( PathExtensionless( s_job.bakeFile.c_str() ), ".river" );
		}
		else{
			filename( PathFilenameless( bspPath ), PathExtensionless( s_job.bakeFile.c_str() ), ".river" );
		}
	}
	else{
		filename( PathExtensionless( bspPath ), "_", s_job.name.c_str(),
		          "_c", int( s_job.cellSize + 0.5f ), ".river" );
	}
	Sys_Printf( "Writing %s\n", filename.c_str() );

	FILE *file = SafeOpenWrite( filename, "wb" );
	SafeWrite( file, header, int( sizeof( header ) ) );
	SafeWrite( file, water.data(), int( water.size() * sizeof( float ) ) );
	fclose( file );

	Sys_Printf( "%9u bed hash\n", solved.bedHash );
	Sys_Printf( "%9u settings hash\n", solved.settingsHash );
	Sys_Printf( "%9zu bytes\n", sizeof( header ) + water.size() * sizeof( float ) );
}

/*
   RiverSolveOne()
   Captures the channel, then solves it.
 */

static void RiverSolveOne( const entity_t& river, float cellSizeOverride,
                           int maxSteps, const char *bspPath ){
	if ( !RiverbedPrepare( river, cellSizeOverride ) ) {
		return;
	}

	if ( s_job.sinks.empty() ) {
		Sys_Warning( "river '%s' has no sink, so the reach can only fill and will never settle\n",
		             s_job.name.c_str() );
	}

	/* hand the captured bed to the solver in its own layout */
	std::vector<float> bed( s_job.cells.size() );
	std::vector<float> ceiling( s_job.cells.size() );
	std::vector<unsigned int> flags( s_job.cells.size() );
	for ( size_t i = 0; i < s_job.cells.size(); ++i )
	{
		bed[ i ] = s_job.cells[ i ].bedHeight;
		ceiling[ i ] = s_job.cells[ i ].ceilingHeight;
		flags[ i ] = s_job.cells[ i ].flags;
	}

	std::vector<RiverSolveSource> sources( s_job.sources.size() );
	for ( size_t i = 0; i < s_job.sources.size(); ++i )
	{
		const riverbedSourceEnt_t& src = s_job.sources[ i ];
		for ( int a = 0; a < 3; ++a )
		{
			sources[ i ].mins[ a ] = src.minmax.mins[ a ];
			sources[ i ].maxs[ a ] = src.minmax.maxs[ a ];
		}
		sources[ i ].direction[ 0 ] = src.direction[ 0 ];
		sources[ i ].direction[ 1 ] = src.direction[ 1 ];
		sources[ i ].type = src.type;
		sources[ i ].discharge = src.discharge;
		sources[ i ].speed = src.speed;
		sources[ i ].surfaceHeight = src.surfaceHeight;
	}

	std::vector<RiverSolveSink> sinks( s_job.sinks.size() );
	for ( size_t i = 0; i < s_job.sinks.size(); ++i )
	{
		const riverbedSinkEnt_t& src = s_job.sinks[ i ];
		for ( int a = 0; a < 3; ++a )
		{
			sinks[ i ].mins[ a ] = src.minmax.mins[ a ];
			sinks[ i ].maxs[ a ] = src.minmax.maxs[ a ];
		}
		sinks[ i ].direction[ 0 ] = src.direction[ 0 ];
		sinks[ i ].direction[ 1 ] = src.direction[ 1 ];
		sinks[ i ].type = src.type;
		sinks[ i ].surfaceHeight = src.surfaceHeight;
		sinks[ i ].drainRate = src.drainRate;
	}

	RiverSolveInput in;
	memset( &in, 0, sizeof( in ) );
	in.name = s_job.name.c_str();
	for ( int a = 0; a < 3; ++a )
	{
		in.mins[ a ] = s_job.minmax.mins[ a ];
		in.maxs[ a ] = s_job.minmax.maxs[ a ];
	}
	in.cellSize = s_job.cellSize;
	in.friction = s_job.friction;
	in.captureHeadroom = s_job.captureHeadroom;
	in.zoneModel = s_job.zoneModel;
	in.width = s_job.width;
	in.height = s_job.height;
	in.cellCount = int( s_job.cells.size() );
	in.cellIndex = s_job.cellIndex.data();
	in.bedHeight = bed.data();
	in.ceilingHeight = ceiling.data();
	in.flags = flags.data();
	in.sources = sources.data();
	in.numSources = int( sources.size() );
	in.sinks = sinks.data();
	in.numSinks = int( sinks.size() );
	in.reach = s_job.reach.empty() ? NULL : s_job.reach.data();
	in.reachMax = s_job.reachMax;
	in.dischargeOverride = -1.0f;
	in.maxSteps = maxSteps;
	in.reportEvery = 5000;

	std::vector<float> depth( s_job.cells.size() );
	std::vector<float> momentum( s_job.cells.size() * 2 );

	RiverSolveOutput out;
	memset( &out, 0, sizeof( out ) );
	out.depth = depth.data();
	out.momentum = momentum.data();

	Sys_Printf( "--- Solving (%s) ---\n", s_job.name.c_str() );
	RiverSolveSetReporter( RiverReport );

	if ( !RiverSolve( in, RiverStageBrushTest, out ) ) {
		Sys_Warning( "river '%s' could not be solved\n", s_job.name.c_str() );
		return;
	}

	Sys_Printf( "%9d inlet cells, %d outlet cells\n", out.sourceCells, out.sinkCells );
	if ( out.sinkCells == 0 && !s_job.sinks.empty() ) {
		Sys_Warning( "the outlet brushes claimed no cell, so nothing can leave the reach "
		             "and it can never settle - check that a sink brush actually overlaps "
		             "the captured channel\n" );
	}
	Sys_Printf( "%9d steps, %.1f seconds simulated\n", out.stepCount, out.simulationTime );
	Sys_Printf( "%9d wet cells of %d\n", out.wetCells, int( s_job.cells.size() ) );
	Sys_Printf( "%9.3e volume, rate %+.3e\n", out.volume, out.volumeRate );

	/* Both guards should read zero.  clamped means the momentum ceiling fired
	   and that step silently did not conserve momentum; reset means a cell went
	   non-finite.  Either one makes the field something other than a solution
	   of the equations, so numbers taken from it are not trustworthy. */
	if ( out.clampedCells != 0 || out.resetCells != 0 ) {
		Sys_Warning( "solver guards fired: %d clamped, %d reset - this bake is not a clean solution\n",
		             out.clampedCells, out.resetCells );
	}

	if ( !out.converged ) {
		Sys_Warning( "river '%s' did not settle within the step limit; writing it anyway, "
		             "and the engine will carry on solving from it as a warm start\n",
		             s_job.name.c_str() );
	}

	RiverWrite( bspPath, out );
}

/*
   RiverMain()
   -river, run against a compiled bsp.
 */

int RiverMain( Args& args ){
	float cellSizeOverride = 0.0f;
	int maxSteps = 0;

	/* arg checking */
	if ( args.empty() ) {
		Sys_Printf( "Usage: q3map2 -river [-v] [-cellsize <n>] [-maxsteps <n>] <mapname>\n" );
		return 0;
	}

	while ( !args.empty() )
	{
		if ( args.takeArg( "-cellsize" ) ) {
			cellSizeOverride = atof( args.takeNext() );
			Sys_Printf( "Overriding cell size with %.0f\n", cellSizeOverride );
		}
		else if ( args.takeArg( "-maxsteps" ) ) {
			maxSteps = atoi( args.takeNext() );
			Sys_Printf( "Stopping after %d steps\n", maxSteps );
		}
		else{
			break;
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
			RiverSolveOne( ent, cellSizeOverride, maxSteps, source );
			++rivers;
		}
	}

	if ( rivers == 0 ) {
		Sys_Warning( "no misc_sailing_river in this bsp\n" );
	}

	/* return to sender */
	return 0;
}
