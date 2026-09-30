/*
   Shadow maps for the simulated map lights. See simshadows.h.
 */

#include "simshadows.h"

#include "igl.h"
#include "irender.h"
#include "iscenegraph.h"
#include "itextstream.h"
#include "renderable.h"
#include "renderer.h"
#include "view.h"
#include "map.h"
#include "largemap.h"
#include "math/frustum.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include <algorithm>
#include <cmath>
#include <vector>

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE_CUBE_MAP
#define GL_TEXTURE_CUBE_MAP 0x8513
#endif
#ifndef GL_TEXTURE_CUBE_MAP_POSITIVE_X
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#endif
#ifndef GL_TEXTURE_WRAP_R
#define GL_TEXTURE_WRAP_R 0x8072
#endif

namespace
{
constexpr int c_sunMapSize = 2048;
constexpr float c_cubeFarMax = 8192.f;
/// Half the depth the sun's orthographic box spans: the whole editable world, corner to corner.
const float c_sunHalfDepth = 1.75f * g_MaxWorldCoord;

/// What a shadow pass draws: depth only, of opaque textured fills. See OpenGLStateBucket::render.
constexpr RenderStateFlags c_shadowState = RENDER_SHADOWPASS
                                         | RENDER_DEPTHTEST
                                         | RENDER_DEPTHWRITE
                                         | RENDER_ALPHATEST
                                         | RENDER_CULLFACE
                                         | RENDER_FILL
                                         | RENDER_TEXTURE;

/// \brief Fills the shader cache's buckets with the scene as \c volume sees it, and nothing else.
class ShadowRenderer : public Renderer
{
	struct state_type
	{
		Shader* m_state = nullptr;
	};
	std::vector<state_type> m_state_stack;
public:
	ShadowRenderer(){
		m_state_stack.emplace_back();
	}
	void SetState( Shader* state, EStyle style ) override {
		if ( style == eFullMaterials ) {
			m_state_stack.back().m_state = state;
		}
	}
	EStyle getStyle() const override {
		return eFullMaterials;
	}
	void PushState() override {
		m_state_stack.push_back( m_state_stack.back() );
	}
	void PopState() override {
		m_state_stack.pop_back();
	}
	void Highlight( EHighlightMode mode, bool bEnable = true ) override {
		/* a selected brush casts what it always cast */
	}
	void addRenderable( const OpenGLRenderable& renderable, const Matrix4& world ) override {
		if ( Shader* state = m_state_stack.back().m_state ) {
			state->addRenderable( renderable, world, nullptr );
		}
	}
};

/// How far out a light's cube is drawn: as far as it shines, but the near plane needs something to be behind.
float cube_far( const SimLight& light ){
	return std::clamp( light.envelope, 16.f, c_cubeFarMax );
}

/// Right-handed look-at, GL conventions: the eye looks down -z, x is right, y is up.
Matrix4 look_at( const Vector3& eye, const Vector3& forward, const Vector3& up ){
	const Vector3 side = vector3_normalised( vector3_cross( forward, up ) );
	const Vector3 top = vector3_cross( side, forward );
	return Matrix4(
	    side.x(), top.x(), -forward.x(), 0,
	    side.y(), top.y(), -forward.y(), 0,
	    side.z(), top.z(), -forward.z(), 0,
	    -vector3_dot( side, eye ), -vector3_dot( top, eye ), vector3_dot( forward, eye ), 1 );
}

Matrix4 orthographic( float left, float right, float bottom, float top, float nearval, float farval ){
	return Matrix4(
	    2 / ( right - left ), 0, 0, 0,
	    0, 2 / ( top - bottom ), 0, 0,
	    0, 0, -2 / ( farval - nearval ), 0,
	    -( right + left ) / ( right - left ), -( top + bottom ) / ( top - bottom ), -( farval + nearval ) / ( farval - nearval ), 1 );
}

/// The orientation each cube face is drawn with, in the order GL numbers them (+x -x +y -y +z -z).
struct CubeFace
{
	Vector3 forward, up;
};
const CubeFace c_cubeFaces[6] = {
	{ Vector3( 1, 0, 0 ), Vector3( 0, -1, 0 ) },
	{ Vector3( -1, 0, 0 ), Vector3( 0, -1, 0 ) },
	{ Vector3( 0, 1, 0 ), Vector3( 0, 0, 1 ) },
	{ Vector3( 0, -1, 0 ), Vector3( 0, 0, -1 ) },
	{ Vector3( 0, 0, 1 ), Vector3( 0, -1, 0 ) },
	{ Vector3( 0, 0, -1 ), Vector3( 0, -1, 0 ) },
};

struct Cube
{
	GLuint texture = 0;
	bool valid = false;        ///< drawn at least once at the current size
	bool assigned = false;     ///< holds a light this frame
	Vector3 origin;
	float farPlane = 0;
	unsigned generation = 0;   ///< scene generation it was drawn at
};

struct SunMap
{
	GLuint texture = 0;
	bool valid = false;
	Vector3 direction;
	float range = 0;
	float lightX = 0, lightY = 0; ///< centre, in the sun's own axes
	Matrix4 matrix;
	unsigned generation = 0;
};

class Shadows
{
	QOpenGLFunctions* m_fbo = nullptr;
	GLuint m_framebuffer = 0;
	bool m_failed = false;
	bool m_pending = false;

	Cube m_cubes[c_simShadowCubesMax];
	int m_cubeSize = 0;
	SunMap m_sun;

	bool init(){
		if ( m_framebuffer != 0 ) {
			return true;
		}
		QOpenGLContext* context = QOpenGLContext::currentContext();
		if ( context == nullptr || !context->functions()->hasOpenGLFeature( QOpenGLFunctions::Framebuffers ) ) {
			return fail( "this driver has no framebuffer objects" );
		}
		m_fbo = context->functions();

		m_fbo->glGenFramebuffers( 1, &m_framebuffer );
		return m_framebuffer != 0 || fail( "cannot create a framebuffer" );
	}

	bool fail( const char* why ){
		m_failed = true;
		globalErrorStream() << "Simulated light shadows are unavailable: " << why << '\n';
		return false;
	}

	/// Storage for a cube; the contents are undefined until it is drawn.
	void allocateCube( Cube& cube, int size ){
		if ( cube.texture == 0 ) {
			gl().glGenTextures( 1, &cube.texture );
		}
		gl().glBindTexture( GL_TEXTURE_CUBE_MAP, cube.texture );
		gl().glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		gl().glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		gl().glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		gl().glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		gl().glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE );
		for ( int face = 0; face < 6; ++face ) {
			gl().glTexImage2D( GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr );
		}
		gl().glBindTexture( GL_TEXTURE_CUBE_MAP, 0 );
		cube.valid = false;
	}

	void allocateSun(){
		if ( m_sun.texture == 0 ) {
			gl().glGenTextures( 1, &m_sun.texture );
		}
		gl().glBindTexture( GL_TEXTURE_2D, m_sun.texture );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		gl().glTexImage2D( GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, c_sunMapSize, c_sunMapSize, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr );
		gl().glBindTexture( GL_TEXTURE_2D, 0 );
		m_sun.valid = false;
	}

	/// What a pass changes that the camera draw does not put back itself.
	struct SavedState
	{
		GLint framebuffer = 0;
		GLint viewport[4] = {};
	};

	SavedState begin(){
		SavedState saved;
		gl().glGetIntegerv( GL_FRAMEBUFFER_BINDING, &saved.framebuffer );
		gl().glGetIntegerv( GL_VIEWPORT, saved.viewport );
		return saved;
	}

	void end( const SavedState& saved ){
		m_fbo->glBindFramebuffer( GL_FRAMEBUFFER, GLuint( saved.framebuffer ) );
		gl().glViewport( saved.viewport[0], saved.viewport[1], saved.viewport[2], saved.viewport[3] );
		gl().glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		gl().glDepthMask( GL_TRUE );
	}

	/// Attaches \p target of \p texture as the depth buffer and empties it. False if the driver refuses.
	bool bindDepth( GLenum target, GLuint texture ){
		m_fbo->glBindFramebuffer( GL_FRAMEBUFFER, m_framebuffer );
		m_fbo->glFramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, target, texture, 0 );
		gl().glDrawBuffer( GL_NONE );
		gl().glReadBuffer( GL_NONE );
		if ( m_fbo->glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE ) {
			return false;
		}
		gl().glDepthMask( GL_TRUE );
		gl().glClear( GL_DEPTH_BUFFER_BIT );
		return true;
	}

	/// One depth-only view of the scene into whatever depth attachment is bound.
	void drawView( const Matrix4& modelview, const Matrix4& projection, std::size_t size, const Vector3& viewer ){
		View view( true );
		view.Construct( projection, modelview, size, size );

		ShadowRenderer renderer;
		Scene_Render( renderer, view );
		GlobalShaderCache().render( c_shadowState, modelview, projection, viewer );
	}

	bool drawCube( Cube& cube, int size ){
		const float nearval = c_simShadowNear;
		const Matrix4 projection = matrix4_frustum( -nearval, nearval, -nearval, nearval, nearval, cube.farPlane );

		gl().glViewport( 0, 0, size, size );
		for ( int face = 0; face < 6; ++face ) {
			if ( !bindDepth( GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, cube.texture ) ) {
				return false;
			}
			const Matrix4 modelview = look_at( cube.origin, c_cubeFaces[face].forward, c_cubeFaces[face].up );
			drawView( modelview, projection, size, cube.origin );
		}
		return true;
	}

	/// The sun's box: \p range each side of a centre snapped to the texel grid, so it does not shimmer as the camera moves.
	static Matrix4 sunMatrices( const Vector3& direction, float centreX, float centreY, float range, Matrix4& modelview, Matrix4& projection ){
		const Vector3 forward = -direction;
		const Vector3 up = std::fabs( forward.z() ) > 0.99f ? Vector3( 0, 1, 0 ) : Vector3( 0, 0, 1 );
		const Vector3 side = vector3_normalised( vector3_cross( forward, up ) );
		const Vector3 top = vector3_cross( side, forward );

		/* the eye sits at the sun's end of the box, on the line through the centre */
		const Vector3 eye = side * centreX + top * centreY - forward * c_sunHalfDepth;
		modelview = look_at( eye, forward, top );
		projection = orthographic( -range, range, -range, range, 0, 2 * c_sunHalfDepth );

		/* clip space [-1,1] to texture space [0,1] */
		const Matrix4 bias( 0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0.5f, 0, 0.5f, 0.5f, 0.5f, 1 );
		return matrix4_multiplied_by_matrix4( bias, matrix4_multiplied_by_matrix4( projection, modelview ) );
	}

public:
	bool pending() const {
		return m_pending;
	}

	void release(){
		for ( Cube& cube : m_cubes ) {
			if ( cube.texture != 0 && GlobalOpenGL().contextValid ) {
				gl().glDeleteTextures( 1, &cube.texture );
			}
			cube = Cube();
		}
		if ( m_sun.texture != 0 && GlobalOpenGL().contextValid ) {
			gl().glDeleteTextures( 1, &m_sun.texture );
		}
		m_sun = SunMap();
		if ( m_framebuffer != 0 && m_fbo != nullptr && GlobalOpenGL().contextValid ) {
			m_fbo->glDeleteFramebuffers( 1, &m_framebuffer );
		}
		m_framebuffer = 0;
		m_fbo = nullptr;
		m_cubeSize = 0;
		m_failed = false; // a new context gets a fresh attempt
	}

	void frame( SimLightsFrame& frame, const Vector3& viewer, const Vector3& viewDir ){
		m_pending = false;
		frame.cubeCount = 0;
		frame.cubeSize = 0;
		frame.sunShadowed = false;
		for ( SimLight& light : frame.lights ) {
			light.shadowSlot = -1;
		}

		if ( !g_simShadows_enabled || m_failed || !GlobalOpenGL().contextValid || !init() ) {
			return;
		}

		const unsigned generation = SimLights_generation();
		const int size = std::clamp( g_largemap_simShadowSize, 128, 2048 );
		const std::size_t wanted = std::clamp<std::size_t>( g_largemap_simShadowLights, 1, c_simShadowCubesMax );

		if ( size != m_cubeSize ) {
			for ( Cube& cube : m_cubes ) {
				if ( cube.texture != 0 ) {
					allocateCube( cube, size ); // reallocates, and forgets what it held
				}
			}
			m_cubeSize = size;
		}

		/* Which lights: the nearest, and the ones that already have a cube keep it,
		   so walking about does not redraw a cube every time the order changes. */
		struct Want
		{
			std::size_t light;
			std::size_t cube;
		};
		std::vector<Want> want;
		for ( Cube& cube : m_cubes ) {
			cube.assigned = false;
		}

		std::vector<std::size_t> lightsWithoutCube;
		for ( std::size_t i = 0; i < frame.lights.size() && want.size() + lightsWithoutCube.size() < wanted; ++i ) {
			const SimLight& light = frame.lights[i];
			const float farPlane = cube_far( light );
			std::size_t found = c_simShadowCubesMax;
			for ( std::size_t c = 0; c < c_simShadowCubesMax; ++c ) {
				const Cube& cube = m_cubes[c];
				if ( !cube.assigned && cube.texture != 0
				  && vector3_length_squared( cube.origin - light.origin ) < 0.25f && std::fabs( cube.farPlane - farPlane ) < 1.f ) {
					found = c;
					break;
				}
			}
			if ( found != c_simShadowCubesMax ) {
				m_cubes[found].assigned = true;
				want.push_back( { i, found } );
			}
			else{
				lightsWithoutCube.push_back( i );
			}
		}
		for ( std::size_t i : lightsWithoutCube ) {
			std::size_t pick = c_simShadowCubesMax;
			for ( std::size_t c = 0; c < c_simShadowCubesMax; ++c ) {
				if ( !m_cubes[c].assigned ) {
					pick = c;
					break;
				}
			}
			if ( pick == c_simShadowCubesMax ) {
				break;
			}
			Cube& cube = m_cubes[pick];
			if ( cube.texture == 0 ) {
				allocateCube( cube, size );
			}
			cube.assigned = true;
			cube.valid = false; // holds another light's picture, if anything
			cube.origin = frame.lights[i].origin;
			cube.farPlane = cube_far( frame.lights[i] );
			want.push_back( { i, pick } );
		}

		/* What needs drawing. A map never drawn is drawn now; one merely out of date
		   waits its turn, one a frame, and the old picture serves meanwhile. */
		std::vector<Cube*> draw;
		std::vector<Cube*> stale;
		for ( const Want& w : want ) {
			Cube& cube = m_cubes[w.cube];
			if ( !cube.valid ) {
				draw.push_back( &cube );
			}
			else if ( cube.generation != generation ) {
				stale.push_back( &cube );
			}
		}

		/* the sun */
		const bool sun = frame.sun.present;
		float sunX = 0, sunY = 0, sunRange = 0;
		bool drawSun = false;
		bool sunStale = false;
		if ( sun ) {
			sunRange = float( std::clamp( g_largemap_simSunShadowRange, 512, 32768 ) );
			const Vector3 forward = -frame.sun.direction;
			const Vector3 up = std::fabs( forward.z() ) > 0.99f ? Vector3( 0, 1, 0 ) : Vector3( 0, 0, 1 );
			const Vector3 side = vector3_normalised( vector3_cross( forward, up ) );
			const Vector3 top = vector3_cross( side, forward );

			/* centred a little ahead of the camera, where the eye is looking */
			const Vector3 focus = viewer + viewDir * ( sunRange * 0.4f );
			const float texel = 2 * sunRange / c_sunMapSize;
			sunX = std::floor( vector3_dot( focus, side ) / texel ) * texel;
			sunY = std::floor( vector3_dot( focus, top ) / texel ) * texel;

			if ( m_sun.texture == 0 ) {
				allocateSun();
			}
			const bool moved = std::fabs( sunX - m_sun.lightX ) > sunRange * 0.25f || std::fabs( sunY - m_sun.lightY ) > sunRange * 0.25f;
			const bool changed = !m_sun.valid || moved
			                  || vector3_length_squared( m_sun.direction - frame.sun.direction ) > 1e-8f
			                  || m_sun.range != sunRange;
			if ( changed ) {
				drawSun = true;
			}
			else if ( m_sun.generation != generation ) {
				sunStale = true;
			}
		}
		/* one out-of-date map a frame, the sun's after the cubes; whatever is left over
		   asks for another frame */
		std::size_t behind = stale.size() + ( sunStale ? 1 : 0 );
		if ( !stale.empty() ) {
			draw.push_back( stale.front() );
			--behind;
		}
		else if ( sunStale ) {
			drawSun = true;
			--behind;
		}
		m_pending = behind != 0;

		if ( !draw.empty() || drawSun ) {
			const SavedState saved = begin();

			for ( Cube* cube : draw ) {
				if ( !drawCube( *cube, size ) ) {
					end( saved );
					fail( "the driver rejected a depth-only framebuffer" );
					return;
				}
				cube->valid = true;
				cube->generation = generation;
			}

			if ( drawSun ) {
				Matrix4 modelview, projection;
				m_sun.matrix = sunMatrices( frame.sun.direction, sunX, sunY, sunRange, modelview, projection );
				gl().glViewport( 0, 0, c_sunMapSize, c_sunMapSize );
				if ( !bindDepth( GL_TEXTURE_2D, m_sun.texture ) ) {
					end( saved );
					fail( "the driver rejected a depth-only framebuffer" );
					return;
				}
				drawView( modelview, projection, c_sunMapSize, viewer );
				m_sun.valid = true;
				m_sun.generation = generation;
				m_sun.direction = frame.sun.direction;
				m_sun.range = sunRange;
				m_sun.lightX = sunX;
				m_sun.lightY = sunY;
			}

			end( saved );
			GlobalOpenGL_debugAssertNoErrors();
		}

		/* hand over what can be sampled */
		frame.cubeSize = size;
		for ( const Want& w : want ) {
			const Cube& cube = m_cubes[w.cube];
			if ( !cube.valid ) {
				continue;
			}
			SimShadowCube& out = frame.cubes[w.cube];
			out.texture = cube.texture;
			out.origin = cube.origin;
			out.farPlane = cube.farPlane;
			frame.lights[w.light].shadowSlot = int( w.cube );
			frame.cubeCount = std::max( frame.cubeCount, w.cube + 1 );
		}
		if ( sun && m_sun.valid ) {
			frame.sunShadowed = true;
			frame.sunTexture = m_sun.texture;
			frame.sunMatrix = m_sun.matrix;
			frame.sunTexel = 2 * m_sun.range / c_sunMapSize;
			frame.sunRange = 2 * c_sunHalfDepth;
		}
	}
};

Shadows g_shadows;
}

void SimShadows_frame( SimLightsFrame& frame, const Vector3& viewer, const Vector3& viewDir ){
	g_shadows.frame( frame, viewer, viewDir );
}

bool SimShadows_pending(){
	return g_shadows.pending();
}

void SimShadows_release(){
	g_shadows.release();
}
