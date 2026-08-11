/*
   Large map performance options. See largemap.h.
 */

#include "largemap.h"

#include "stringio.h"
#include "preferencesystem.h"

#include <QCheckBox>

LatchedBool g_largemap_deferEntityList( false, "Deferred Entity List population" );
LatchedBool g_largemap_spatialIndex( false, "Spatial index for view culling" );
LatchedBool g_largemap_staticBatch( false, "Batched static geometry" );
LatchedBool g_largemap_incrementalBounds( false, "Cached container bounds" );


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
	    "LargeMapIncrementalBounds",
	    makeBoolStringImportCallback( LatchedAssignCaller( g_largemap_incrementalBounds ) ),
	    BoolExportStringCaller( g_largemap_incrementalBounds.m_latched )
	);
	GlobalPreferenceSystem().registerPreference(
	    "LargeMapStaticBatch",
	    makeBoolStringImportCallback( LatchedAssignCaller( g_largemap_staticBatch ) ),
	    BoolExportStringCaller( g_largemap_staticBatch.m_latched )
	);

	LargeMap_registerPreferencesPage();
}
