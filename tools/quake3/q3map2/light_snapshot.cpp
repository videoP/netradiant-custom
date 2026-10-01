/*
   Light snapshot: what the editor's lit-view overlay reads.

   After the direct pass and after every bounce, -lightsnap <file> writes the
   lit surfaces and their lightmap pages to a small flat file, so a viewer can
   show each pass as it finishes instead of waiting for the whole compile.
   Patches are tessellated here, so the reader only ever sees triangles.
 */

#include "q3map2.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace
{
constexpr int c_patchSubdivisions = 8;

struct SnapVert
{
	float xyz[3];
	float st[2];
	float lm[2];
	byte color[4];
};

struct SnapSurface
{
	unsigned shader;
	int lightmapNum; // -1: by vertex colour
	std::vector<SnapVert> verts;
	std::vector<unsigned> indexes;
};

SnapVert snap_vert( const bspDrawVert_t& v ){
	SnapVert out;
	for ( int i = 0; i < 3; ++i ) {
		out.xyz[i] = v.xyz[i];
	}
	out.st[0] = v.st[0];
	out.st[1] = v.st[1];
	out.lm[0] = v.lightmap[0][0];
	out.lm[1] = v.lightmap[0][1];
	for ( int i = 0; i < 4; ++i ) {
		out.color[i] = v.color[0][i];
	}
	return out;
}

SnapVert snap_mix( const SnapVert& a, const SnapVert& b, const SnapVert& c, float wa, float wb, float wc ){
	SnapVert out;
	for ( int i = 0; i < 3; ++i ) {
		out.xyz[i] = a.xyz[i] * wa + b.xyz[i] * wb + c.xyz[i] * wc;
	}
	for ( int i = 0; i < 2; ++i ) {
		out.st[i] = a.st[i] * wa + b.st[i] * wb + c.st[i] * wc;
		out.lm[i] = a.lm[i] * wa + b.lm[i] * wb + c.lm[i] * wc;
	}
	for ( int i = 0; i < 4; ++i ) {
		out.color[i] = byte( std::clamp( a.color[i] * wa + b.color[i] * wb + c.color[i] * wc + 0.5f, 0.f, 255.f ) );
	}
	return out;
}

/// The quadratic bezier a patch is made of, one 3x3 piece at a time.
void tessellate_patch( const bspDrawSurface_t& ds, SnapSurface& out ){
	const int width = ds.patchWidth;
	const int height = ds.patchHeight;
	if ( width < 3 || height < 3 || ( width & 1 ) == 0 || ( height & 1 ) == 0 ) {
		return;
	}

	const int n = c_patchSubdivisions;
	for ( int py = 0; py + 2 < height; py += 2 ) {
		for ( int px = 0; px + 2 < width; px += 2 ) {
			SnapVert control[3][3];
			for ( int y = 0; y < 3; ++y ) {
				for ( int x = 0; x < 3; ++x ) {
					control[y][x] = snap_vert( bspDrawVerts[ ds.firstVert + ( py + y ) * width + px + x ] );
				}
			}

			const unsigned base = unsigned( out.verts.size() );
			for ( int j = 0; j <= n; ++j ) {
				const float v = float( j ) / n;
				const float bv[3] = { ( 1 - v ) * ( 1 - v ), 2 * v * ( 1 - v ), v * v };
				for ( int i = 0; i <= n; ++i ) {
					const float u = float( i ) / n;
					const float bu[3] = { ( 1 - u ) * ( 1 - u ), 2 * u * ( 1 - u ), u * u };

					SnapVert vert{};
					float colour[4] = {};
					for ( int y = 0; y < 3; ++y ) {
						for ( int x = 0; x < 3; ++x ) {
							const float w = bu[x] * bv[y];
							const SnapVert& c = control[y][x];
							for ( int k = 0; k < 3; ++k ) {
								vert.xyz[k] += c.xyz[k] * w;
							}
							for ( int k = 0; k < 2; ++k ) {
								vert.st[k] += c.st[k] * w;
								vert.lm[k] += c.lm[k] * w;
							}
							for ( int k = 0; k < 4; ++k ) {
								colour[k] += c.color[k] * w;
							}
						}
					}
					for ( int k = 0; k < 4; ++k ) {
						vert.color[k] = byte( std::clamp( colour[k] + 0.5f, 0.f, 255.f ) );
					}
					out.verts.push_back( vert );
				}
			}
			for ( int j = 0; j < n; ++j ) {
				for ( int i = 0; i < n; ++i ) {
					const unsigned a = base + j * ( n + 1 ) + i;
					const unsigned b = a + 1;
					const unsigned c = a + ( n + 1 );
					const unsigned d = c + 1;
					for ( unsigned index : { a, c, b, b, c, d } ) {
						out.indexes.push_back( index );
					}
				}
			}
		}
	}
}

template<typename T>
void put( std::vector<byte>& out, const T& value ){
	const byte* bytes = reinterpret_cast<const byte*>( &value );
	out.insert( out.end(), bytes, bytes + sizeof( T ) );
}
}

/*
   WriteLightSnapshot()
   \p pass counts the lighting passes done: 0 is direct light, n is after n bounces.
 */
void WriteLightSnapshot( const char *path, int pass, bool final ){
	if ( strEmptyOrNull( path ) ) {
		return;
	}

	std::vector<SnapSurface> surfaces;
	std::vector<std::string> shaders;
	std::vector<int> shaderRemap( bspShaders.size(), -1 );

	for ( const bspDrawSurface_t& ds : bspDrawSurfaces ) {
		if ( ds.shaderNum < 0 || size_t( ds.shaderNum ) >= bspShaders.size() ) {
			continue;
		}
		if ( ds.surfaceType != MST_PLANAR && ds.surfaceType != MST_PATCH && ds.surfaceType != MST_TRIANGLE_SOUP && ds.surfaceType != MST_FOLIAGE ) {
			continue;
		}

		const char* shaderName = bspShaders[ ds.shaderNum ].shader;
		const int flags = ShaderInfoForShader( shaderName ).compileFlags;
		if ( flags & ( C_SKY | C_NODRAW | C_FOG | C_TRANSLUCENT ) ) {
			continue;
		}

		SnapSurface surface;
		surface.lightmapNum = ds.lightmapNum[0] >= 0 ? ds.lightmapNum[0] : -1;

		if ( ds.surfaceType == MST_PATCH ) {
			tessellate_patch( ds, surface );
		}
		else {
			for ( int i = 0; i < ds.numVerts; ++i ) {
				surface.verts.push_back( snap_vert( bspDrawVerts[ ds.firstVert + i ] ) );
			}
			for ( int i = 0; i < ds.numIndexes; ++i ) {
				surface.indexes.push_back( unsigned( bspDrawIndexes[ ds.firstIndex + i ] ) );
			}
		}
		if ( surface.indexes.empty() ) {
			continue;
		}

		int& remap = shaderRemap[ ds.shaderNum ];
		if ( remap < 0 ) {
			remap = int( shaders.size() );
			shaders.emplace_back( shaderName );
		}
		surface.shader = unsigned( remap );
		surfaces.push_back( std::move( surface ) );
	}

	const unsigned lmSize = unsigned( g_game->lightmapSize );
	const size_t pageBytes = size_t( lmSize ) * lmSize * 3;
	const unsigned numLightmaps = externalLightmaps || pageBytes == 0 ? 0 : unsigned( bspLightBytes.size() / pageBytes );

	std::vector<byte> out;
	out.insert( out.end(), { 'L', 'S', 'N', '1' } );
	put( out, unsigned( pass ) );
	put( out, unsigned( final ? 1 : 0 ) );
	put( out, lmSize );
	put( out, numLightmaps );
	put( out, unsigned( shaders.size() ) );
	put( out, unsigned( surfaces.size() ) );

	out.insert( out.end(), bspLightBytes.begin(), bspLightBytes.begin() + size_t( numLightmaps ) * pageBytes );

	for ( const std::string& shader : shaders ) {
		put( out, unsigned( shader.size() ) );
		out.insert( out.end(), shader.begin(), shader.end() );
	}

	for ( const SnapSurface& surface : surfaces ) {
		put( out, surface.shader );
		put( out, int( surface.lightmapNum < int( numLightmaps ) ? surface.lightmapNum : -1 ) );
		put( out, unsigned( surface.verts.size() ) );
		put( out, unsigned( surface.indexes.size() ) );
		for ( const SnapVert& v : surface.verts ) {
			put( out, v );
		}
		for ( unsigned index : surface.indexes ) {
			put( out, index );
		}
	}

	/* the viewer polls this file: it must never see half of it */
	const std::string temporary = std::string( path ) + ".tmp";
	if ( FILE* file = fopen( temporary.c_str(), "wb" ) ) {
		const bool written = fwrite( out.data(), 1, out.size(), file ) == out.size();
		fclose( file );
		if ( written ) {
			std::error_code error;
			std::filesystem::rename( temporary, path, error );
			if ( error ) {
				std::filesystem::remove( path, error );
				std::filesystem::rename( temporary, path, error );
			}
			Sys_Printf( "Light snapshot (pass %d%s) written: %zu surfaces, %u lightmaps\n", pass, final ? ", final" : "", surfaces.size(), numLightmaps );
		}
	}
	else {
		Sys_Warning( "Unable to open %s for writing\n", temporary.c_str() );
	}
}

/*
   LightSnapBSPMain()
   -lightsnapbsp -lightsnap <out> <mapname>: writes what an already compiled bsp
   shows (its lightmaps, or vertex colours where it has none) in the snapshot
   format, so the editor can look at a compile's result without lighting anything.
 */
int LightSnapBSPMain( Args& args ){
	if ( args.size() < 3 ) {
		Sys_Printf( "Usage: q3map2 -lightsnapbsp -lightsnap <out.lsn> <mapname>\n" );
		return 1;
	}

	const char *fileName = args.takeBack();
	while ( args.takeArg( "-lightsnap" ) ) {
		lightSnapFile = args.takeNext();
	}
	if ( lightSnapFile.empty() ) {
		Sys_Printf( "-lightsnap <out.lsn> is required\n" );
		return 1;
	}

	strcpy( source, ExpandArg( fileName ) );
	path_set_extension( source, ".bsp" );

	/* the compile flags of a shader decide what is drawn at all */
	LoadShaderInfo();

	Sys_Printf( "Loading %s\n", source );
	LoadBSPFile( source );

	Sys_Printf( "--- LightSnapBSP ---\n" );
	WriteLightSnapshot( lightSnapFile.c_str(), 0, true );
	return 0;
}
