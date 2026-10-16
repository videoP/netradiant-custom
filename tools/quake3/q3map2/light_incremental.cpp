/*
   Cache direct lighting before stitching/subsampling. Dependencies contain the
   ordered light states that could reach a receiver, including lights occluded
   by geometry. A changed/removed light therefore invalidates its old receivers
   as well as its new ones, without depending on entity array indices.

   The geometry digest deliberately forbids rematching changed geometry in
   this version. BSP output lighting is excluded: the sidecar owns reusable
   samples, atlas placements and the preview. Source geometry, compiler,
   options, scripts and texture pixels are also checked.
 */
#include "q3map2.h"
#include "light_incremental.h"
#include "bspfile_rbsp.h"
#include "lightcacheformat.h"
#include "lightcachecompression.h"
#include "surface_extra.h"

#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <unordered_map>

#ifdef WIN32
#include <windows.h>
#elif defined( __APPLE__ )
#include <mach-o/dyld.h>
#endif

namespace {
using Digest = uint64_t;
struct Hash {
	Digest value = 14695981039346656037ULL;
	void bytes( const void *data, size_t size ) {
		for ( const byte *p = static_cast<const byte*>( data ); size; --size, ++p )
			value = ( value ^ *p ) * 1099511628211ULL;
	}
	template<typename T> void add( const T& value ) { bytes( &value, sizeof( value ) ); }
	void text( const char *text ) { bytes( text, strlen( text ) + 1 ); }
};

bool fileHash( const char *name, Digest& digest ) {
	std::ifstream file( name, std::ios::binary );
	if ( !file ) return false;
	Hash hash;
	char buffer[65536];
	while ( file.read( buffer, sizeof( buffer ) ) || file.gcount() )
		hash.bytes( buffer, size_t( file.gcount() ) );
	if ( !file.eof() ) return false;
	digest = hash.value;
	return true;
}

using Header = LightCacheHeader;
struct Chart {
	Digest dependencies{};
	int w{}, h{}, sw{}, sh{}, used{};
	Array4<byte> styles{ LS_NORMAL, LS_NONE, LS_NONE, LS_NONE };
	Array4<byte> solid{};
	Array4<int> pages{ -1, -1, -1, -1 }, x{}, y{};
	Array4<std::vector<SuperLuxel>> luxels;
	std::vector<int> clusters;
};
struct Grid {
	Digest dependencies{};
	bspGridPoint_t point{};
};

Hash settings, resources;
bool compilerKnown, reusable;
Header current;
std::string cacheName;
std::string traceCacheName;
std::vector<Chart> charts;
std::vector<Grid> grid;
std::vector<byte> dirtyCharts, dirtyGrid;
std::vector<Digest> chartDependencies, gridDependencies;
std::vector<byte> cachedOutputPages;
std::vector<Array4<Color4b>> cachedVertexColors;
int oldPages;
Digest loadedPayload;
const char *cacheReason = "cache missing or unreadable";

Digest headerChecksum( Header header ) {
	return LightCacheHeaderChecksum( header );
}

void observeScript( const char *name, const void *data, size_t size ) {
	// Surface extras are hashed after parsing. BSP rebuilds can change comments
	// and number formatting without changing any lighting input.
	if ( path_extension_is( name, "srf" ) ) return;
	resources.text( name );
	resources.add( size );
	resources.bytes( data, size );
}

void sourceToken( Hash& hash, const char *token ) {
	char *end;
	const double number = strtod( token, &end );
	const bool numeric = end != token && *end == '\0' && std::isfinite( number );
	hash.add( numeric );
	if ( numeric ) {
		// Geometry/texture samples are ultimately floats in the BSP. Radiant
		// can expand a six-decimal number to its exact float on save. Preserve
		// large integer content flags without rounding off individual bits.
		const double normalized = std::abs( number ) <= 16777216.? double( float( number ) ) : number;
		hash.add( normalized == 0? 0. : normalized );
	}
	else hash.text( token );
}

void entityValue( Hash& hash, const std::string& key, const std::string& value ) {
	// Entity names, targets and other strings must retain their exact spelling.
	// Spatial keys are floats: editor decimal/whitespace changes do not move
	// an entity and must not force a BSP rebuild when only a light was edited.
	const int components = striEqual( key.c_str(), "origin" ) || striEqual( key.c_str(), "angles" )? 3
	                     : striEqual( key.c_str(), "angle" )? 1 : 0;
	std::array<float, 3> numbers{};
	const char *cursor = value.c_str();
	bool numeric = components != 0;
	for ( int i = 0; numeric && i < components; ++i ) {
		char *end;
		numbers[i] = strtof( cursor, &end );
		numeric = end != cursor && std::isfinite( numbers[i] );
		cursor = end;
	}
	while ( *cursor && std::isspace( static_cast<unsigned char>( *cursor ) ) ) ++cursor;
	numeric = numeric && *cursor == '\0';
	hash.add( numeric );
	if ( numeric ) {
		for ( int i = 0; i < components; ++i ) hash.add( numbers[i] == 0? 0.f : numbers[i] );
	}
	else hash.text( value.c_str() );
}

void requireBSPUpdate() {
	Error( "Source geometry or non-light entities changed. Rebuild BSP/VIS for geometry edits; "
	       "for entity-only edits, run -bsp -onlyents before lighting." );
}

Digest sourceGeometry( const char *name ) {
	if ( !LoadScriptFile( name, -1, false ) )
		Error( "Incremental lighting needs the source map: %s", name );
	Hash result;
	while ( GetToken( true ) ) {
		if ( !strEqual( token, "{" ) )
			Error( "Invalid entity in incremental source check: %s", name );
		Hash entity;
		std::vector<std::pair<std::string, std::string>> epairs;
		bool primitive = false, isLight = false;
		bool closed = false;
		while ( GetToken( true ) ) {
			if ( strEqual( token, "}" ) ) { closed = true; break; }
			if ( strEqual( token, "{" ) ) {
				primitive = true;
				sourceToken( entity, token );
				int depth = 1;
				while ( depth && GetToken( true ) ) {
					sourceToken( entity, token );
					if ( strEqual( token, "{" ) ) ++depth;
					else if ( strEqual( token, "}" ) ) --depth;
				}
				if ( depth ) Error( "Incomplete map primitive" );
			}
			else {
				const std::string key = token;
				const bool classname = striEqual( token, "classname" );
				if ( !GetToken( true ) ) Error( "Incomplete map entity" );
				if ( classname ) isLight = striEqualPrefix( token, "light" );
				epairs.emplace_back( key, token );
			}
		}
		if ( !closed ) Error( "Incomplete map entity" );
		// Key order has no meaning, and Radiant moves classname on save.
		std::stable_sort( epairs.begin(), epairs.end(), []( const auto& a, const auto& b ) { return a.first < b.first; } );
		for ( const auto& [key, value] : epairs ) { entity.text( key.c_str() ); entityValue( entity, key, value ); }
		if ( !isLight || primitive ) result.add( entity.value );
	}
	return result.value;
}

Digest lightState( const light_t& light ) {
	Hash hash;
	hash.add( light.type ); hash.add( light.flags );
	hash.add( light.origin ); hash.add( light.normal ); hash.add( light.dist );
	hash.add( light.photons ); hash.add( light.style ); hash.add( light.color );
	hash.add( light.radiusByDist ); hash.add( light.fade ); hash.add( light.angleScale );
	hash.add( light.extraDist ); hash.add( light.add ); hash.add( light.envelope );
	hash.add( light.minmax ); hash.add( light.cluster );
	hash.add( light.falloffTolerance ); hash.add( light.filterRadius ); hash.add( light.skyIndex );
	for ( const auto& point : light.w ) hash.add( point );
	if ( light.si ) hash.text( light.si->shader );
	return hash.value;
}

struct DependencyLight { const light_t *light; Digest state; };
struct CachedDependencyLight {
	Digest state;
	Vector3 origin;
	float envelope;
	int sun, reserved = 0;
};
static_assert( sizeof( CachedDependencyLight ) == 32 );
struct DependencyHeader {
	uint64_t magic = 0x3153504544434c51ULL; // QLCDEPS1
	uint64_t key{}, lighting{}, grid{}, surfaces{}, payload{}, checksum{};
};
std::vector<CachedDependencyLight> previousGridLights, previousSurfaceLights, gridLights, surfaceLights;
bool dependencyCacheLoaded;
std::vector<int> gridWork, surfaceWork;

std::vector<CachedDependencyLight> cacheDependencyLights( const std::vector<DependencyLight>& lights ) {
	std::vector<CachedDependencyLight> result;
	for ( const auto& entry : lights )
		result.push_back( { entry.state, entry.light->origin, entry.light->envelope, entry.light->type == ELightType::Sun } );
	return result;
}

bool changedDependencyLights( const std::vector<CachedDependencyLight>& before,
		const std::vector<CachedDependencyLight>& after, std::vector<CachedDependencyLight>& changed ) {
	// Match duplicate light states by occurrence, keeping the relative order of
	// surviving lights. A reorder falls back to the full dependency calculation
	// because floating-point light accumulation depends on that order.
	std::unordered_map<Digest, std::vector<size_t>> positions;
	for ( size_t i = before.size(); i > 0; --i ) positions[before[i - 1].state].push_back( i - 1 );
	std::vector<byte> matched( before.size() );
	size_t last = 0;
	bool first = true;
	for ( const auto& entry : after ) {
		auto& available = positions[entry.state];
		if ( available.empty() ) { changed.push_back( entry ); continue; }
		const size_t position = available.back(); available.pop_back();
		if ( !first && position <= last ) return false;
		first = false; last = position; matched[position] = 1;
	}
	for ( size_t i = 0; i < before.size(); ++i ) if ( !matched[i] ) changed.push_back( before[i] );
	return true;
}

bool dependencyReaches( const CachedDependencyLight& light, const MinMax& bounds ) {
	const Vector3 origin = bounds.origin();
	return light.sun || !( vector3_length( light.origin - origin ) - light.envelope -
		vector3_length( bounds.maxs - origin ) > 0 );
}
std::vector<DependencyLight> dependencyLights() {
	std::vector<DependencyLight> result;
	for ( const light_t& light : lights ) result.push_back( { &light, lightState( light ) } );
	return result;
}

// Limit dependency checks to nearby lights. The old all-lights scan performed
// twelve billion distance tests for racepack1's million grid samples alone.
// Keep the final sphere test and original light order exactly as before.
class DependencyIndex {
	using Cell = std::array<int, 3>;
	static constexpr float cellSize = 1024.f;
	std::map<Cell, std::vector<size_t>> cells;
	std::vector<size_t> global;
	float padding;
	bool scanAll;
	static Cell cell( const Vector3& point ) {
		return { int( std::floor( point[0] / cellSize ) ), int( std::floor( point[1] / cellSize ) ), int( std::floor( point[2] / cellSize ) ) };
	}
	static uint64_t count( const Cell& lo, const Cell& hi ) {
		return uint64_t( int64_t( hi[0] ) - lo[0] + 1 ) *
		       uint64_t( int64_t( hi[1] ) - lo[1] + 1 ) *
		       uint64_t( int64_t( hi[2] ) - lo[2] + 1 );
	}
public:
	const std::vector<DependencyLight> list = dependencyLights();
	explicit DependencyIndex( float padding = 0, bool scanAll = false ) : padding( padding ), scanAll( scanAll ) {
		if ( scanAll ) {
			for ( size_t i = 0; i < list.size(); ++i ) global.push_back( i );
			return;
		}
		for ( size_t i = 0; i < list.size(); ++i ) {
			const light_t& light = *list[i].light;
			const float radius = light.envelope + padding + 1.f; // conservative roundoff margin
			if ( light.type == ELightType::Sun || !std::isfinite( radius ) || radius > cellSize * 16 ) {
				global.push_back( i ); continue;
			}
			const Cell lo = cell( light.origin - Vector3( radius ) ), hi = cell( light.origin + Vector3( radius ) );
			if ( count( lo, hi ) > 4096 ) { global.push_back( i ); continue; }
			for ( int z = lo[2]; z <= hi[2]; ++z )
				for ( int y = lo[1]; y <= hi[1]; ++y )
					for ( int x = lo[0]; x <= hi[0]; ++x ) cells[{ x, y, z }].push_back( i );
		}
		// Grid samples use a single padded cell. Merge its ordered list once,
		// instead of allocating and sorting it for every sample in that cell.
		for ( auto& [cell, local] : cells ) {
			std::vector<size_t> merged;
			merged.reserve( global.size() + local.size() );
			std::merge( global.begin(), global.end(), local.begin(), local.end(), std::back_inserter( merged ) );
			local = std::move( merged );
		}
	}
	const std::vector<size_t>& candidates( const Vector3& origin, float radius, std::vector<size_t>& result ) const {
		if ( scanAll ) return global;
		if ( radius <= padding ) {
			const auto it = cells.find( cell( origin ) );
			return it == cells.end()? global : it->second;
		}
		result = global;
		const float reach = std::max( 0.f, radius - padding );
		const Cell lo = cell( origin - Vector3( reach ) ), hi = cell( origin + Vector3( reach ) );
		if ( count( lo, hi ) > 4096 ) {
			result.resize( list.size() );
			for ( size_t i = 0; i < result.size(); ++i ) result[i] = i;
			return result;
		}
		for ( int z = lo[2]; z <= hi[2]; ++z )
			for ( int y = lo[1]; y <= hi[1]; ++y )
				for ( int x = lo[0]; x <= hi[0]; ++x )
					if ( const auto it = cells.find( { x, y, z } ); it != cells.end() )
						result.insert( result.end(), it->second.begin(), it->second.end() );
		std::sort( result.begin(), result.end() );
		result.erase( std::unique( result.begin(), result.end() ), result.end() );
		return result;
	}
};

Digest receiverDependencies( const MinMax& bounds, const DependencyIndex& index, std::vector<size_t>& candidates ) {
	Hash hash;
	const Vector3 origin = bounds.origin();
	const float radius = vector3_length( bounds.maxs - origin );
	for ( const size_t i : index.candidates( origin, radius, candidates ) ) {
		const auto& entry = index.list[i];
		const light_t& light = *entry.light;
		// Match the compiler's conservative bounding-sphere test. No visibility
		// or shadow rejection: an occluded light is still a dependency.
		if ( light.type == ELightType::Sun ||
		     !( vector3_length( light.origin - origin ) - light.envelope - radius > 0 ) )
			hash.add( entry.state );
	}
	return hash.value;
}

const DependencyIndex *gridDependencyIndex;
const DependencyIndex *surfaceDependencyIndex;
void surfaceDependency( int i ) {
	thread_local std::vector<size_t> candidates;
	chartDependencies[i] = receiverDependencies( rawLightmaps[i].minmax, *surfaceDependencyIndex, candidates );
	dirtyCharts[i] = !reusable || charts[i].dependencies != chartDependencies[i];
}

void publishCacheFile( const std::string& temporary, const std::string& target ) {
	std::error_code error;
#ifdef WIN32
	if ( !MoveFileExA( temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) )
		error = std::error_code( GetLastError(), std::system_category() );
#else
	std::filesystem::rename( temporary, target, error );
#endif
	if ( error ) Sys_Warning( "Cannot publish cache %s: %s\n", target.c_str(), error.message().c_str() );
}

struct SurfaceGroupHeader {
	uint64_t magic = 0x3150554f52474c51ULL; // QLGROUP1
	uint64_t key{}, groups{}, surfaces{}, payload{}, checksum{};
};
Digest surfaceGroupKey() {
	Hash hash;
	hash.add( current.settings ); hash.add( current.geometry ); hash.add( current.map );
	hash.add( resources.value );
	hash.add( superSample ); hash.add( sampleScale ); hash.add( patchSubdivisions );
	hash.add( noLightmaps ); hash.add( lmLimitSize );
	for ( const surfaceInfo_t& info : Span( surfaceInfos, bspDrawSurfaces.size() ) ) {
		hash.text( info.si->shader );
		hash.add( info.parentSurfaceNum ); hash.add( info.entityNum );
		hash.add( info.castShadows ); hash.add( info.recvShadows );
		hash.add( info.sampleSize ); hash.add( info.ambientColor );
		hash.add( info.longestCurve ); hash.add( info.axis ); hash.add( info.minmax );
		hash.add( info.hasLightmap );
		hash.add( info.si->lmCustomWidth ); hash.add( info.si->lmCustomHeight );
		hash.add( info.si->lmBrightness ); hash.add( info.si->lmFilterRadius );
		hash.add( info.si->lmMergable ); hash.add( info.si->splotchFix );
	}
	return hash.value;
}
Digest surfaceGroupKeyAtSetup;
void gridDependency( int i ) {
	const int x = i % gridBounds[0], y = ( i / gridBounds[0] ) % gridBounds[1],
	          z = i / ( gridBounds[0] * gridBounds[1] );
	const Vector3 origin = gridMins + Vector3( x, y, z ) * gridSize;
	MinMax bounds;
	// TraceGrid can nudge a sample by half a grid cell on each axis.
	bounds.mins = origin - gridSize * 0.5f;
	bounds.maxs = origin + gridSize * 0.5f;
	thread_local std::vector<size_t> candidates;
	gridDependencies[i] = receiverDependencies( bounds, *gridDependencyIndex, candidates );
	dirtyGrid[i] = !reusable || grid[i].dependencies != gridDependencies[i];
}
void gridDependencyWork( int i ) { gridDependency( gridWork[i] ); }
void surfaceDependencyWork( int i ) { surfaceDependency( surfaceWork[i] ); }

template<typename T> bool read( std::ifstream& file, T& value ) {
	return bool( file.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) );
}
template<typename T> bool write( LightCacheWriter& writer, Hash& hash, const T *data, size_t count ) {
	const size_t size = count * sizeof( T );
	hash.bytes( data, size );
	return writer.write( data, size );
}

bool loadDependencyCache() {
	if ( !reusable ) return false;
	std::ifstream file( cacheName + ".deps", std::ios::binary | std::ios::ate );
	if ( !file ) return false;
	const auto length = file.tellg(); file.seekg( 0 );
	DependencyHeader header;
	if ( !read( file, header ) || header.magic != DependencyHeader{}.magic ||
	     header.key != LightCacheTraceKey() || header.lighting != loadedPayload ) return false;
	const auto checksum = header.checksum; header.checksum = 0;
	if ( checksum != LightCacheChecksum( &header, sizeof( header ) ) ||
	     header.grid > INT_MAX || header.surfaces > INT_MAX || length < 0 ||
	     uint64_t( length ) != sizeof( header ) + ( header.grid + header.surfaces ) * sizeof( CachedDependencyLight ) ) return false;
	std::vector<CachedDependencyLight> data( size_t( header.grid + header.surfaces ) );
	if ( !file.read( reinterpret_cast<char*>( data.data() ), data.size() * sizeof( data[0] ) ) ||
	     LightCacheChecksum( data.data(), data.size() * sizeof( data[0] ) ) != header.payload ) return false;
	for ( const auto& entry : data )
		if ( ( entry.sun != 0 && entry.sun != 1 ) || !std::isfinite( entry.envelope ) || entry.envelope < 0 ||
		     !std::isfinite( entry.origin[0] ) || !std::isfinite( entry.origin[1] ) || !std::isfinite( entry.origin[2] ) ) return false;
	previousGridLights.assign( data.begin(), data.begin() + header.grid );
	previousSurfaceLights.assign( data.begin() + header.grid, data.end() );
	return true;
}

void saveDependencyCache() {
	DependencyHeader header;
	header.key = LightCacheTraceKey(); header.lighting = current.payload;
	header.grid = gridLights.size(); header.surfaces = surfaceLights.size();
	std::vector<CachedDependencyLight> data = gridLights;
	data.insert( data.end(), surfaceLights.begin(), surfaceLights.end() );
	header.payload = LightCacheChecksum( data.data(), data.size() * sizeof( data[0] ) );
	header.checksum = LightCacheChecksum( &header, sizeof( header ) );
	const std::string target = cacheName + ".deps", temporary = target + ".tmp";
	std::ofstream file( temporary, std::ios::binary | std::ios::trunc );
	file.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
	file.write( reinterpret_cast<const char*>( data.data() ), data.size() * sizeof( data[0] ) );
	file.close();
	if ( !file ) { Sys_Warning( "Cannot write dependency cache: %s\n", temporary.c_str() ); return; }
	publishCacheFile( temporary, target );
}

bool loadCache() {
	std::ifstream file( cacheName, std::ios::binary | std::ios::ate );
	if ( !file ) return false;
	const auto length = file.tellg(); file.seekg( 0 );
	Header old;
	cacheReason = "cache header is invalid or belongs to a different format";
	if ( !read( file, old ) || old.magic != current.magic ||
	     old.checksum != headerChecksum( old ) ) return false;
	// A light-only invocation cannot repair geometry which the BSP stage has
	// never seen. Stop before writing the BSP, even if settings also changed.
	if ( old.geometry == current.geometry && old.map != current.map )
		requireBSPUpdate();
	cacheReason = "compiler or build options changed";
	if ( old.settings != current.settings ) return false;
	cacheReason = "shader, texture or surface-extra inputs changed";
	if ( old.resources != current.resources ) return false;
	cacheReason = "compiled BSP or source geometry changed";
	if ( old.geometry != current.geometry || old.map != current.map ) return false;
	cacheReason = "chart/grid layout changed or cache data is invalid";
	if ( old.charts != current.charts || old.grid != current.grid ||
	     old.pages > old.charts * MAX_LIGHTMAPS || old.pages > INT_MAX ) return false;
	if ( length < 0 || old.previewOffset < sizeof( old ) || old.previewOffset > uint64_t( length ) ||
	     old.previewSize != uint64_t( length ) - old.previewOffset ) return false;
	file.seekg( sizeof( old ) );
	LightCacheCompressedHeader samplesHeader;
	if ( !read( file, samplesHeader ) || samplesHeader.magic != LightCacheCompressedHeader{}.magic ||
	     old.previewOffset < sizeof( old ) + sizeof( samplesHeader ) ||
	     samplesHeader.storedSize != old.previewOffset - sizeof( old ) - sizeof( samplesHeader ) ) return false;
	file.seekg( sizeof( old ) );
	const bool compressed = true;
	LightCacheReader reader( file, old.previewOffset, samplesHeader.rawSize, compressed );
	if ( !reader ) return false;
	Hash payload;
	auto block = [&]( void *data, size_t size ) {
		if ( !reader.read( data, size ) ) return false;
		payload.bytes( data, size );
		return true;
	};
	// Counts come from the current BSP, never from an untrusted cache length.
	std::vector<std::vector<byte>> occupied( size_t( old.pages ),
		std::vector<byte>( size_t( g_game->lightmapSize ) * g_game->lightmapSize ) );
	for ( int i = 0; i < numRawLightmaps; ++i ) {
		Chart& chart = charts[i];
		const rawLightmap_t& lm = rawLightmaps[i];
		int dimensions[5];
		if ( !block( &chart.dependencies, sizeof( chart.dependencies ) ) ||
		     !block( dimensions, sizeof( dimensions ) ) ||
		     !block( chart.styles.data(), sizeof( chart.styles ) ) ||
		     !block( chart.solid.data(), sizeof( chart.solid ) ) ||
		     !block( chart.pages.data(), sizeof( chart.pages ) ) ||
		     !block( chart.x.data(), sizeof( chart.x ) ) ||
		     !block( chart.y.data(), sizeof( chart.y ) ) ) { cacheReason = "truncated chart header"; return false; }
		chart.w = dimensions[0]; chart.h = dimensions[1]; chart.sw = dimensions[2]; chart.sh = dimensions[3]; chart.used = dimensions[4];
		if ( chart.w != lm.w || chart.h != lm.h || chart.sw != lm.sw || chart.sh != lm.sh ) {
			Sys_Printf( "Cache chart %d dimensions changed: %dx%d (%dx%d samples) -> %dx%d (%dx%d samples)\n",
				i, chart.w, chart.h, chart.sw, chart.sh, lm.w, lm.h, lm.sw, lm.sh );
			cacheReason = "chart dimensions changed";
			return false;
		}
		if ( chart.used < 0 || chart.used > lm.w * lm.h ) { cacheReason = "invalid cached chart size"; return false; }
		std::set<byte> seenStyles;
		for ( int channel = 0; channel < MAX_LIGHTMAPS; ++channel ) {
			if ( chart.styles[channel] == LS_NONE ) {
				if ( chart.pages[channel] != -1 ) { cacheReason = "invalid unused style placement"; return false; }
				continue;
			}
			if ( !style_is_valid( chart.styles[channel] ) || chart.solid[channel] > 1 || !seenStyles.insert( chart.styles[channel] ).second ) {
				cacheReason = "invalid cached style"; return false;
			}
			// Vertex-lit and twinned channels have no independent atlas rectangle.
			const int width = chart.solid[channel] ? 1 : lm.w;
			const int height = chart.solid[channel] ? 1 : lm.h;
			if ( chart.pages[channel] != -1 ) {
				if ( chart.pages[channel] < 0 || uint64_t( chart.pages[channel] ) >= old.pages || chart.x[channel] < 0 || chart.y[channel] < 0 ||
				     chart.x[channel] > g_game->lightmapSize - width || chart.y[channel] > g_game->lightmapSize - height ) {
					cacheReason = "invalid cached style or chart placement"; return false;
				}
				for ( int y = 0; y < height; ++y ) for ( int x = 0; x < width; ++x ) {
					byte& used = occupied[chart.pages[channel]][( chart.y[channel] + y ) * g_game->lightmapSize + chart.x[channel] + x];
					if ( used ) { cacheReason = "overlapping cached chart placements"; return false; }
					used = 1;
				}
			}
			chart.luxels[channel].resize( size_t( lm.sw ) * lm.sh );
			if ( !block( chart.luxels[channel].data(), chart.luxels[channel].size() * sizeof( SuperLuxel ) ) ) {
				cacheReason = "truncated chart samples"; return false;
			}
		}
		chart.clusters.resize( size_t( lm.sw ) * lm.sh );
		if ( !block( chart.clusters.data(), chart.clusters.size() * sizeof( int ) ) ) { cacheReason = "truncated chart clusters"; return false; }
	}
	if ( !block( grid.data(), grid.size() * sizeof( Grid ) ) ) { cacheReason = "truncated grid samples"; return false; }
	cachedVertexColors.resize( bspDrawVerts.size() );
	if ( !block( cachedVertexColors.data(), cachedVertexColors.size() * sizeof( cachedVertexColors[0] ) ) ) {
		cacheReason = "truncated vertex colors"; return false;
	}
	if ( !reader.finish() || uint64_t( file.tellg() ) != old.previewOffset ) {
		cacheReason = "invalid compressed samples or preview offset"; return false;
	}
	uint64_t previewBytes = old.previewSize;
	if ( compressed ) {
		LightCacheCompressedHeader section;
		if ( !read( file, section ) ) return false;
		previewBytes = section.rawSize;
		file.seekg( old.previewOffset );
	}
	if ( previewBytes > INT_MAX ) return false;
	LightCacheReader previewReader( file, uint64_t( length ), previewBytes, compressed );
	if ( !previewReader ) return false;
	Hash previewHash;
	auto previewBlock = [&]( void *data, size_t size ) {
		if ( !previewReader.read( data, size ) ) return false;
		payload.bytes( data, size ); previewHash.bytes( data, size );
		return true;
	};
	// The validated preview already contains the finished output pages. Use
	// those bytes instead of trusting the lighting currently stored in the BSP.
	uint32_t previewHeader[7];
	const size_t pageBytes = size_t( g_game->lightmapSize ) * g_game->lightmapSize * 3;
	if ( previewBytes < sizeof( previewHeader ) || !previewBlock( previewHeader, sizeof( previewHeader ) ) ||
	     ( memcmp( previewHeader, "LSN1", 4 ) && memcmp( previewHeader, "LSN2", 4 ) ) || previewHeader[3] != uint32_t( g_game->lightmapSize ) ||
	     previewHeader[4] != old.pages || previewBytes - sizeof( previewHeader ) < old.pages * pageBytes ) {
		cacheReason = "invalid cached output pages"; return false;
	}
	cachedOutputPages.resize( size_t( old.pages ) * pageBytes );
	if ( !previewBlock( cachedOutputPages.data(), cachedOutputPages.size() ) ) { cacheReason = "truncated output pages"; return false; }
	char buffer[65536];
	for ( uint64_t remaining = previewBytes - sizeof( previewHeader ) - cachedOutputPages.size(); remaining; ) {
		const size_t size = std::min<uint64_t>( remaining, sizeof( buffer ) );
		if ( !previewBlock( buffer, size ) ) { cacheReason = "truncated preview"; return false; }
		remaining -= size;
	}
	if ( !previewReader.finish() || previewHash.value != old.previewChecksum || payload.value != old.payload || file.peek() != EOF ) {
		cacheReason = "cache payload checksum or length mismatch"; return false;
	}
	oldPages = int( old.pages );
	loadedPayload = old.payload;
	return true;
}
}

void LightCacheSettings( Args& args ) {
	// Capture general/game/resource options before argument parsing removes them.
	const auto arguments = args.getVector();
	if ( std::ranges::none_of( arguments, []( const char *arg ) {
		return striEqual( arg, "-incremental" ) || striEqual( arg, "-lightcache" );
	} ) ) return;
	for ( size_t i = 0; i < arguments.size(); ++i ) {
		const char *arg = arguments[i];
		if ( striEqual( arg, "-incremental" ) || striEqual( arg, "-lightcache" ) || striEqual( arg, "-incrementaldebug" ) ) continue;
		if ( striEqual( arg, "-connect" ) ) { ++i; continue; }
		if ( striEqual( arg, "-v" ) ) continue;
		settings.text( arg );
	}
	Digest compiler;
	// argv[0] can be only a basename when launched through PATH.
#ifdef WIN32
	char executable[32768];
	const DWORD length = GetModuleFileNameA( nullptr, executable, sizeof( executable ) );
	compilerKnown = length > 0 && length < sizeof( executable ) && fileHash( executable, compiler );
#elif defined( __linux__ )
	compilerKnown = fileHash( "/proc/self/exe", compiler );
#elif defined( __APPLE__ )
	char executable[4096];
	uint32_t length = sizeof( executable );
	compilerKnown = _NSGetExecutablePath( executable, &length ) == 0 && fileHash( executable, compiler );
#else
	compilerKnown = fileHash( args.getArg0(), compiler );
#endif
	if ( compilerKnown ) settings.add( compiler );
}

void LightCacheObserveScripts( bool enabled ) {
	if ( lightCacheRequested ) scriptLoadObserver = enabled? observeScript : nullptr;
}

void LightCacheBegin( const char *mapName ) {
	if ( !lightCacheRequested ) return;
	Sys_Printf( "--- Incremental Light Build ---\n" );
	current.settings = settings.value;
	std::filesystem::path map( ExpandArg( mapName ) );
	if ( !path_extension_is( mapName, "reg" ) ) map.replace_extension( ".map" );
	current.map = sourceGeometry( map.string().c_str() );
	cacheName = std::filesystem::path( source ).replace_extension( ".lightcache" ).string();
	traceCacheName = cacheName + ".trace";
	LightCacheObserveScripts( true );
}

void LightCacheGeometry() {
	if ( !lightCacheRequested ) return;
	// The BSP supplies geometry, collision and visibility. Its output lighting
	// is not an input: samples, chart placements and preview live in .lightcache.
	Hash hash;
	// Version the digest rules so a cache from an older map/entity check
	// falls back cleanly instead of reporting an unbuilt geometry edit.
	hash.text( "lightcache canonical geometry 3" );
	const auto lump = [&hash]( const auto& values ) {
		hash.add( values.size() );
		hash.bytes( values.data(), values.size() * sizeof( values[0] ) );
	};
	lump( bspModels ); lump( bspPlanes ); lump( bspNodes ); lump( bspLeafs );
	lump( bspLeafSurfaces ); lump( bspLeafBrushes ); lump( bspBrushes );
	lump( bspBrushSides ); lump( bspDrawIndexes ); lump( bspVisBytes ); lump( bspFogs );
	for ( const auto& shader : bspShaders ) {
		hash.text( shader.shader ); hash.add( shader.surfaceFlags ); hash.add( shader.contentFlags );
	}
	for ( const auto& ds : bspDrawSurfaces ) {
		hash.add( ds.shaderNum ); hash.add( ds.fogNum ); hash.add( ds.surfaceType );
		hash.add( ds.firstVert ); hash.add( ds.numVerts ); hash.add( ds.firstIndex ); hash.add( ds.numIndexes );
		hash.add( ds.lightmapVecs ); hash.add( ds.patchWidth ); hash.add( ds.patchHeight );
	}
	for ( const auto& vertex : bspDrawVerts ) {
		hash.add( vertex.xyz ); hash.add( vertex.st ); hash.add( vertex.normal );
		for ( const auto& color : vertex.color ) hash.add( color[3] );
	}
	for ( const auto& entity : entities ) {
		if ( striEqualPrefix( entity.classname(), "light" ) ) continue;
		hash.text( "entity" );
		std::vector<std::pair<std::string, std::string>> epairs;
		for ( const auto& ep : entity.epairs ) {
			if ( striEqualPrefix( ep.key.c_str(), "_q3map2_" ) || striEqual( ep.key.c_str(), "gridsize" ) ) continue;
			epairs.emplace_back( ep.key.c_str(), ep.value.c_str() );
		}
		std::stable_sort( epairs.begin(), epairs.end(), []( const auto& a, const auto& b ) { return a.first < b.first; } );
		for ( const auto& [key, value] : epairs ) { hash.text( key.c_str() ); entityValue( hash, key, value ); }
	}
	current.geometry = hash.value;
	std::ifstream file( cacheName, std::ios::binary );
	Header old;
	if ( read( file, old ) && old.magic == current.magic && old.checksum == headerChecksum( old ) &&
	     old.geometry == current.geometry && old.map != current.map )
		requireBSPUpdate();
}

void LightCacheRefreshEntityLights() {
	if ( !lightCacheRequested ) return;
	const bool keep = entities[0].boolForKey( "_keepLights" );
	const bool switched = g_game->load == LoadRBSPFile;
	if ( !keep && !switched ) return;
	std::vector<size_t> compiled, sourceLights;
	for ( size_t i = 1; i < entities.size(); ++i ) {
		const entity_t& entity = entities[i];
		if ( !entity.classname_prefixed( "light" ) ) continue;
		if ( i < size_t( numBSPEntities ) ) compiled.push_back( i );
		else if ( keep || ( switched && !strEmpty( entity.valueForKey( "targetname" ) ) ) )
			sourceLights.push_back( i );
	}
	// Preserve BSP entity indices and generated switchable styles. A move or
	// intensity/color edit needs only fresh epairs, not a new BSP/VIS tree.
	// Changed style groups require the normal entity update to assign styles.
	if ( compiled.size() != sourceLights.size() )
		Error( "BSP-stored lights were added or removed. Run -bsp -onlyents before lighting." );
	for ( size_t i = 0; i < compiled.size(); ++i ) {
		const entity_t& before = entities[compiled[i]];
		const entity_t& after = entities[sourceLights[i]];
		const bool named = switched && !strEmpty( before.valueForKey( "targetname" ) );
		const int sourceStyle = before.intForKey( named? "switch_style" : "style" );
		if ( !strEqual( before.valueForKey( "targetname" ), after.valueForKey( "targetname" ) ) ||
		     sourceStyle != after.intForKey( "style" ) )
			Error( "BSP-stored light style groups changed. Run -bsp -onlyents before lighting." );
	}
	for ( size_t i = 0; i < compiled.size(); ++i ) {
		entity_t& before = entities[compiled[i]];
		entity_t& after = entities[sourceLights[i]];
		const bool named = switched && !strEmpty( before.valueForKey( "targetname" ) );
		const int style = before.intForKey( "style" );
		const int switchStyle = before.intForKey( "switch_style" );
		before.epairs = std::move( after.epairs );
		if ( named ) {
			before.setKeyValue( "style", style );
			if ( switchStyle != LS_NORMAL ) before.setKeyValue( "switch_style", switchStyle );
		}
		// The compiled entity is authoritative for game output and lighting;
		// discard the map copy so it cannot contribute a second shadow model.
		after.epairs.clear();
	}
	if ( !compiled.empty() )
		Sys_Printf( "Refreshed %zu BSP-stored light entities from source (styles preserved).\n", compiled.size() );
}

void LightCachePrepare() {
	if ( !lightCacheRequested ) return;
	LightCacheObserveScripts( false );
	const char *unsupported = nullptr;
	if ( !compilerKnown ) unsupported = "compiler executable could not be fingerprinted";
	else if ( bounce > 0 ) unsupported = "bounce lighting requires a full build";
	else if ( deluxemap || externalLightmaps || approximateTolerance > 0 || lightmapFill || trisoup )
		unsupported = "deluxe/external/approximated/filled/trisoup lightmaps require a full build";
	else if ( dirty || floodlighty || lightRandomSamples )
		unsupported = "dirt, floodlight or random samples require a full build";
	for ( const rawLightmap_t& lm : Span( rawLightmaps, numRawLightmaps ) ) {
		if ( lm.customWidth != g_game->lightmapSize || lm.customHeight != g_game->lightmapSize )
			unsupported = "custom lightmap pages require a full build";
		if ( lm.floodlightIntensity != 0 ) unsupported = "surface floodlight requires a full build";
	}
	if ( unsupported ) {
		Sys_Printf( "Incremental fallback: %s.\n", unsupported );
		return;
	}
	// Hash decoded texture contents as well as all loaded scripts, including
	// includes, custom surface parameters and the surface-extra file.
	if ( incrementalLightingDebug ) Sys_Printf( "Cache script digest: %016llx\n", (unsigned long long) resources.value );
	for ( int i = -1; i < int( bspDrawSurfaces.size() ); ++i ) {
		const surfaceExtra_t& extra = GetSurfaceExtra( i );
		resources.text( extra.si? extra.si->shader.c_str() : "" );
		resources.add( extra.parentSurfaceNum ); resources.add( extra.entityNum );
		resources.add( extra.castShadows ); resources.add( extra.recvShadows );
		resources.add( extra.sampleSize ); resources.add( extra.ambientColor );
		resources.add( extra.longestCurve ); resources.add( extra.lightmapAxis );
	}
	std::set<const image_t*> images;
	if ( incrementalLightingDebug ) Sys_Printf( "Cache surface-extra digest: %016llx\n", (unsigned long long) resources.value );
	for ( const shaderInfo_t& si : shaderInfo )
		for ( const image_t *image : { si.shaderImage, si.lightImage, si.normalImage } )
			if ( image ) images.insert( image );
	std::vector<const image_t*> orderedImages( images.begin(), images.end() );
	std::sort( orderedImages.begin(), orderedImages.end(), []( const image_t *a, const image_t *b ) {
		return strcmp( a->name.c_str(), b->name.c_str() ) < 0;
	} );
	for ( const image_t *image : orderedImages ) {
		resources.text( image->name.c_str() );
		resources.add( image->width ); resources.add( image->height );
		resources.bytes( image->pixels, size_t( image->width ) * image->height * 4 );
	}
	resources.add( LightCacheTraceInputKey() );
	resources.add( gridSize ); resources.add( gridMins ); resources.add( gridBounds );
	if ( incrementalLightingDebug ) Sys_Printf( "Cache resource digest: %016llx\n", (unsigned long long) resources.value );
	current.resources = resources.value;
	current.charts = numRawLightmaps;
	current.grid = rawGridPoints.size();
	charts.resize( numRawLightmaps ); grid.resize( rawGridPoints.size() );
	dirtyCharts.resize( charts.size(), 1 ); dirtyGrid.resize( grid.size(), 1 );
	chartDependencies.resize( charts.size() ); gridDependencies.resize( grid.size() );
	const bool loaded = incrementalLighting && loadCache();
	reusable = incrementalLighting && loaded;
	dependencyCacheLoaded = loadDependencyCache();
	lightCacheActive = true;
	noCollapse = true; // independent rectangular charts cannot share solid/twin slots
	Sys_Printf( "Cache: %s\n", reusable? "valid; preserving atlas allocations" :
		"full lighting and fresh atlas" );
	if ( !reusable ) Sys_Printf( "Incremental fallback: %s.\n", incrementalLighting? cacheReason : "explicit full cache refresh" );
}

bool LightCacheReusable() { return reusable; }

bool LightCacheLoadSurfaceGroups( std::vector<std::vector<int>>& groups ) {
	if ( !lightCacheRequested ) return false;
	surfaceGroupKeyAtSetup = surfaceGroupKey();
	if ( !incrementalLighting ) return false;
	std::ifstream file( cacheName + ".groups", std::ios::binary | std::ios::ate );
	if ( !file ) return false;
	const auto length = file.tellg();
	file.seekg( 0 );
	SurfaceGroupHeader header;
	if ( !read( file, header ) || header.magic != SurfaceGroupHeader{}.magic ||
	     header.key != surfaceGroupKeyAtSetup ) return false;
	const auto checksum = header.checksum;
	header.checksum = 0;
	if ( checksum != LightCacheChecksum( &header, sizeof( header ) ) ||
	     header.groups > bspDrawSurfaces.size() || header.surfaces > bspDrawSurfaces.size() ||
	     header.groups > header.surfaces || length < 0 ||
	     uint64_t( length ) != sizeof( header ) + ( header.groups + header.surfaces ) * sizeof( int ) ) return false;
	std::vector<int> data( size_t( header.groups + header.surfaces ) );
	if ( !file.read( reinterpret_cast<char*>( data.data() ), data.size() * sizeof( int ) ) ||
	     LightCacheChecksum( data.data(), data.size() * sizeof( int ) ) != header.payload ) return false;
	std::vector<byte> seen( bspDrawSurfaces.size() );
	std::vector<std::vector<int>> loaded;
	size_t position = 0;
	for ( uint64_t i = 0; i < header.groups; ++i ) {
		if ( position >= data.size() ) return false;
		const int count = data[position++];
		if ( count <= 0 || size_t( count ) > data.size() - position ) return false;
		const int seed = data[position];
		if ( seed < 0 || size_t( seed ) >= seen.size() || surfaceInfos[seed].parentSurfaceNum >= 0 ) return false;
		for ( int j = 0; j < count; ++j ) {
			const int surface = data[position + j];
			if ( surface < 0 || size_t( surface ) >= seen.size() || seen[surface] || !surfaceInfos[surface].hasLightmap ) return false;
			seen[surface] = 1;
		}
		loaded.emplace_back( data.begin() + position, data.begin() + position + count );
		position += count;
	}
	if ( position != data.size() ) return false;
	for ( size_t i = 0; i < seen.size(); ++i )
		if ( surfaceInfos[i].hasLightmap && surfaceInfos[i].parentSurfaceNum < 0 && !seen[i] ) return false;
	groups = std::move( loaded );
	return true;
}

void LightCacheSaveSurfaceGroups( const std::vector<std::vector<int>>& groups ) {
	if ( !lightCacheRequested ) return;
	SurfaceGroupHeader header;
	header.key = surfaceGroupKeyAtSetup;
	header.groups = groups.size();
	std::vector<int> data;
	for ( const auto& group : groups ) {
		header.surfaces += group.size();
		data.push_back( int( group.size() ) );
		data.insert( data.end(), group.begin(), group.end() );
	}
	header.payload = LightCacheChecksum( data.data(), data.size() * sizeof( int ) );
	header.checksum = LightCacheChecksum( &header, sizeof( header ) );
	const std::string target = cacheName + ".groups", temporary = target + ".tmp";
	std::ofstream file( temporary, std::ios::binary | std::ios::trunc );
	file.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
	file.write( reinterpret_cast<const char*>( data.data() ), data.size() * sizeof( int ) );
	file.close();
	if ( !file ) { Sys_Warning( "Cannot write surface group cache: %s\n", temporary.c_str() ); return; }
	publishCacheFile( temporary, target );
}

const char *LightCacheTraceFile() { return traceCacheName.c_str(); }
std::uint64_t LightCacheTraceKey() {
	Hash hash;
	hash.add( current.settings ); hash.add( current.resources );
	hash.add( current.geometry ); hash.add( current.map );
	return hash.value;
}

void LightCacheGridDependencies() {
	if ( !lightCacheActive ) return;
	const auto start = std::chrono::steady_clock::now();
	gridLights = cacheDependencyLights( dependencyLights() );
	std::vector<CachedDependencyLight> changed;
	const bool selective = dependencyCacheLoaded && changedDependencyLights( previousGridLights, gridLights, changed );
	std::fill( dirtyGrid.begin(), dirtyGrid.end(), byte( !selective ) );
	if ( selective ) {
		for ( size_t i = 0; i < grid.size(); ++i ) gridDependencies[i] = grid[i].dependencies;
		const float padding = vector3_length( gridSize * 0.5f ) + 1.f;
		for ( const auto& light : changed ) {
			if ( grid.empty() ) break;
			const double radius = double( light.envelope ) + padding;
			if ( light.sun || !std::isfinite( radius ) || !std::isfinite( light.origin[0] ) ||
			     !std::isfinite( light.origin[1] ) || !std::isfinite( light.origin[2] ) ) {
				std::fill( dirtyGrid.begin(), dirtyGrid.end(), byte( 1 ) ); break;
			}
			int lo[3], hi[3];
			for ( int axis = 0; axis < 3; ++axis ) {
				lo[axis] = int( std::clamp( std::ceil( ( double( light.origin[axis] ) - radius - gridMins[axis] ) / gridSize[axis] ), 0., double( gridBounds[axis] - 1 ) ) );
				hi[axis] = int( std::clamp( std::floor( ( double( light.origin[axis] ) + radius - gridMins[axis] ) / gridSize[axis] ), 0., double( gridBounds[axis] - 1 ) ) );
			}
			for ( int z = lo[2]; z <= hi[2]; ++z ) for ( int y = lo[1]; y <= hi[1]; ++y ) for ( int x = lo[0]; x <= hi[0]; ++x )
				dirtyGrid[( z * gridBounds[1] + y ) * gridBounds[0] + x] = 1;
		}
	}
	gridWork.clear();
	for ( size_t i = 0; i < grid.size(); ++i ) if ( dirtyGrid[i] ) gridWork.push_back( int( i ) );
	if ( !gridWork.empty() ) {
		const DependencyIndex index( vector3_length( gridSize * 0.5f ) + 1.f,
			gridWork.size() <= 4000000 / std::max<size_t>( 1, gridLights.size() ) );
		gridDependencyIndex = &index;
		RunThreadsOnIndividual( gridWork.size(), false, gridDependencyWork );
		gridDependencyIndex = nullptr;
	}
	Sys_Printf( "Dependency cache: %zu grid samples checked%s\n", gridWork.size(), selective? " in changed light regions" : "" );
	gridWork.clear();
	for ( size_t i = 0; i < grid.size(); ++i ) {
		if ( dirtyGrid[i] ) gridWork.push_back( int( i ) );
		else bspGridPoints[i] = grid[i].point;
	}
	const size_t reused = std::count( dirtyGrid.begin(), dirtyGrid.end(), byte( 0 ) );
	Sys_Printf( "Lightgrid: %zu reused, %zu relit\n", reused, grid.size() - reused );
	Sys_Printf( "Grid cache dependency checks: %.2f seconds\n", std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count() );
}

int LightCacheGridWorkCount() { return lightCacheActive? int( gridWork.size() ) : int( rawGridPoints.size() ); }
int LightCacheGridWorkIndex( int i ) { return lightCacheActive? gridWork[i] : i; }

bool LightCacheRestoreGrid( int num ) {
	if ( !lightCacheActive || dirtyGrid[num] ) return false;
	bspGridPoints[num] = grid[num].point;
	return true;
}

void LightCacheSurfaceDependencies() {
	if ( !lightCacheActive ) return;
	surfaceLights = cacheDependencyLights( dependencyLights() );
	std::vector<CachedDependencyLight> changed;
	const bool selective = dependencyCacheLoaded && changedDependencyLights( previousSurfaceLights, surfaceLights, changed );
	surfaceWork.clear();
	for ( int i = 0; i < numRawLightmaps; ++i ) {
		chartDependencies[i] = charts[i].dependencies;
		dirtyCharts[i] = !selective || std::ranges::any_of( changed, [&]( const auto& light ) {
			return dependencyReaches( light, rawLightmaps[i].minmax );
		} );
		if ( dirtyCharts[i] ) surfaceWork.push_back( i );
	}
	if ( !surfaceWork.empty() ) {
		const DependencyIndex index( 0, surfaceWork.size() <= 4000000 / std::max<size_t>( 1, surfaceLights.size() ) );
		surfaceDependencyIndex = &index;
		RunThreadsOnIndividual( surfaceWork.size(), false, surfaceDependencyWork );
		surfaceDependencyIndex = nullptr;
	}
	Sys_Printf( "Dependency cache: %zu lightmaps checked%s\n", surfaceWork.size(), selective? " in changed light regions" : "" );
	size_t reused = 0, surfaces = 0;
	for ( int i = 0; i < numRawLightmaps; ++i ) {
		if ( !dirtyCharts[i] ) { ++reused; surfaces += rawLightmaps[i].numLightSurfaces; }
		else if ( reusable && incrementalLightingDebug )
			Sys_Printf( "chart %d dirty: potential light dependencies changed\n", i );
	}
	Sys_Printf( "Lightmaps: %zu reused (%zu surfaces), %zu relit; vertex lighting rebuilt\n",
		reused, surfaces, charts.size() - reused );
}

bool LightCacheRestoreLightmap( int num ) {
	if ( !lightCacheActive || dirtyCharts[num] ) return false;
	rawLightmap_t& lm = rawLightmaps[num];
	const Chart& chart = charts[num];
	lm.styles = chart.styles;
	for ( int channel = 0; channel < MAX_LIGHTMAPS; ++channel ) {
		lm.solid[channel] = chart.solid[channel] != 0;
		if ( lm.styles[channel] == LS_NONE ) continue;
		if ( lm.superLuxels[channel] == nullptr )
			lm.superLuxels[channel] = safe_malloc( chart.luxels[channel].size() * sizeof( SuperLuxel ) );
		memcpy( lm.superLuxels[channel], chart.luxels[channel].data(), chart.luxels[channel].size() * sizeof( SuperLuxel ) );
	}
	memcpy( lm.superClusters, chart.clusters.data(), chart.clusters.size() * sizeof( int ) );
	return true;
}

void LightCacheCapture() {
	if ( !lightCacheActive ) return;
	for ( int i = 0; i < numRawLightmaps; ++i ) {
		Chart& chart = charts[i];
		const rawLightmap_t& lm = rawLightmaps[i];
		chart.dependencies = chartDependencies[i];
		chart.w = lm.w; chart.h = lm.h; chart.sw = lm.sw; chart.sh = lm.sh;
		chart.styles = lm.styles;
		for ( int channel = 0; channel < MAX_LIGHTMAPS; ++channel ) {
			chart.luxels[channel].clear();
			if ( chart.styles[channel] == LS_NONE ) continue;
			chart.luxels[channel].resize( size_t( lm.sw ) * lm.sh );
			if ( lm.superLuxels[channel] != nullptr )
				memcpy( chart.luxels[channel].data(), lm.superLuxels[channel], chart.luxels[channel].size() * sizeof( SuperLuxel ) );
		}
		chart.clusters.assign( lm.superClusters, lm.superClusters + size_t( lm.sw ) * lm.sh );
	}
	for ( size_t i = 0; i < grid.size(); ++i ) {
		grid[i].dependencies = gridDependencies[i];
		grid[i].point = bspGridPoints[i];
	}
}

int LightCachePageCount() { return reusable? oldPages : 0; }

bool LightCacheRestoreOutputPage( int page, Vector3b *pixels ) {
	if ( !reusable ) return false;
	const size_t pageBytes = size_t( g_game->lightmapSize ) * g_game->lightmapSize * 3;
	memcpy( pixels, cachedOutputPages.data() + size_t( page ) * pageBytes, pageBytes );
	return true;
}

bool LightCacheRestoreOutputLightmap( rawLightmap_t *lm ) {
	if ( !reusable || dirtyCharts[lm - rawLightmaps] ) return false;
	const Chart& chart = charts[lm - rawLightmaps];
	lm->used = chart.used;
	for ( int channel = 0; channel < MAX_LIGHTMAPS; ++channel )
		lm->solid[channel] = chart.solid[channel] != 0;
	return true;
}

bool LightCacheRestoreOutputChannel( rawLightmap_t *lm, int channel ) {
	if ( !reusable || channel < 0 || channel >= MAX_LIGHTMAPS ) return false;
	const size_t index = lm - rawLightmaps;
	const Chart& chart = charts[index];
	return !dirtyCharts[index] && chart.styles[channel] == lm->styles[channel] &&
	       chart.solid[channel] == byte( lm->solid[channel] ) && chart.pages[channel] >= 0;
}

void LightCacheClearDirtyOutputRegions( Vector3b *const *pages ) {
	if ( !reusable ) return;
	for ( size_t i = 0; i < charts.size(); ++i ) {
		if ( !dirtyCharts[i] ) continue;
		const Chart& chart = charts[i];
		for ( int channel = 0; channel < MAX_LIGHTMAPS; ++channel ) {
			const int page = chart.pages[channel];
			if ( page < 0 ) continue;
			const int width = chart.solid[channel] ? 1 : chart.w;
			const int height = chart.solid[channel] ? 1 : chart.h;
			for ( int row = 0; row < height; ++row )
				std::fill_n( pages[page] + ( chart.y[channel] + row ) * g_game->lightmapSize + chart.x[channel], width, Vector3b( 0 ) );
		}
	}
}

bool LightCacheRestoreVertexColors( int surface ) {
	if ( !reusable || !surfaceInfos[surface].lm || dirtyCharts[surfaceInfos[surface].lm - rawLightmaps] ) return false;
	const bspDrawSurface_t& ds = bspDrawSurfaces[surface];
	for ( int vertex = ds.firstVert; vertex < ds.firstVert + ds.numVerts; ++vertex )
		bspDrawVerts[vertex].color = cachedVertexColors[vertex];
	return true;
}

bool LightCachePlacement( const rawLightmap_t *lm, int channel, int& page, int& x, int& y ) {
	if ( !reusable || channel < 0 || channel >= MAX_LIGHTMAPS ) return false;
	const Chart& chart = charts[lm - rawLightmaps];
	if ( chart.styles[channel] != lm->styles[channel] || chart.solid[channel] != byte( lm->solid[channel] ) || chart.pages[channel] < 0 ) return false;
	page = chart.pages[channel]; x = chart.x[channel]; y = chart.y[channel];
	return true;
}

void LightCacheFinish() {
	if ( !lightCacheActive ) return;
	current.pages = numOutLightmaps;
	const std::string temporary = cacheName + ".tmp";
	std::ofstream file( temporary, std::ios::binary | std::ios::trunc );
	file.write( reinterpret_cast<const char*>( &current ), sizeof( current ) );
	LightCacheWriter writer( file );
	Hash payload;
	for ( int i = 0; i < numRawLightmaps; ++i ) {
		Chart& chart = charts[i];
		const rawLightmap_t& lm = rawLightmaps[i];
		chart.styles = lm.styles;
		for ( int channel = 0; channel < MAX_LIGHTMAPS; ++channel ) chart.solid[channel] = lm.solid[channel] ? 1 : 0;
		for ( int channel = 0; channel < MAX_LIGHTMAPS; ++channel ) {
			chart.pages[channel] = -1;
			chart.x[channel] = chart.y[channel] = 0;
			if ( lm.styles[channel] != LS_NONE && lm.twins[channel] == nullptr && lm.outLightmapNums[channel] >= 0 ) {
				chart.pages[channel] = lm.outLightmapNums[channel];
				chart.x[channel] = lm.lightmapX[channel];
				chart.y[channel] = lm.lightmapY[channel];
			}
		}
		chart.used = lm.used;
		int dimensions[5] = { chart.w, chart.h, chart.sw, chart.sh, chart.used };
		if ( !write( writer, payload, &chart.dependencies, 1 ) ||
			!write( writer, payload, dimensions, 5 ) ||
			!write( writer, payload, chart.styles.data(), chart.styles.size() ) ||
			!write( writer, payload, chart.solid.data(), chart.solid.size() ) ||
		     !write( writer, payload, chart.pages.data(), chart.pages.size() ) ||
		     !write( writer, payload, chart.x.data(), chart.x.size() ) ||
		     !write( writer, payload, chart.y.data(), chart.y.size() ) ) file.setstate( std::ios::badbit );
		for ( int channel = 0; channel < MAX_LIGHTMAPS; ++channel )
			if ( chart.styles[channel] != LS_NONE && !write( writer, payload, chart.luxels[channel].data(), chart.luxels[channel].size() ) )
				file.setstate( std::ios::badbit );
		if ( !write( writer, payload, chart.clusters.data(), chart.clusters.size() ) ) file.setstate( std::ios::badbit );
	}
	if ( !write( writer, payload, grid.data(), grid.size() ) ) file.setstate( std::ios::badbit );
	std::vector<Array4<Color4b>> vertexColors;
	vertexColors.reserve( bspDrawVerts.size() );
	for ( const bspDrawVert_t& vertex : bspDrawVerts ) vertexColors.push_back( vertex.color );
	if ( !write( writer, payload, vertexColors.data(), vertexColors.size() ) || !writer.finish() ) file.setstate( std::ios::badbit );
	const auto preview = CreateLightSnapshot( 0, true );
	current.previewOffset = uint64_t( file.tellp() );
	current.previewChecksum = LightCacheChecksum( preview.data(), preview.size() );
	LightCacheWriter previewWriter( file );
	if ( !write( previewWriter, payload, preview.data(), preview.size() ) || !previewWriter.finish() ) file.setstate( std::ios::badbit );
	current.previewSize = uint64_t( file.tellp() ) - current.previewOffset;
	current.payload = payload.value;
	current.checksum = headerChecksum( current );
	file.seekp( 0 );
	file.write( reinterpret_cast<const char*>( &current ), sizeof( current ) );
	file.close();
	if ( !file ) { Sys_Warning( "Cannot write lighting cache: %s\n", temporary.c_str() ); return; }
	std::error_code error;
#ifdef WIN32
	if ( !MoveFileExA( temporary.c_str(), cacheName.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) )
		error = std::error_code( GetLastError(), std::system_category() );
#else
	std::filesystem::rename( temporary, cacheName, error );
#endif
	if ( error ) Sys_Warning( "Cannot publish lighting cache: %s\n", error.message().c_str() );
	else Sys_Printf( "Saved %s (%zu charts, %zu grid samples, %d fixed pages)\n",
		cacheName.c_str(), charts.size(), grid.size(), numOutLightmaps );
	if ( !error ) saveDependencyCache();
}
