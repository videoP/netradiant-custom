/*
   Large map performance options. See largemap.h.
 */

#include "largemap.h"

#include "stringio.h"
#include "preferencesystem.h"

#include <QCheckBox>

LatchedBool g_largemap_deferEntityList( false, "Deferred Entity List population" );
LatchedBool g_largemap_spatialIndex( false, "Spatial index for view culling" );


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

	LargeMap_registerPreferencesPage();
}
