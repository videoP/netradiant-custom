// Simulated map lights (radiant/simlights.cpp): fragment stage.
//
// Per pixel, what q3map2's LightContributionToSample() does per luxel for
// point and spot lights (tools/quake3/q3map2/light.cpp). Quantities are in
// q3map2's lightmap-byte units until the last line, where 255 is full bright.
//
// SIMLIGHTS_MAX is defined by the loader, from the driver's uniform limit.

const float LINEAR_SCALE = 1.0 / 8000.0;   // q3map2 linearScale
const float FALLOFF_TOLERANCE = 1.0;       // q3map2 falloffTolerance: -fast drops anything at or below it
const float FLAG_LINEAR = 1.0;
const float FLAG_ANGLE = 2.0;
const float FLAG_SPOT = 4.0;

uniform sampler2D u_diffusemap;
uniform int u_count;
uniform vec3 u_ambient;   // worldspawn _ambient * _color
uniform vec3 u_minlight;  // worldspawn _minlight * _color
// four vec4 per light:
//   [0] origin.xyz, envelope
//   [1] colour.rgb, photons
//   [2] fade, anglescale, extradist, radiusByDist (spot cone)
//   [3] spot axis.xyz, flags
uniform vec4 u_lights[ SIMLIGHTS_MAX * 4 ];

varying vec3 var_world_pos;
varying vec3 var_world_normal;

bool has_flag( float flags, float flag )
{
	return mod( floor( flags / flag ), 2.0 ) >= 1.0;
}

void main()
{
	vec4 diffuse = texture2D( u_diffusemap, gl_TexCoord[0].st );

	// a shader without cull is visible from behind: light that side as its own surface
	vec3 N = normalize( var_world_normal );
	if ( !gl_FrontFacing ) {
		N = -N;
	}

	vec3 lit = u_ambient;

	for ( int i = 0; i < SIMLIGHTS_MAX; ++i ) {
		if ( i >= u_count ) {
			break;
		}
		vec4 a = u_lights[ i * 4 + 0 ];
		vec4 b = u_lights[ i * 4 + 1 ];
		vec4 c = u_lights[ i * 4 + 2 ];
		vec4 d = u_lights[ i * 4 + 3 ];

		vec3 to_light = a.xyz - var_world_pos;
		float dist = length( to_light );
		if ( dist >= a.w || dist <= 0.0001 ) {
			continue;
		}
		vec3 L = to_light / dist;

		// q3map2: a light behind the surface never reaches it
		float ndotl = dot( N, L );
		if ( ndotl < 0.0 ) {
			continue;
		}

		float flags = d.w;
		float photons = b.w;

		// clamp the distance so a light on a surface does not blow out
		float eff = max( 16.0, sqrt( dist * dist + c.z * c.z ) );

		float angle = has_flag( flags, FLAG_ANGLE ) ? ndotl : 1.0;
		if ( c.y != 0.0 ) {
			angle = min( angle / c.y, 1.0 );
		}

		float add;
		if ( has_flag( flags, FLAG_LINEAR ) ) {
			add = max( 0.0, angle * photons * LINEAR_SCALE - eff * c.x );
		}
		else {
			add = photons / ( eff * eff ) * angle;
		}

		if ( has_flag( flags, FLAG_SPOT ) ) {
			// distance along the axis; behind the light is outside the cone
			float along = dot( var_world_pos - a.xyz, d.xyz );
			if ( along < 0.0 ) {
				continue;
			}
			float cone = c.w * along;
			float off_axis = length( var_world_pos - ( a.xyz + d.xyz * along ) );
			if ( off_axis >= cone ) {
				continue;
			}
			if ( off_axis > cone - 32.0 ) {
				add *= ( cone - off_axis ) / 32.0;
			}
		}

		if ( add <= FALLOFF_TOLERANCE ) {
			continue;
		}
		lit += b.rgb * add;
	}

	// _minlight is a floor on the finished luxel
	lit = max( lit, u_minlight );

	// ColorToBytes(): over-bright light keeps its hue and clamps the brightest channel
	float peak = max( lit.r, max( lit.g, lit.b ) );
	if ( peak > 255.0 ) {
		lit *= 255.0 / peak;
	}

	gl_FragColor = vec4( diffuse.rgb * ( lit / 255.0 ), diffuse.a * gl_Color.a );
}
