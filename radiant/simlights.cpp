/*
   Simulated map lights. See simlights.h.
 */

#include "simlights.h"

#include "ientity.h"
#include "iscenegraph.h"
#include "scenelib.h"
#include "stringio.h"
#include "string/string.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

bool g_simLights_enabled = false;

namespace
{
/* q3map2 defaults for the Quake 3 family (q3map2.h). Only -pointscale and
   -spotscale change these at compile time, and the editor has no way to know
   what a given compile used, so the defaults are what is simulated. */
constexpr float c_pointScale = 7500.f;
constexpr float c_spotScale = 7500.f;
constexpr float c_linearScale = 1.f / 8000.f;
constexpr float c_falloffTolerance = 1.f;

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
	LightWalker( std::vector<Candidate>& lights, std::vector<Target>& targets, Vector3& ambient, Vector3& minlight, bool& haveWorld ) :
		m_lights( lights ), m_targets( targets ), m_ambient( ambient ), m_minlight( minlight ), m_haveWorld( haveWorld ){
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

	std::vector<Candidate> candidates;
	std::vector<Target> targets;
	bool haveWorld = false;
	Node_getTraversable( GlobalSceneGraph().root() )->traverse( LightWalker( candidates, targets, frame.ambient, frame.minlight, haveWorld ) );

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
