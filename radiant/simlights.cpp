/*
   Simulated map lights. See simlights.h.
 */

#include "simlights.h"

#include "ientity.h"
#include "ishaders.h"
#include "iscenegraph.h"
#include "irender.h"
#include "qerplugin.h"
#include "scenelib.h"
#include "stringio.h"
#include "string/string.h"
#include "brush.h"
#include "largemap.h"
#include "texturelib.h"
#include "timer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>

bool g_simLights_enabled = false;
bool g_simShadows_enabled = false;

namespace
{
unsigned g_generation = 1;
}

void SimLights_sceneChanged(){
	++g_generation;
}

unsigned SimLights_generation(){
	return g_generation;
}

namespace
{
/* q3map2 defaults for the Quake 3 family (q3map2.h). Only -pointscale and
   -spotscale change these at compile time, and the editor has no way to know
   what a given compile used, so the defaults are what is simulated. */
constexpr float c_pointScale = 7500.f;
constexpr float c_spotScale = 7500.f;
constexpr float c_linearScale = 1.f / 8000.f;
constexpr float c_falloffTolerance = 1.f;
constexpr float c_areaScale = 0.25f;
constexpr float c_formFactorValueScale = 3.f;
constexpr float c_defaultBacksplashFraction = 0.05f;
constexpr float c_defaultBacksplashDistance = 23.f;

constexpr int c_spawnLinear = 1;
constexpr int c_spawnNoAngle = 2;
constexpr int c_spawnUnnormalized = 32;

bool key_vector3( const Entity& entity, const char* key, Vector3& out ){
	return string_parse_vector3( entity.getKeyValue( key ), out );
}

/// q3map2's ColorNormalize: divide by the largest component, not the length.
Vector3 colour_normalised( Vector3 colour ){
	const float max = std::max( { colour.x(), colour.y(), colour.z() } );
	return max > 0 ? colour / max : Vector3( 1, 1, 1 );
}

struct Target
{
	std::string name;
	Vector3 origin;
};

struct Candidate
{
	SimLight light;
	float intensity;    ///< before the point / spot scale, which depends on the target
	std::string target; ///< non-empty: a spotlight aimed at the entity of this name
	float radius;       ///< spot cone `radius` key
};

class LightWalker : public scene::Traversable::Walker
{
	std::vector<Candidate>& m_lights;
	std::vector<Target>& m_targets;
	Vector3& m_ambient;
	Vector3& m_minlight;
	bool& m_haveWorld;
	bool& m_noShaderSun;

	void world( const Entity& entity ) const {
		if ( m_haveWorld ) {
			return;
		}
		m_haveWorld = true;

		/* LightWorld(): a missing or all-zero _color means white, and the same
		   colour tints both _ambient and _minlight. */
		Vector3 colour( 1, 1, 1 );
		if ( Vector3 c; key_vector3( entity, "_color", c ) && c != Vector3( 0, 0, 0 ) ) {
			colour = c;
		}
		float ambient = string_read_float( entity.getKeyValue( "_ambient" ) );
		if ( entity.getKeyValue( "_ambient" )[0] == '\0' ) {
			ambient = string_read_float( entity.getKeyValue( "ambient" ) );
		}
		m_ambient = colour * ambient;
		m_minlight = colour * string_read_float( entity.getKeyValue( "_minlight" ) );
		m_noShaderSun = string_read_int( entity.getKeyValue( "_noshadersun" ) ) != 0;
	}

	void light( const Entity& entity ) const {
		Vector3 origin;
		if ( !key_vector3( entity, "origin", origin ) ) {
			return;
		}

		const int spawnflags = string_read_int( entity.getKeyValue( "spawnflags" ) );

		/* DefaultQ3A is angle attenuation on; linear turns it off; flag 2 turns
		   it off; a non-zero _anglescale turns it back on. */
		const bool linear = spawnflags & c_spawnLinear;
		bool angle = !linear && !( spawnflags & c_spawnNoAngle );
		float angleScale = string_read_float( entity.getKeyValue( "_anglescale" ) );
		if ( angleScale != 0 ) {
			angle = true;
		}

		float fade = 1;
		if ( linear ) {
			fade = string_read_float( entity.getKeyValue( "fade" ) );
			if ( fade == 0 ) {
				fade = 1;
			}
		}

		/* _light wins over light; zero means the default of 300 */
		float intensity = 300;
		for ( const char* key : { "_light", "light" } ) {
			if ( entity.hasKeyValue( key ) ) {
				intensity = string_read_float( entity.getKeyValue( key ) );
				break;
			}
		}
		if ( intensity == 0 ) {
			intensity = 300;
		}
		if ( const float scale = string_read_float( entity.getKeyValue( "scale" ) ); scale != 0 ) {
			intensity *= scale;
		}
		if ( intensity <= 0 ) {
			return; // a dark light subtracts in q3map2; nothing to add here
		}

		Vector3 colour( 1, 1, 1 );
		if ( Vector3 c; key_vector3( entity, "_color", c ) ) {
			colour = ( spawnflags & c_spawnUnnormalized ) ? c : colour_normalised( c );
		}

		Candidate candidate{};
		SimLight& l = candidate.light;
		l.origin = origin;
		l.colour = colour;
		l.fade = fade;
		l.angleScale = angleScale;
		l.extraDist = std::fabs( string_read_float( entity.getKeyValue( "_extradist" ) ) );
		l.photons = intensity * c_pointScale;
		l.radiusByDist = 0;
		l.direction = Vector3( 0, 0, 0 );
		l.flags = ( linear ? SimLight::kFlagLinear : 0 ) | ( angle ? SimLight::kFlagAngle : 0 );
		candidate.intensity = intensity;

		/* a light with a target is a spotlight, unless the target is missing;
		   resolved once every target has been seen */
		candidate.target = entity.getKeyValue( "target" );
		candidate.radius = string_read_float( entity.getKeyValue( "radius" ) );
		if ( candidate.radius == 0 ) {
			candidate.radius = 64;
		}

		m_lights.push_back( std::move( candidate ) );
	}

public:
	LightWalker( std::vector<Candidate>& lights, std::vector<Target>& targets, Vector3& ambient, Vector3& minlight, bool& haveWorld, bool& noShaderSun ) :
		m_lights( lights ), m_targets( targets ), m_ambient( ambient ), m_minlight( minlight ), m_haveWorld( haveWorld ), m_noShaderSun( noShaderSun ){
	}

	bool pre( scene::Node& node ) const override {
		if ( const Entity* entity = Node_getEntity( node ) ) {
			const char* classname = entity->getClassName();

			if ( string_equal( classname, "worldspawn" ) ) {
				world( *entity );
			}
			/* q3map2 takes any classname starting "light"; lightJunior only
			   lights the light grid, i.e. entities, never surfaces */
			else if ( string_equal_prefix_nocase( classname, "light" ) && !string_equal_nocase( classname, "lightJunior" ) ) {
				light( *entity );
			}

			const char* targetname = entity->getKeyValue( "targetname" );
			if ( !string_empty( targetname ) ) {
				if ( Vector3 origin; key_vector3( *entity, "origin", origin ) ) {
					m_targets.push_back( { targetname, origin } );
				}
			}
		}
		return false; // top level only
	}
};

/// What a face's shader contributes to the light stage, looked up once per shader.
struct ShaderLight
{
	ShaderLightInfo info;
	Vector3 colour{ 1, 1, 1 }; ///< normalised, q3map2's si.color
	bool sky = false;
};

/*! \brief The map's q3map_surfacelight faces as area lights, and its q3map_sun.

    One SimLight per triangle: q3map2's exact point-to-polygon form factor adds
    up over pieces of a polygon, so cutting a face into triangles changes nothing
    the shader computes, and q3map2 does the same (RadLightForTriangles).
    q3map2's subdivision only moves where the trace starts; here it would move
    where a shadow cube is centred, and the nearest few lights get one anyway.
 */
class SurfaceLightScan
{
	std::unordered_map<Shader*, ShaderLight> m_shaders;

	const ShaderLight& shaderLight( Shader* state, const char* name ){
		const auto found = m_shaders.find( state );
		if ( found != m_shaders.end() ) {
			return found->second;
		}
		ShaderLight entry;
		if ( IShader* shader = GlobalShaderSystem().getShaderForName( name ) ) {
			entry.info = shader->getLightInfo();
			entry.sky = ( shader->getFlags() & QER_SKY ) != 0;
			if ( entry.info.hasLightRGB ) {
				entry.colour = colour_normalised( Vector3( entry.info.lightRGB[0], entry.info.lightRGB[1], entry.info.lightRGB[2] ) );
			}
			else if ( const qtexture_t* texture = shader->getTexture() ) {
				entry.colour = colour_normalised( Vector3( texture->color[0], texture->color[1], texture->color[2] ) );
			}
			shader->DecRef();
		}
		return m_shaders.emplace( state, entry ).first->second;
	}

	/// Distance at which a polygon of \p area stops adding more than q3map2's falloff tolerance.
	/// Far from a polygon its form factor is area * cos * cos / ( 2 pi d^2 ).
	static float envelope( float area, float add, float radius ){
		return std::max( 100.f, std::sqrt( area * add / ( 2.f * 3.14159265f * c_falloffTolerance ) ) + radius );
	}

	void emit( std::vector<SimLight>& out, const ShaderLight& shader, Vector3 v0, Vector3 v1, Vector3 v2, const Vector3& normal ){
		/* q3map2 winds a face clockwise seen from its front, so the cross product
		   of two edges points backwards. Whatever order the editor's windings come
		   in, the shader's form factor needs that one. */
		if ( vector3_dot( vector3_cross( v1 - v0, v2 - v0 ), normal ) > 0 ) {
			std::swap( v1, v2 );
		}

		const float area = 0.5f * vector3_length( vector3_cross( v1 - v0, v2 - v0 ) );
		if ( area < 1 || area > 20000000.f ) {
			return; // RadSubdivideDiffuseLight drops these
		}

		const Vector3 mins( std::min( { v0.x(), v1.x(), v2.x() } ), std::min( { v0.y(), v1.y(), v2.y() } ), std::min( { v0.z(), v1.z(), v2.z() } ) );
		const Vector3 maxs( std::max( { v0.x(), v1.x(), v2.x() } ), std::max( { v0.y(), v1.y(), v2.y() } ), std::max( { v0.z(), v1.z(), v2.z() } ) );
		const Vector3 centre = ( mins + maxs ) * 0.5f;
		const float radius = vector3_length( maxs - mins ) * 0.5f;

		SimLight l{};
		l.colour = shader.colour;
		l.verts[0] = v0;
		l.verts[1] = v1;
		l.verts[2] = v2;
		l.origin = centre + normal; // "nudge it off the plane a bit"
		l.flags = SimLight::kFlagArea;
		l.photons = shader.info.surfaceLight * c_formFactorValueScale * c_areaScale;
		l.envelope = envelope( area, l.photons, radius );
		out.push_back( l );

		const float fraction = shader.info.backsplashFraction >= 0 ? shader.info.backsplashFraction : c_defaultBacksplashFraction;
		const float distance = shader.info.backsplashDistance >= 0 ? shader.info.backsplashDistance : c_defaultBacksplashDistance;
		if ( fraction > 0 && !shader.sky ) {
			/* a dimmer polygon in front of the emitter, facing back at it: what q3map2
			   calls backsplash, and the reason a light in a wall lights the wall */
			SimLight splash = l;
			splash.verts[0] = v2 + normal * distance;
			splash.verts[1] = v1 + normal * distance;
			splash.verts[2] = v0 + normal * distance;
			splash.origin = normal * distance + l.origin;
			splash.photons = l.photons * 7.f * fraction;
			splash.envelope = envelope( area, splash.photons, radius );
			out.push_back( splash );
		}
	}

	/// First pass over a brush's faces: does anything on it emit, and is there a sun.
	class FaceProbe : public BrushVisitor
	{
		SurfaceLightScan& m_scan;
		SimSun& m_sun;
		bool m_allowSun;
	public:
		mutable bool m_emits = false;
		FaceProbe( SurfaceLightScan& scan, SimSun& sun, bool allowSun ) : m_scan( scan ), m_sun( sun ), m_allowSun( allowSun ){
		}
		void visit( Face& face ) const override {
			Shader* state = face.getShader().state();
			if ( state == nullptr ) {
				return;
			}
			const ShaderLight& light = m_scan.shaderLight( state, face.getShader().getShader() );
			if ( m_allowSun && light.info.hasSun && !m_sun.present ) {
				const double angle = degrees_to_radians( light.info.sunDegrees );
				const double elevation = degrees_to_radians( light.info.sunElevation );
				m_sun.present = true;
				m_sun.direction = vector3_for_spherical( angle, elevation );
				m_sun.colour = colour_normalised( Vector3( light.info.sunColour[0], light.info.sunColour[1], light.info.sunColour[2] ) );
				m_sun.photons = light.info.sunIntensity;
			}
			if ( light.info.surfaceLight > 0 ) {
				m_emits = true;
			}
		}
	};

	/// Second pass, only for brushes the first one flagged: needs the windings.
	class FaceEmit : public BrushVisitor
	{
		SurfaceLightScan& m_scan;
		std::vector<SimLight>& m_out;
		const Matrix4& m_localToWorld;
	public:
		FaceEmit( SurfaceLightScan& scan, std::vector<SimLight>& out, const Matrix4& localToWorld ) : m_scan( scan ), m_out( out ), m_localToWorld( localToWorld ){
		}
		void visit( Face& face ) const override {
			const Winding& winding = face.getWinding();
			Shader* state = face.getShader().state();
			if ( state == nullptr || winding.numpoints < 3 ) {
				return;
			}
			const ShaderLight& light = m_scan.shaderLight( state, face.getShader().getShader() );
			if ( !( light.info.surfaceLight > 0 ) ) {
				return;
			}

			const DoubleVector3& n = face.plane3().normal();
			const Vector3 normal = vector3_normalised( matrix4_transformed_direction( m_localToWorld, Vector3( float( n.x() ), float( n.y() ), float( n.z() ) ) ) );

			const auto point = [&]( std::size_t i ){
				const DoubleVector3& v = winding.points[i].vertex;
				return matrix4_transformed_point( m_localToWorld, Vector3( float( v.x() ), float( v.y() ), float( v.z() ) ) );
			};
			for ( std::size_t i = 1; i + 1 < winding.numpoints; ++i ) // fan; brush faces are convex
			{
				m_scan.emit( m_out, light, point( 0 ), point( i ), point( i + 1 ), normal );
			}
		}
	};

	class Walker : public scene::Graph::Walker
	{
		SurfaceLightScan& m_scan;
		bool m_allowSun;
	public:
		Walker( SurfaceLightScan& scan, bool allowSun ) : m_scan( scan ), m_allowSun( allowSun ){
		}
		bool pre( const scene::Path& path, scene::Instance& instance ) const override {
			if ( path.size() <= 2 ) {
				return true; // the scene root, then any entity: descend to the primitives
			}
			if ( path.size() != 3 ) {
				return false;
			}
			if ( BrushInstance* brush = Instance_getBrush( instance ) ) {
				/* Shader names first: b-reps are built lazily and a big map has
				   nearly none that emit, so asking each brush for its windings
				   would build them all. */
				const FaceProbe probe( m_scan, m_scan.m_sun, m_allowSun );
				brush->getBrush().forEachFace( probe );
				if ( probe.m_emits ) {
					brush->getBrush().evaluateBRep();
					brush->getBrush().forEachFace( FaceEmit( m_scan, m_scan.m_lights, instance.localToWorld() ) );
				}
			}
			return false;
		}
	};

public:
	std::vector<SimLight> m_lights;
	SimSun m_sun;
	unsigned m_generation = 0;
	bool m_valid = false;
	bool m_withSun = false;
	std::size_t m_reportedLights = std::size_t( -1 );
	bool m_reportedSun = false;

	/// \brief Up to date for this scene? Rescans if not.
	void update( bool allowSun ){
		if ( m_valid && m_generation == g_generation && m_withSun == allowSun ) {
			return;
		}

		Timer timer;
		timer.start();

		m_lights.clear();
		m_sun = SimSun();
		m_shaders.clear(); // shaders can be reloaded between scans

		GlobalSceneGraph().traverse( Walker( *this, allowSun ) );

		m_generation = g_generation;
		m_withSun = allowSun;
		m_valid = true;

		const int msec = timer.elapsed_msec();
		if ( msec > 100 ) {
			globalOutputStream() << "Simulated lights: gathering surface lights took " << msec << " ms (" << Unsigned( m_lights.size() ) << " lights)\n";
		}

		/* what the map's shaders gave, said whenever it changes: an emitter or a sun
		   that does nothing is otherwise indistinguishable from one that was not found */
		if ( m_reportedLights != m_lights.size() || m_reportedSun != m_sun.present ) {
			m_reportedLights = m_lights.size();
			m_reportedSun = m_sun.present;
			globalOutputStream() << "Simulated lights: " << Unsigned( m_lights.size() ) << " surface light triangles, "
			                     << ( m_sun.present ? "a q3map_sun" : "no q3map_sun" ) << " in this map's shaders\n";
		}
	}
};

SurfaceLightScan g_surfaceLights;

struct Plane
{
	float a, b, c, d;
};

/// Frustum planes (normals pointing in) from projection * modelview, column-major.
void frustum_planes( const Matrix4& modelview, const Matrix4& projection, Plane ( &planes )[6] ){
	const float* p = reinterpret_cast<const float*>( &projection );
	const float* m = reinterpret_cast<const float*>( &modelview );

	float clip[16];
	for ( int col = 0; col < 4; ++col ) {
		for ( int row = 0; row < 4; ++row ) {
			float sum = 0;
			for ( int k = 0; k < 4; ++k ) {
				sum += p[k * 4 + row] * m[col * 4 + k];
			}
			clip[col * 4 + row] = sum;
		}
	}

	/* plane = row 3 +/- row `other` of the clip matrix */
	auto plane = [&clip]( float sign, int other ){
		return Plane{
			clip[0 * 4 + 3] + sign * clip[0 * 4 + other],
			clip[1 * 4 + 3] + sign * clip[1 * 4 + other],
			clip[2 * 4 + 3] + sign * clip[2 * 4 + other],
			clip[3 * 4 + 3] + sign * clip[3 * 4 + other] };
	};
	planes[0] = plane( +1, 0 ); // left
	planes[1] = plane( -1, 0 ); // right
	planes[2] = plane( +1, 1 ); // bottom
	planes[3] = plane( -1, 1 ); // top
	planes[4] = plane( +1, 2 ); // near
	planes[5] = plane( -1, 2 ); // far
}

bool sphere_in_frustum( const Plane ( &planes )[6], const Vector3& centre, float radius ){
	for ( const Plane& plane : planes ) {
		const float length = std::sqrt( plane.a * plane.a + plane.b * plane.b + plane.c * plane.c );
		if ( length == 0 ) {
			continue;
		}
		if ( plane.a * centre.x() + plane.b * centre.y() + plane.c * centre.z() + plane.d < -radius * length ) {
			return false;
		}
	}
	return true;
}
}

void SimLights_collect( SimLightsFrame& frame, const Matrix4& modelview, const Matrix4& projection, const Vector3& viewer, std::size_t maxLights ){
	frame.lights.clear();
	frame.ambient = Vector3( 0, 0, 0 );
	frame.minlight = Vector3( 0, 0, 0 );
	frame.total = 0;
	frame.inView = 0;
	frame.sun = SimSun();

	std::vector<Candidate> candidates;
	std::vector<Target> targets;
	bool haveWorld = false;
	bool noShaderSun = false;
	Node_getTraversable( GlobalSceneGraph().root() )->traverse( LightWalker( candidates, targets, frame.ambient, frame.minlight, haveWorld, noShaderSun ) );

	Plane planes[6];
	frustum_planes( modelview, projection, planes );

	std::vector<SimLight> inView;
	for ( Candidate& candidate : candidates ) {
		SimLight& l = candidate.light;

		if ( !candidate.target.empty() ) {
			const auto target = std::find_if( targets.begin(), targets.end(), [&]( const Target& t ){
				return string_equal_nocase( t.name.c_str(), candidate.target.c_str() );
			} );
			if ( target != targets.end() ) {
				/* a spotlight: always inverse square with angle attenuation, and
				   spotScale instead of pointScale */
				Vector3 axis = target->origin - l.origin;
				float dist = vector3_length( axis );
				if ( dist != 0 ) {
					axis /= dist;
				}
				else{
					dist = 64;
				}
				l.direction = axis;
				l.radiusByDist = ( candidate.radius + 16 ) / dist;
				l.photons = candidate.intensity * c_spotScale;
				l.fade = 1;
				l.flags = SimLight::kFlagAngle | SimLight::kFlagSpot;
			}
		}

		l.envelope = ( l.flags & SimLight::kFlagLinear )
		             ? ( l.photons * c_linearScale - c_falloffTolerance ) / l.fade
		             : std::sqrt( l.photons / c_falloffTolerance );
		if ( !( l.envelope > 0 ) ) {
			continue;
		}

		++frame.total;
		if ( sphere_in_frustum( planes, l.origin, l.envelope ) ) {
			inView.push_back( l );
		}
	}

	/* Surface lights and the sun both come from what the brushes' shaders say,
	   so one cached scan serves them. An unshadowed sun would light the inside of
	   every room, so the sun waits for shadows. */
	const bool wantSun = g_simShadows_enabled && !noShaderSun;
	if ( g_largemap_simSurfaceLights || wantSun ) {
		g_surfaceLights.update( !noShaderSun );
		if ( g_largemap_simSurfaceLights ) {
			for ( const SimLight& l : g_surfaceLights.m_lights ) {
				++frame.total;
				if ( sphere_in_frustum( planes, l.origin, l.envelope ) ) {
					inView.push_back( l );
				}
			}
		}
		if ( wantSun && g_surfaceLights.m_sun.present && g_surfaceLights.m_sun.photons > 0 ) {
			frame.sun = g_surfaceLights.m_sun;
		}
	}

	frame.inView = inView.size();

	if ( inView.size() > maxLights ) {
		std::partial_sort( inView.begin(), inView.begin() + maxLights, inView.end(), [&]( const SimLight& a, const SimLight& b ){
			return vector3_length_squared( a.origin - viewer ) < vector3_length_squared( b.origin - viewer );
		} );
		inView.resize( maxLights );
	}
	else{
		std::sort( inView.begin(), inView.end(), [&]( const SimLight& a, const SimLight& b ){
			return vector3_length_squared( a.origin - viewer ) < vector3_length_squared( b.origin - viewer );
		} );
	}
	frame.lights = std::move( inView );
}


void SimLights_pack( const SimLight& l, float* out ){
	const float flags = float( ( l.flags & SimLight::kKindMask ) + ( l.shadowSlot + 1 ) * SimLight::kSlotScale );
	float packed[c_simLightTexels * 4] = {};
	if ( l.flags & SimLight::kFlagArea ) {
		/* the triangle stands in for the position, which the shader works out from it */
		const float area[16] = {
			l.verts[0].x(), l.verts[0].y(), l.verts[0].z(), l.envelope,
			l.verts[1].x(), l.verts[1].y(), l.verts[1].z(), l.photons,
			l.verts[2].x(), l.verts[2].y(), l.verts[2].z(), 0,
			l.colour.x(), l.colour.y(), l.colour.z(), flags };
		std::copy( std::begin( area ), std::end( area ), packed );
	}
	else{
		const float point[16] = {
			l.origin.x(), l.origin.y(), l.origin.z(), l.envelope,
			l.colour.x(), l.colour.y(), l.colour.z(), l.photons,
			l.fade, l.angleScale, l.extraDist, l.radiusByDist,
			l.direction.x(), l.direction.y(), l.direction.z(), flags };
		std::copy( std::begin( point ), std::end( point ), packed );
	}
	/* the fifth texel is what the shader reads first, to throw a light out cheaply */
	packed[16] = l.origin.x();
	packed[17] = l.origin.y();
	packed[18] = l.origin.z();
	packed[19] = l.envelope;
	std::copy( std::begin( packed ), std::end( packed ), out );
}

namespace
{
/* Clusters are tiles of the screen times slices of depth that grow with distance,
   so a cluster is roughly as deep as it is wide wherever it is. */
constexpr int c_clusterTilePx = 64;
constexpr int c_clusterSlices = 24;
constexpr float c_clusterNear = 16.f;    ///< everything nearer is in the first slice
constexpr float c_clusterFar = 16384.f;  ///< everything further is in the last
constexpr float c_projectionNear = 1.f;  ///< the camera's near plane; a sphere that reaches it may be anywhere on screen
/// What a light adds, in lightmap-byte units of 255, below which it is not worth listing.
constexpr float c_clusterFaint = 0.5f;

struct ClusterRange
{
	int x0, x1, y0, y1, z0, z1;
};

float light_area( const SimLight& l ){
	return 0.5f * vector3_length( vector3_cross( l.verts[1] - l.verts[0], l.verts[2] - l.verts[0] ) );
}

/// Roughly what \p l adds at \p distance from it, ignoring angle and shadow: only for ranking lights.
float light_strength( const SimLight& l, float area, float distance ){
	const float d = std::max( distance, 16.f );
	const float colour = std::max( { l.colour.x(), l.colour.y(), l.colour.z() } );
	float add;
	if ( l.flags & SimLight::kFlagArea ) {
		add = l.photons * area / ( 2.f * 3.14159265f * d * d );
	}
	else if ( l.flags & SimLight::kFlagLinear ) {
		add = std::max( 0.f, l.photons * c_linearScale - d * l.fade );
	}
	else{
		add = l.photons / ( d * d );
	}
	return add * colour;
}
}

void SimLights_buildClusters( SimClusters& out, const SimLightsFrame& frame, const Matrix4& modelview, const Matrix4& projection, int width, int height ){
	out.tilePx = c_clusterTilePx;
	out.tilesX = std::max( ( width + c_clusterTilePx - 1 ) / c_clusterTilePx, 1 );
	out.tilesY = std::max( ( height + c_clusterTilePx - 1 ) / c_clusterTilePx, 1 );
	out.slices = c_clusterSlices;
	out.zNear = c_clusterNear;
	out.depthScale = float( c_clusterSlices ) / std::log( c_clusterFar / c_clusterNear );
	out.lightCount = frame.lights.size();

	const std::size_t clusters = std::size_t( out.tilesX ) * out.tilesY * out.slices;
	const auto clusterIndex = [&]( int x, int y, int z ){
		return ( std::size_t( z ) * out.tilesY + y ) * out.tilesX + x;
	};

	std::vector<float> lightTexels( std::max<std::size_t>( frame.lights.size(), 1 ) * c_simLightTexels * 4, 0.f );
	for ( std::size_t i = 0; i < frame.lights.size(); ++i ) {
		SimLights_pack( frame.lights[i], &lightTexels[i * c_simLightTexels * 4] );
	}

	/* Nothing about the lights or the view changed since the last frame: what was built stands. */
	if ( out.built && lightTexels == out.lightTexels && out.builtModelview == modelview
	  && out.builtProjection == projection && out.builtWidth == width && out.builtHeight == height ) {
		return;
	}
	out.lightTexels = std::move( lightTexels );
	out.overflow = 0;
	out.built = true;
	out.builtModelview = modelview;
	out.builtProjection = projection;
	out.builtWidth = width;
	out.builtHeight = height;

	const float sx = projection.xx(), sy = projection.yy();

	/* the edges of every tile column and row, in NDC, and of every slice, in depth */
	std::vector<float> ndcX( out.tilesX + 1 ), ndcY( out.tilesY + 1 ), sliceZ( out.slices + 1 );
	for ( int x = 0; x <= out.tilesX; ++x ) {
		ndcX[x] = float( x * c_clusterTilePx ) / float( width ) * 2.f - 1.f;
	}
	for ( int y = 0; y <= out.tilesY; ++y ) {
		ndcY[y] = float( y * c_clusterTilePx ) / float( height ) * 2.f - 1.f;
	}
	for ( int z = 0; z <= out.slices; ++z ) {
		sliceZ[z] = c_clusterNear * std::exp( float( z ) / out.depthScale );
	}
	sliceZ[0] = 0;                          // the first slice takes everything nearer
	sliceZ[out.slices] = 1e30f;             // and the last everything further

	const auto slice_of = [&]( float z ){
		return std::clamp( int( std::floor( std::log( std::max( z, c_clusterNear ) / c_clusterNear ) * out.depthScale ) ), 0, out.slices - 1 );
	};
	const auto tile_of = [&]( float ndc, int tiles, int size ){
		return std::clamp( int( std::floor( ( ndc * 0.5f + 0.5f ) * float( size ) / float( c_clusterTilePx ) ) ), 0, tiles - 1 );
	};

	/* Does light \p l's sphere reach this cluster's cell? The cell is four planes through the eye and two depths. */
	const auto reaches = [&]( const Vector3& eye, float radius, int x, int y, int z ){
		const float depth = -eye.z();
		if ( depth + radius < sliceZ[z] || depth - radius > sliceZ[z + 1] ) {
			return false;
		}
		// sx * x - ndc * depth, over the plane normal's length, is how far inside the side the centre is
		const auto outside = [&]( float scale, float along, float ndc, float sign ){
			return sign * ( scale * along + ndc * eye.z() ) < -radius * std::sqrt( scale * scale + ndc * ndc );
		};
		return !outside( sx, eye.x(), ndcX[x], 1.f ) && !outside( sx, eye.x(), ndcX[x + 1], -1.f )
		    && !outside( sy, eye.y(), ndcY[y], 1.f ) && !outside( sy, eye.y(), ndcY[y + 1], -1.f );
	};

	std::vector<Vector3> eyes( frame.lights.size() );
	std::vector<ClusterRange> ranges( frame.lights.size() );
	std::vector<unsigned> counts( clusters, 0 );

	for ( std::size_t i = 0; i < frame.lights.size(); ++i ) {
		const SimLight& l = frame.lights[i];
		const Vector3 eye = matrix4_transformed_point( modelview, l.origin );
		eyes[i] = eye;
		const float depth = -eye.z(); // eye space looks down -z
		const float radius = l.envelope;

		ClusterRange& r = ranges[i];
		r.z0 = slice_of( depth - radius );
		r.z1 = slice_of( depth + radius );

		const float nearest = depth - radius;
		if ( nearest <= c_projectionNear ) {
			r.x0 = 0; r.x1 = out.tilesX - 1; r.y0 = 0; r.y1 = out.tilesY - 1; // reaches the camera
		}
		else{
			/* a point at x, z projects to x / z; over the sphere that is largest at its nearest depth */
			const float x0 = ( eye.x() - radius ) * sx / nearest, x1 = ( eye.x() + radius ) * sx / nearest;
			const float y0 = ( eye.y() - radius ) * sy / nearest, y1 = ( eye.y() + radius ) * sy / nearest;
			if ( x1 < -1 || x0 > 1 || y1 < -1 || y0 > 1 ) {
				r.x0 = 1; r.x1 = 0; r.y0 = 1; r.y1 = 0; // off screen: empty
			}
			else{
				r.x0 = tile_of( x0, out.tilesX, width );
				r.x1 = tile_of( x1, out.tilesX, width );
				r.y0 = tile_of( y0, out.tilesY, height );
				r.y1 = tile_of( y1, out.tilesY, height );
			}
		}

		for ( int z = r.z0; z <= r.z1; ++z ) {
			for ( int y = r.y0; y <= r.y1; ++y ) {
				for ( int x = r.x0; x <= r.x1; ++x ) {
					if ( reaches( eye, radius, x, y, z ) ) {
						++counts[clusterIndex( x, y, z )];
					}
				}
			}
		}
	}

	/* every cluster's full list, in light order, before it is trimmed */
	std::vector<std::size_t> starts( clusters + 1, 0 );
	for ( std::size_t c = 0; c < clusters; ++c ) {
		starts[c + 1] = starts[c] + counts[c];
	}
	std::vector<unsigned> entries( starts[clusters] );
	{
		std::vector<unsigned> fill( clusters, 0 );
		for ( std::size_t i = 0; i < frame.lights.size(); ++i ) {
			const ClusterRange& r = ranges[i];
			for ( int z = r.z0; z <= r.z1; ++z ) {
				for ( int y = r.y0; y <= r.y1; ++y ) {
					for ( int x = r.x0; x <= r.x1; ++x ) {
						if ( reaches( eyes[i], frame.lights[i].envelope, x, y, z ) ) {
							const std::size_t c = clusterIndex( x, y, z );
							entries[starts[c] + fill[c]++] = unsigned( i );
						}
					}
				}
			}
		}
	}

	std::vector<float> areas( frame.lights.size(), 0.f );
	for ( std::size_t i = 0; i < frame.lights.size(); ++i ) {
		if ( frame.lights[i].flags & SimLight::kFlagArea ) {
			areas[i] = light_area( frame.lights[i] );
		}
	}

	/* Keep, per cluster, the lights that add the most there: the ones too faint to
	   see are not worth a texture fetch, and the strongest c_simClusterMax of the rest fit. */
	out.clusterTexels.assign( clusters * 2, 0.f );
	out.indexTexels.clear();
	out.indexTexels.reserve( std::min<std::size_t>( entries.size(), clusters * 32 ) );
	std::vector<std::pair<float, unsigned>> ranked;
	for ( int z = 0; z < out.slices; ++z ) {
		/* where this slice's cells are, for judging how far a light is from them */
		const float cellNear = z == 0 ? 0.f : sliceZ[z];
		const float cellFar = z == out.slices - 1 ? sliceZ[z] * 1.33f : sliceZ[z + 1];
		const float mid = z == 0 ? c_clusterNear * 0.5f : std::sqrt( cellNear * cellFar );
		for ( int y = 0; y < out.tilesY; ++y ) {
			for ( int x = 0; x < out.tilesX; ++x ) {
				const std::size_t c = clusterIndex( x, y, z );
				const Vector3 centre( ( ndcX[x] + ndcX[x + 1] ) * 0.5f * mid / sx, ( ndcY[y] + ndcY[y + 1] ) * 0.5f * mid / sy, -mid );
				const float halfW = ( ndcX[x + 1] - ndcX[x] ) * 0.5f * cellFar / sx;
				const float halfH = ( ndcY[y + 1] - ndcY[y] ) * 0.5f * cellFar / sy;
				const float radius = std::sqrt( halfW * halfW + halfH * halfH + ( cellFar - cellNear ) * ( cellFar - cellNear ) * 0.25f );

				ranked.clear();
				for ( std::size_t e = starts[c]; e < starts[c + 1]; ++e ) {
					const unsigned i = entries[e];
					const float distance = vector3_length( eyes[i] - centre ) - radius;
					const float strength = light_strength( frame.lights[i], areas[i], distance );
					if ( strength >= c_clusterFaint ) {
						ranked.emplace_back( strength, i );
					}
				}
				if ( ranked.size() > c_simClusterMax ) {
					out.overflow += ranked.size() - c_simClusterMax;
					std::nth_element( ranked.begin(), ranked.begin() + c_simClusterMax, ranked.end(), []( const auto& a, const auto& b ){
						return a.first > b.first;
					} );
					ranked.resize( c_simClusterMax );
				}
				out.clusterTexels[c * 2 + 0] = float( out.indexTexels.size() );
				out.clusterTexels[c * 2 + 1] = float( ranked.size() );
				for ( const auto& [ strength, light ] : ranked ) {
					out.indexTexels.push_back( float( light ) );
				}
			}
		}
	}
	out.indexCount = out.indexTexels.size();
	if ( out.indexTexels.empty() ) {
		out.indexTexels.push_back( 0.f );
	}
}
