/*
   Large map performance options. See largemap.h.
 */

#include "largemap.h"

#include "stringio.h"
#include "preferencesystem.h"
#include "script/scripttokeniser.h" // g_scriptTokeniser_fastPath

#include <QCheckBox>

LatchedBool g_largemap_deferEntityList( false, "Deferred Entity List population" );
LatchedBool g_largemap_spatialIndex( false, "Spatial index for view culling" );
LatchedBool g_largemap_staticBatch( false, "Batched static geometry" );
LatchedBool g_largemap_incrementalBounds( false, "Cached container bounds" );
LatchedBool g_largemap_shareShaderNames( false, "Shared shader names" );
LatchedBool g_largemap_poolFaces( false, "Pooled face allocation" );
LatchedBool g_largemap_fastParse( false, "Fast map parsing" );

/* not latched: these are only ever read when a build command is assembled */
bool g_largemap_cullGrid = false;
bool g_largemap_tjGrid = false;


void LargeMap_constructPreferences( PreferencesPage& page ){
	page.appendCheckBox(
	    "Load", "Defer Entity List population",
	    LatchedImportCaller( g_largemap_deferEntityList ),
	    BoolExportCaller( g_largemap_deferEntityList.m_latched )
	)->setToolTip(
	    "Speeds up opening a large map.\n\n"
	    "Every primitive is normally filed into the Entity List as it loads, at a\n"
	    "cost that grows with the number already there, so the time taken grows\n"
	    "with the square of the map size. Nothing reads that list unless the\n"
	    "Entity List window is open, so this builds it only when you open it.\n\n"
	    "Measured: a 200 MB map went from 20s to 6.5s.\n\n"
	    "Cost: opening the Entity List on a huge map takes a moment."
	);
	page.appendCheckBox(
	    "View", "Spatial index for view culling",
	    LatchedImportCaller( g_largemap_spatialIndex ),
	    BoolExportCaller( g_largemap_spatialIndex.m_latched )
	)->setToolTip(
	    "Stops every view walking the whole map every frame.\n\n"
	    "Worldspawn holds practically the entire map, so its bounds always\n"
	    "overlap the view and nothing above individual brushes is ever skipped.\n"
	    "This groups brushes into 1024-unit cells, so a cell out of view is\n"
	    "rejected in one test instead of one test per brush inside it.\n\n"
	    "Most of the benefit needs Batched static geometry on as well: without it\n"
	    "the brushes in a visible cell still have to be visited one by one."
	);
	page.appendCheckBox(
	    "View", "Batched static geometry (3D view)",
	    LatchedImportCaller( g_largemap_staticBatch ),
	    BoolExportCaller( g_largemap_staticBatch.m_latched )
	)->setToolTip(
	    "Draws unselected worldspawn geometry in far fewer pieces.\n\n"
	    "Normally every face of every brush is submitted to the graphics card\n"
	    "separately, every frame. This pre-combines them per cell, so a cell\n"
	    "costs one submission per texture rather than one per face, and its\n"
	    "brushes can then be skipped by the spatial index entirely.\n\n"
	    "Measured with the index: about 9x the frame rate on a 122k brush map.\n\n"
	    "Selected brushes leave the batch so they can still show a highlight, so\n"
	    "selecting a great deal at once is slower until you deselect. Turns\n"
	    "itself off in the 3D view's Lighting and Wireframe modes, which it\n"
	    "cannot draw correctly."
	);
	page.appendCheckBox(
	    "Editing", "Cached container bounds",
	    LatchedImportCaller( g_largemap_incrementalBounds ),
	    BoolExportCaller( g_largemap_incrementalBounds.m_latched )
	)->setToolTip(
	    "Makes editing on a large map cheaper.\n\n"
	    "Changing one brush marks worldspawn's overall size as unknown, and\n"
	    "recomputing it measures every brush in the map. That happens after any\n"
	    "edit and on every frame while you drag. This remembers the answer and\n"
	    "only widens it as things move.\n\n"
	    "The remembered size can end up slightly larger than the truth until the\n"
	    "next time brushes are added or deleted, which costs a little culling\n"
	    "accuracy and nothing else."
	);
	page.appendCheckBox(
	    "Load", "Shared shader names",
	    LatchedImportCaller( g_largemap_shareShaderNames ),
	    BoolExportCaller( g_largemap_shareShaderNames.m_latched )
	)->setToolTip(
	    "Saves memory and load time on maps with many faces.\n\n"
	    "Every face normally keeps its own copy of the name of the shader on it,\n"
	    "even though a terrain map has hundreds of thousands of faces naming a\n"
	    "handful of shaders. This keeps one copy of each name and shares it.\n\n"
	    "On a 280k brush map that is roughly two million copies avoided."
	);
	page.appendCheckBox(
	    "Load", "Pooled face allocation",
	    LatchedImportCaller( g_largemap_poolFaces ),
	    BoolExportCaller( g_largemap_poolFaces.m_latched )
	)->setToolTip(
	    "Speeds up loading and saves memory on maps with many brushes.\n\n"
	    "Brush faces are all the same size and are created in enormous numbers,\n"
	    "which is what a general-purpose allocator handles worst - each one\n"
	    "carries its own bookkeeping. This hands them out of large blocks.\n\n"
	    "Memory taken by faces is not returned to the system until you quit,\n"
	    "which suits an editor, since the peak is reached when the map loads."
	);
	page.appendCheckBox(
	    "Load", "Fast map parsing",
	    LatchedImportCaller( g_largemap_fastParse ),
	    BoolExportCaller( g_largemap_fastParse.m_latched )
	)->setToolTip(
	    "Reads .map files considerably faster.\n\n"
	    "Recognising a word in the file normally costs an indirect function call\n"
	    "for every single character, and looking up the shader on each face builds\n"
	    "a temporary copy of its name just to search with. This reads whole words\n"
	    "straight out of the file buffer and searches without the copy.\n\n"
	    "Measured on a 1 GB map: parsing went from 23.7s to 13.1s.\n\n"
	    "It produces exactly the same result - the two were compared word for word\n"
	    "over 215 million words of a real map. The switch is here so that if a map\n"
	    "ever loads oddly, this can be ruled out in one restart."
	);
	page.appendCheckBox(
	    "Compile", "Gridded CullSides (q3map2 -cullgrid)",
	    g_largemap_cullGrid
	)->setToolTip(
	    "Speeds up the CullSides stage of a compile on a map with many brushes.\n\n"
	    "q3map2 works out which brush faces are buried inside other brushes by\n"
	    "testing every brush against every other one, so the work grows with the\n"
	    "square of the brush count and nearly all of it goes on proving that\n"
	    "brushes at opposite ends of the map do not touch. This sorts them into a\n"
	    "grid first and compares only brushes that are actually near each other.\n\n"
	    "Measured on a 97k brush map: CullSides went from 26.7s to 0.3s, and the\n"
	    "compiled .bsp was identical.\n\n"
	    "Unlike the rest of this page this is not a setting of the editor: it adds\n"
	    "-cullgrid to the command line of every build command that runs q3map2. It\n"
	    "needs a q3map2 new enough to know that switch - an older one will print\n"
	    "\"Unknown option\" and compile exactly as it did before."
	);

	page.appendCheckBox(
	    "Compile", "Gridded FixTJunctions (q3map2 -tjgrid)",
	    g_largemap_tjGrid
	)->setToolTip(
	    "Speeds up the FixTJunctions stage of a compile on a map with many brushes.\n\n"
	    "To weld cracks between neighbouring faces q3map2 groups edges onto the\n"
	    "lines they lie along, and it finds an edge's line by comparing it with\n"
	    "every line found so far. Both numbers grow with the map, so the work grows\n"
	    "with the square of it. This looks up only the lines that run near the edge.\n\n"
	    "Measured on a 6.4k brush map: 1.9 billion line comparisons down to 5\n"
	    "million, 1.6s to 0.1s, and the compiled .bsp was identical.\n\n"
	    "Like the option above this is a q3map2 command line switch rather than a\n"
	    "setting of the editor, and an older q3map2 will ignore it with a warning."
	);

	/* To read the effect of these: View / Show Stats. */
}

void LargeMap_constructPage( PreferenceGroup& group ){
	PreferencesPage page( group.createPage( "Large Maps", "Large Map Performance" ) );
	LargeMap_constructPreferences( page );
}

void LargeMap_registerPreferencesPage(){
	PreferencesDialog_addSettingsPage( makeCallbackF( LargeMap_constructPage ) );
}

void LargeMap_Construct(){
	GlobalPreferenceSystem().registerPreference(
	    "LargeMapDeferEntityList",
	    makeBoolStringImportCallback( LatchedAssignCaller( g_largemap_deferEntityList ) ),
	    BoolExportStringCaller( g_largemap_deferEntityList.m_latched )
	);
	GlobalPreferenceSystem().registerPreference(
	    "LargeMapSpatialIndex",
	    makeBoolStringImportCallback( LatchedAssignCaller( g_largemap_spatialIndex ) ),
	    BoolExportStringCaller( g_largemap_spatialIndex.m_latched )
	);
	GlobalPreferenceSystem().registerPreference(
	    "LargeMapShareShaderNames",
	    makeBoolStringImportCallback( LatchedAssignCaller( g_largemap_shareShaderNames ) ),
	    BoolExportStringCaller( g_largemap_shareShaderNames.m_latched )
	);
	GlobalPreferenceSystem().registerPreference(
	    "LargeMapPoolFaces",
	    makeBoolStringImportCallback( LatchedAssignCaller( g_largemap_poolFaces ) ),
	    BoolExportStringCaller( g_largemap_poolFaces.m_latched )
	);
	GlobalPreferenceSystem().registerPreference(
	    "LargeMapIncrementalBounds",
	    makeBoolStringImportCallback( LatchedAssignCaller( g_largemap_incrementalBounds ) ),
	    BoolExportStringCaller( g_largemap_incrementalBounds.m_latched )
	);
	GlobalPreferenceSystem().registerPreference(
	    "LargeMapStaticBatch",
	    makeBoolStringImportCallback( LatchedAssignCaller( g_largemap_staticBatch ) ),
	    BoolExportStringCaller( g_largemap_staticBatch.m_latched )
	);
	GlobalPreferenceSystem().registerPreference(
	    "LargeMapFastParse",
	    makeBoolStringImportCallback( LatchedAssignCaller( g_largemap_fastParse ) ),
	    BoolExportStringCaller( g_largemap_fastParse.m_latched )
	);
	GlobalPreferenceSystem().registerPreference(
	    "LargeMapCullGrid",
	    BoolImportStringCaller( g_largemap_cullGrid ),
	    BoolExportStringCaller( g_largemap_cullGrid )
	);
	GlobalPreferenceSystem().registerPreference(
	    "LargeMapTJGrid",
	    BoolImportStringCaller( g_largemap_tjGrid ),
	    BoolExportStringCaller( g_largemap_tjGrid )
	);

	/* The tokeniser lives in libs/ and cannot see this header, so the latched
	   value is pushed to it once, here, after preferences have been read. */
	g_scriptTokeniser_fastPath = g_largemap_fastParse.m_value;

	LargeMap_registerPreferencesPage();
}
