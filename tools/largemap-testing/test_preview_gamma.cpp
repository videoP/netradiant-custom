/* Exercise the exact preview shader in a real OpenGL context. */
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QSurfaceFormat>
#include <QFile>

#include <GL/gl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

static QByteArray shaderSource( const QByteArray& code, const char* marker ){
	const int start = code.indexOf( marker );
	if ( start < 0 ) return {};
	const int body = start + int( strlen( marker ) );
	const int end = code.indexOf( ")GLSL\"", body );
	return end < 0 ? QByteArray{} : code.mid( body, end - body );
}

int main( int argc, char** argv ){
	QGuiApplication app( argc, argv );
	if ( argc != 2 ) return 2;
	QSurfaceFormat format;
	format.setVersion( 2, 1 );
	format.setProfile( QSurfaceFormat::CompatibilityProfile );
	QOpenGLContext context;
	context.setFormat( format );
	if ( !context.create() ) { std::puts( "No OpenGL context" ); return 3; }
	QOffscreenSurface surface;
	surface.setFormat( context.format() );
	surface.create();
	if ( !surface.isValid() || !context.makeCurrent( &surface ) ) { std::puts( "No offscreen surface" ); return 3; }

	QFile source( argv[1] );
	if ( !source.open( QIODevice::ReadOnly ) ) return 2;
	const QByteArray code = source.readAll();
	const QByteArray vertex = shaderSource( code, "static const char* vertex = R\"GLSL(" );
	const QByteArray fragment = shaderSource( code, "static const char* fragment = R\"GLSL(" );
	if ( vertex.isEmpty() || fragment.isEmpty() ) return 2;
	QOpenGLShaderProgram program;
	if ( !program.addShaderFromSourceCode( QOpenGLShader::Vertex, vertex ) ||
	     !program.addShaderFromSourceCode( QOpenGLShader::Fragment, fragment ) || !program.link() ) {
		std::fprintf( stderr, "%s\n", program.log().toUtf8().constData() );
		return 1;
	}

	QOpenGLFramebufferObject target( 4, 4 );
	if ( !target.isValid() || !target.bind() ) return 3;
	QOpenGLFunctions* gl = context.functions();
	GLuint textures[2]{};
	glGenTextures( 2, textures );
	for ( int i = 0; i < 2; ++i ) {
		gl->glActiveTexture( GL_TEXTURE0 + i );
		glBindTexture( GL_TEXTURE_2D, textures[i] );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		const unsigned char rgb[4] = { 128, 128, 128, 255 };
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgb );
	}
	glViewport( 0, 0, 4, 4 );
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	program.bind();
	program.setUniformValue( "u_diffuse", 0 );
	program.setUniformValue( "u_lightmap", 1 );
	for ( float scale : { 1.f, 2.f, 4.f } )
	for ( float gamma : { 1.f, 2.f } )
	for ( int lightmapped = 0; lightmapped < 2; ++lightmapped ) {
		program.setUniformValue( "u_scale", scale );
		program.setUniformValue( "u_inverse_gamma", 1.f / gamma );
		program.setUniformValue( "u_lightmapped", bool( lightmapped ) );
		glClearColor( 0, 0, 0, 1 );
		glClear( GL_COLOR_BUFFER_BIT );
		glColor3ub( 128, 128, 128 );
		glBegin( GL_TRIANGLES );
		glTexCoord2f( 0, 0 ); glVertex2f( -1, -1 );
		glTexCoord2f( 0, 0 ); glVertex2f( 3, -1 );
		glTexCoord2f( 0, 0 ); glVertex2f( -1, 3 );
		glEnd();
		unsigned char pixel[4]{};
		glReadPixels( 2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel );
		const double lit = std::min( 1.0, scale * ( 128.0 / 255.0 ) * ( 128.0 / 255.0 ) );
		const int expected = int( 255 * std::pow( lit, 1.0 / gamma ) + 0.5 );
		if ( std::abs( int( pixel[0] ) - expected ) > 2 || pixel[0] != pixel[1] || pixel[1] != pixel[2] ) {
			std::fprintf( stderr, "gamma pixel (%s, scale %.0f, gamma %.1f): %u, expected %d\n", lightmapped ? "lightmap" : "vertex", scale, gamma, pixel[0], expected );
			return 1;
		}
	}
	program.release();
	glDeleteTextures( 2, textures );
	std::puts( "PASS preview shader: 1x/2x/4x scale, neutral/adjusted gamma, lightmap and vertex colors" );
	return 0;
}
