/*
   river_solve.h

   The boundary between the -river stage and the engine's shallow water solver.

   Nothing here names an engine type.  river_solve.cpp compiles the solver
   verbatim against river_shim.h, which redefines qboolean, vec3_t and Q_stricmp
   the way jaPRO does and therefore cannot be in the same translation unit as
   q3map2.h.  Plain floats and ints cross this line instead.
 */

#pragma once

struct RiverSolveSource
{
	float mins[ 3 ];
	float maxs[ 3 ];
	float direction[ 2 ];
	int type;                       /* 0 discharge, 1 velocity */
	float discharge;                /* already scaled to cubic map units */
	float speed;
	float surfaceHeight;
};

struct RiverSolveSink
{
	float mins[ 3 ];
	float maxs[ 3 ];
	float direction[ 2 ];
	int type;                       /* 0 open, 1 fixed, 2 normal, 3 overfall, 4 drain */
	float surfaceHeight;
	float drainRate;
};

struct RiverSolveInput
{
	const char *name;
	float mins[ 3 ];
	float maxs[ 3 ];
	float cellSize;
	float friction;
	float captureHeadroom;
	int zoneModel;

	/* bed, as riverbed.cpp captured it */
	int width;
	int height;
	int cellCount;
	const int *cellIndex;           /* grid -> compact, -1 outside the corridor */
	const float *bedHeight;         /* cellCount entries */
	const float *ceilingHeight;
	const unsigned int *flags;

	const RiverSolveSource *sources;
	int numSources;
	const RiverSolveSink *sinks;
	int numSinks;

	/* Along-channel distance of each cell in cells, from the capture walk, so
	   progress can be reported as how far the front has come rather than as a
	   volume nobody can size by eye.  Optional; NULL to omit it. */
	const int *reach;
	int reachMax;

	float dischargeOverride;        /* < 0 for none */
	int maxSteps;                   /* 0 for unlimited */
	int reportEvery;                /* steps between progress lines, 0 to hush */
};

struct RiverSolveOutput
{
	float *depth;                   /* cellCount, caller-owned */
	float *momentum;                /* cellCount * 2 */
	unsigned int bedHash;
	unsigned int settingsHash;
	float simulationTime;
	int stepCount;
	int wetCells;
	float volume;
	float volumeRate;
	int clampedCells;
	int resetCells;
	/* How many cells each boundary actually claimed.  An outlet that claimed
	   none cannot remove anything, which looks exactly like a reach that will
	   not settle - so it is worth knowing before blaming the hydraulics. */
	int sourceCells;
	int sinkCells;
	bool converged;
};

/* Exact brush membership, supplied by the stage because only it can see the
   bsp.  kind is 0 zone, 1 source, 2 sink; index selects which. */
typedef bool ( *RiverBrushTestFn )( int kind, int index,
                                    const float *mins, const float *maxs );

bool RiverSolve( const RiverSolveInput& in, RiverBrushTestFn brushTest,
                 RiverSolveOutput& out );
