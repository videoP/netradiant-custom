// Simulated map lights (radiant/simlights.cpp): fragment stage.
//
// Per pixel, what q3map2's LightContributionToSample() does per luxel for
// point, spot, surface (area) and sun lights (tools/quake3/q3map2/light.cpp).
// Quantities are in q3map2's lightmap-byte units until the last line, where 255
// is full bright.
//
// Defined by the loader, from what the driver allows:
//   SIMLIGHTS_CLUSTER_MAX  lights one cluster's list can hold
//   SIMLIGHTS_TEX_W        texels across the data textures
//   SIMSHADOW_CUBES   cube shadow samplers (0..6)
//   SIMSHADOW_SUN     1 if there is a 2D sampler for the sun's shadow map
//   SIMSHADOW_NEAR    near plane the cube faces were drawn with

const float LINEAR_SCALE = 1.0 / 8000.0;   // q3map2 linearScale
const float FALLOFF_TOLERANCE = 1.0;       // q3map2 falloffTolerance: -fast drops anything at or below it
const float INV_TWO_PI = 0.15915494309;
const float FLAG_LINEAR = 1.0;
const float FLAG_ANGLE = 2.0;
const float FLAG_SPOT = 4.0;
const float FLAG_AREA = 8.0;
const float FLAG_TWOSIDED = 16.0;
const float FLAG_KINDS = 32.0;             // flags = kinds + ( shadow slot + 1 ) * 32

uniform sampler2D u_diffusemap;
uniform vec3 u_ambient;   // worldspawn _ambient * _color
uniform vec3 u_minlight;  // worldspawn _minlight * _color

// Every light in view lives in u_light_tex, four RGBA texels each, flags always in
// the fourth texel's w. The screen is cut into tiles, and depth into slices that
// grow with distance; u_cluster_tex says, per cluster, where in u_index_tex the
// list of lights that reach it starts and how long it is. A point or spot light:
//   [0] origin.xyz, envelope
//   [1] colour.rgb, photons
//   [2] fade, anglescale, extradist, radiusByDist (spot cone)
//   [3] spot axis.xyz, flags
// An area light, one triangle wound as q3map2 winds a face:
//   [0] v0.xyz, envelope
//   [1] v1.xyz, add
//   [2] v2.xyz, unused
//   [3] colour.rgb, flags
uniform sampler2D u_light_tex;
uniform sampler2D u_cluster_tex;   // ( first index, count )
uniform sampler2D u_index_tex;     // light numbers
uniform vec3 u_data_rows;          // heights of the three textures above
uniform vec4 u_cluster_grid;       // tiles across, tiles up, slices, tile size in pixels
uniform vec2 u_cluster_depth;      // depth of the first slice, and slices per e-fold: slice = log( z / near ) * this

// Shadow cubes: the position each was drawn from, and how far out it reaches.
#if SIMSHADOW_CUBES > 0
uniform vec4 u_shadow_pos[ SIMSHADOW_CUBES ];
#else
uniform vec4 u_shadow_pos[ 1 ];
#endif
uniform float u_shadow_texel;   // 2 / cube size: a texel's width as a fraction of the distance along the face axis

#if SIMSHADOW_CUBES > 0
uniform samplerCube u_shadowmap0;
#endif
#if SIMSHADOW_CUBES > 1
uniform samplerCube u_shadowmap1;
#endif
#if SIMSHADOW_CUBES > 2
uniform samplerCube u_shadowmap2;
#endif
#if SIMSHADOW_CUBES > 3
uniform samplerCube u_shadowmap3;
#endif
#if SIMSHADOW_CUBES > 4
uniform samplerCube u_shadowmap4;
#endif
#if SIMSHADOW_CUBES > 5
uniform samplerCube u_shadowmap5;
#endif

// The sun: q3map_sun, when shadows are on.
uniform int u_sun_on;
uniform vec3 u_sun_dir;      // unit, from a surface towards the sun
uniform vec3 u_sun_light;    // colour * intensity
uniform int u_sun_shadow_on;
uniform mat4 u_sun_matrix;   // world -> the sun map's [0,1]^3
uniform vec4 u_sun_params;   // x world units per texel, y world units the depth spans, z 1 / map size
#if SIMSHADOW_SUN
uniform sampler2D u_sun_map;
#endif

varying vec3 var_world_pos;
varying vec3 var_world_normal;
varying float var_eye_z;

bool has_flag( float flags, float flag )
{
	return mod( floor( mod( flags, FLAG_KINDS ) / flag ), 2.0 ) >= 1.0;
}

// ---------------------------------------------------------------------------
// the light data: a flat array in a texture, SIMLIGHTS_TEX_W texels to a row

vec4 fetch_texel( sampler2D tex, float index, float rows )
{
	float row = floor( index / SIMLIGHTS_TEX_W );
	float col = index - row * SIMLIGHTS_TEX_W;
	return texture2D( tex, vec2( ( col + 0.5 ) / SIMLIGHTS_TEX_W, ( row + 0.5 ) / rows ) );
}

// ---------------------------------------------------------------------------
// shadows

// what a cube holds in a direction: depth as the hardware stored it
float cube_depth( int slot, vec3 dir )
{
#if SIMSHADOW_CUBES > 0
	if ( slot == 0 ) return textureCube( u_shadowmap0, dir ).r;
#endif
#if SIMSHADOW_CUBES > 1
	if ( slot == 1 ) return textureCube( u_shadowmap1, dir ).r;
#endif
#if SIMSHADOW_CUBES > 2
	if ( slot == 2 ) return textureCube( u_shadowmap2, dir ).r;
#endif
#if SIMSHADOW_CUBES > 3
	if ( slot == 3 ) return textureCube( u_shadowmap3, dir ).r;
#endif
#if SIMSHADOW_CUBES > 4
	if ( slot == 4 ) return textureCube( u_shadowmap4, dir ).r;
#endif
#if SIMSHADOW_CUBES > 5
	if ( slot == 5 ) return textureCube( u_shadowmap5, dir ).r;
#endif
	return 1.0;
}

// distance along the axis of the cube face that \c dir points into
float face_distance( vec3 dir, vec3 offset )
{
	vec3 a = abs( dir );
	if ( a.x >= a.y && a.x >= a.z ) return abs( offset.x );
	if ( a.y >= a.z ) return abs( offset.y );
	return abs( offset.z );
}

// 1 if the point is lit as far as this cube can tell, 0 if something is in the way
float cube_tap( int slot, float farPlane, vec3 dir, vec3 offset, float bias )
{
	float d = cube_depth( slot, dir );
	// undo the perspective: eye-space distance along the face axis
	float z = 2.0 * SIMSHADOW_NEAR * farPlane / ( ( farPlane + SIMSHADOW_NEAR ) - ( 2.0 * d - 1.0 ) * ( farPlane - SIMSHADOW_NEAR ) );
	return ( face_distance( dir, offset ) - bias > z ) ? 0.0 : 1.0;
}

float shadow_cube( int slot, vec3 P, vec3 N )
{
	vec4 pf = u_shadow_pos[ slot ];
	vec3 offset = P - pf.xyz;
	float axis = max( abs( offset.x ), max( abs( offset.y ), abs( offset.z ) ) );
	if ( axis >= pf.w ) {
		return 1.0; // past what the cube was drawn to
	}

	// move the tested point off its own surface, along the normal, by about a texel
	float texel = axis * u_shadow_texel;
	offset += N * ( texel * 1.5 + 0.5 );
	float bias = 0.5 + axis * 0.002;

	// two directions across the texel grid, for a small filter
	vec3 t1 = normalize( cross( offset, abs( offset.z ) < 0.9 * length( offset ) ? vec3( 0.0, 0.0, 1.0 ) : vec3( 1.0, 0.0, 0.0 ) ) );
	vec3 t2 = normalize( cross( offset, t1 ) );
	float o = texel * 0.75;

	float lit = cube_tap( slot, pf.w, offset, offset, bias );
	lit += cube_tap( slot, pf.w, offset + t1 * o, offset, bias );
	lit += cube_tap( slot, pf.w, offset - t1 * o, offset, bias );
	lit += cube_tap( slot, pf.w, offset + t2 * o, offset, bias );
	lit += cube_tap( slot, pf.w, offset - t2 * o, offset, bias );
	return lit * 0.2;
}

float sun_tap( vec3 s, vec2 shift, float bias )
{
#if SIMSHADOW_SUN
	return ( s.z - bias > texture2D( u_sun_map, s.xy + shift ).r ) ? 0.0 : 1.0;
#else
	return 1.0;
#endif
}

float shadow_sun( vec3 P, vec3 N )
{
	float texel = u_sun_params.x;
	vec3 moved = P + N * ( texel * 1.5 + 0.5 );
	vec3 s = ( u_sun_matrix * vec4( moved, 1.0 ) ).xyz;
	if ( s.x < 0.0 || s.x > 1.0 || s.y < 0.0 || s.y > 1.0 || s.z > 1.0 ) {
		return 1.0; // outside the map that follows the camera
	}
	float bias = ( 0.5 + texel * 0.5 ) / u_sun_params.y;
	float o = u_sun_params.z * 0.75;
	float lit = sun_tap( s, vec2( 0.0, 0.0 ), bias );
	lit += sun_tap( s, vec2( o, 0.0 ), bias );
	lit += sun_tap( s, vec2( -o, 0.0 ), bias );
	lit += sun_tap( s, vec2( 0.0, o ), bias );
	lit += sun_tap( s, vec2( 0.0, -o ), bias );
	return lit * 0.2;
}

// ---------------------------------------------------------------------------
// area lights: q3map2's PointToPolygonFormFactor for a triangle

float form_factor_edge( vec3 a, vec3 b, vec3 normal )
{
	vec3 tri = cross( a, b );
	float len = length( tri );
	if ( len < 0.0001 ) {
		return 0.0;
	}
	float angle = acos( clamp( dot( a, b ), -1.0, 1.0 ) );
	return dot( normal, tri / len ) * angle;
}

float form_factor( vec3 point, vec3 normal, vec3 v0, vec3 v1, vec3 v2 )
{
	vec3 d0 = normalize( v0 - point );
	vec3 d1 = normalize( v1 - point );
	vec3 d2 = normalize( v2 - point );
	float total = form_factor_edge( d0, d1, normal ) + form_factor_edge( d1, d2, normal ) + form_factor_edge( d2, d0, normal );
	if ( total > 6.3 || total < -6.3 ) {
		return 0.0;
	}
	return total * INV_TWO_PI;
}

// ---------------------------------------------------------------------------

void main()
{
	vec4 diffuse = texture2D( u_diffusemap, gl_TexCoord[0].st );

	// a shader without cull is visible from behind: light that side as its own surface
	vec3 N = normalize( var_world_normal );
	if ( !gl_FrontFacing ) {
		N = -N;
	}

	vec3 lit = u_ambient;

	// this pixel's cluster: its tile, and the slice its distance falls in
	float slice = clamp( floor( log( max( var_eye_z, u_cluster_depth.x ) / u_cluster_depth.x ) * u_cluster_depth.y ), 0.0, u_cluster_grid.z - 1.0 );
	vec2 tile = clamp( floor( gl_FragCoord.xy / u_cluster_grid.w ), vec2( 0.0 ), u_cluster_grid.xy - 1.0 );
	float cluster = ( slice * u_cluster_grid.y + tile.y ) * u_cluster_grid.x + tile.x;
	vec4 list = fetch_texel( u_cluster_tex, cluster, u_data_rows.y );
	float first = list.x;
	int count = int( list.y + 0.5 );

	for ( int i = 0; i < SIMLIGHTS_CLUSTER_MAX; ++i ) {
		if ( i >= count ) {
			break;
		}
		float light = fetch_texel( u_index_tex, first + float( i ), u_data_rows.z ).x;
		float base = light * 8.0;

		// the fifth texel is the light's position and reach: most lights in a cluster's list are out of range of any one pixel
		vec4 reach = fetch_texel( u_light_tex, base + 4.0, u_data_rows.x );
		vec3 to_light = reach.xyz - var_world_pos;
		float dist_squared = dot( to_light, to_light );
		if ( dist_squared >= reach.w * reach.w || dist_squared <= 0.00000001 ) {
			continue;
		}

		vec4 a = fetch_texel( u_light_tex, base, u_data_rows.x );
		vec4 b = fetch_texel( u_light_tex, base + 1.0, u_data_rows.x );
		vec4 c = fetch_texel( u_light_tex, base + 2.0, u_data_rows.x );
		vec4 d = fetch_texel( u_light_tex, base + 3.0, u_data_rows.x );

		float flags = d.w;
		int slot = int( floor( flags / FLAG_KINDS ) ) - 1;

		vec3 origin = reach.xyz; // an area light's is the centre of its bounds, nudged off its plane
		vec3 colour;
		vec3 v0 = a.xyz;
		vec3 v1 = b.xyz;
		vec3 v2 = c.xyz;
		vec3 light_normal = vec3( 0.0 );
		bool area = has_flag( flags, FLAG_AREA );
		if ( area ) {
			// q3map2 winds a face clockwise seen from its front, so this points backwards
			light_normal = -normalize( cross( v1 - v0, v2 - v0 ) );
			colour = d.rgb;
		}
		else {
			colour = b.rgb;
		}

		float dist = sqrt( dist_squared );
		vec3 L = to_light / dist;

		// q3map2: a light behind the surface never reaches it
		float ndotl = dot( N, L );
		if ( ndotl < 0.0 ) {
			continue;
		}

		float add;
		if ( area ) {
			// distance in front of the light's (nudged) plane
			float front = dot( var_world_pos - origin, light_normal );
			if ( front < 3.0 ) {
				if ( front < -1.0 ) {
					continue;
				}
				// coplanar with the emitter: surfaces meeting a light do not get black edges
				if ( front > -3.0 && dot( N, light_normal ) > 0.9 ) {
					continue;
				}
			}
			// "nudge the point so that it is clearly forward of the light"
			vec3 pushed = ( front > -8.0 && front < 8.0 ) ? var_world_pos + light_normal * ( 8.0 - front ) : var_world_pos;
			float factor = form_factor( pushed, N, v0, v1, v2 );
			if ( factor <= 0.0 ) {
				continue; // behind the emitter, or nothing of it in view
			}
			add = factor * b.w;
		}
		else {
			// clamp the distance so a light on a surface does not blow out
			float eff = max( 16.0, sqrt( dist * dist + c.z * c.z ) );

			float angle = has_flag( flags, FLAG_ANGLE ) ? ndotl : 1.0;
			if ( c.y != 0.0 ) {
				angle = min( angle / c.y, 1.0 );
			}

			if ( has_flag( flags, FLAG_LINEAR ) ) {
				add = max( 0.0, angle * b.w * LINEAR_SCALE - eff * c.x );
			}
			else {
				add = b.w / ( eff * eff ) * angle;
			}

			if ( has_flag( flags, FLAG_SPOT ) ) {
				// distance along the axis; behind the light is outside the cone
				float along = dot( var_world_pos - origin, d.xyz );
				if ( along < 0.0 ) {
					continue;
				}
				float cone = c.w * along;
				float off_axis = length( var_world_pos - ( origin + d.xyz * along ) );
				if ( off_axis >= cone ) {
					continue;
				}
				if ( off_axis > cone - 32.0 ) {
					add *= ( cone - off_axis ) / 32.0;
				}
			}
		}

		if ( add <= FALLOFF_TOLERANCE ) {
			continue;
		}

		if ( slot >= 0 ) {
			add *= shadow_cube( slot, var_world_pos, N );
		}
		lit += colour * add;
	}

	// the sun: intensity falls off with angle, not distance
	if ( u_sun_on != 0 ) {
		float ndotl = dot( N, u_sun_dir );
		if ( ndotl > 0.0 ) {
			float visible = 1.0;
			if ( u_sun_shadow_on != 0 ) {
				visible = shadow_sun( var_world_pos, N );
			}
			lit += u_sun_light * ndotl * visible;
		}
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
