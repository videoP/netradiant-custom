/*
   riverbed.h

   What the -river stage borrows from -riverbed.  Both stages need the same
   channel: capture answers "where is the bed", and the solver then answers
   "where does the water settle" over exactly those cells.
 */

#pragma once

#include "q3map2.h"

#define RIVERBED_CELL_ACTIVE    0x00000001u

struct riverbedCell_t
{
	float bedHeight;
	float ceilingHeight;
	unsigned int flags;
};

/* one solid brush, with the bounds used to bucket it */
struct riverbedBrush_t
{
	int firstSide;
	int numSides;
	MinMax minmax;
};

/* Entity records, carried whole so the -river stage can hand the solver the
   same numbers the engine would have read off the same keys. */
struct riverbedSourceEnt_t
{
	MinMax minmax;
	int model;
	Vector3 origin;
	float direction[ 2 ];
	int type;                       /* 0 discharge, 1 velocity */
	float discharge;                /* cubic map units per second */
	float speed;
	float surfaceHeight;
};

struct riverbedSinkEnt_t
{
	MinMax minmax;
	int model;
	Vector3 origin;
	float direction[ 2 ];
	int type;                       /* 0 open, 1 fixed, 2 normal, 3 overfall, 4 drain */
	float surfaceHeight;
	float drainRate;
};

struct riverbedJob_t
{
	std::string name;
	int zoneModel;
	MinMax minmax;                  /* corridor brush bounds, plus entity origin */
	Vector3 zoneOrigin;
	float cellSize;
	float captureHeadroom;

	int width;
	int height;
	float origin[ 2 ];

	std::vector<int> cellIndex;     /* grid -> compact, or -1 outside the corridor */
	std::vector<riverbedCell_t> cells;

	/* Inlets and outlets in entity order, which the solver's settings hash
	   depends on: it walks the tables in registration order, so a different
	   order is a different hash and the engine rejects the bake. */
	std::vector<riverbedSourceEnt_t> sources;
	std::vector<riverbedSinkEnt_t> sinks;

	float friction;
};

extern riverbedJob_t s_job;
extern std::vector<riverbedBrush_t> s_solidBrushes;

void RiverbedBuildSolidIndex();
bool RiverbedPrepare( const entity_t& river, float cellSizeOverride );
void RiverbedWrite( const char *bspPath );
bool RiverbedBoxHitsModel( int modelNum, const Vector3& translation, const MinMax& box );
