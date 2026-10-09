/* Direct-light reuse requires unchanged compiled geometry and visibility. */
#pragma once
#include <chrono>
#include <cstdint>
#include <vector>

inline bool incrementalLighting;
inline bool lightCacheRequested;
inline bool lightCacheActive;
inline bool incrementalLightingDebug;

template<typename Function>
void LightCacheTimed( const char *name, Function&& function ) {
	const auto start = std::chrono::steady_clock::now();
	function();
	if ( lightCacheRequested )
		Sys_Printf( "Lighting time: %s %.2f seconds\n", name,
			std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count() );
}

void LightCacheSettings( Args& args );
void LightCacheBegin( const char *mapName );
void LightCacheGeometry();
void LightCacheObserveScripts( bool enabled );
void LightCachePrepare();
bool LightCacheReusable();
const char *LightCacheTraceFile();
std::uint64_t LightCacheTraceKey();
bool LightCacheTraceEligible();
void LightCacheTraceInputs();
std::uint64_t LightCacheTraceInputKey();
bool LightCacheLoadSurfaceGroups( std::vector<std::vector<int>>& groups );
void LightCacheSaveSurfaceGroups( const std::vector<std::vector<int>>& groups );
void LightCacheGridDependencies();
int LightCacheGridWorkCount();
int LightCacheGridWorkIndex( int i );
bool LightCacheRestoreGrid( int num );
void LightCacheSurfaceDependencies();
bool LightCacheRestoreLightmap( int num );
void LightCacheCapture();
int LightCachePageCount();
bool LightCacheRestoreOutputPage( int page, Vector3b *pixels );
bool LightCacheRestoreOutputLightmap( rawLightmap_t *lm );
bool LightCacheRestoreOutputChannel( rawLightmap_t *lm, int channel );
void LightCacheClearDirtyOutputRegions( Vector3b *const *pages );
bool LightCacheRestoreVertexColors( int surface );
bool LightCachePlacement( const rawLightmap_t *lm, int channel, int& page, int& x, int& y );
void LightCacheFinish();
