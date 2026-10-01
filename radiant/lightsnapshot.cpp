/*
   Light snapshot. See lightsnapshot.h.
 */

#include "lightsnapshot.h"

#include "build.h"
#include "igl.h"
#include "ientity.h"
#include "ishaders.h"
#include "iscenegraph.h"
#include "itextstream.h"
#include "brush.h"
#include "map.h"
#include "texturelib.h"
#include "stringio.h"
#include "preferencesystem.h"
#include "string/string.h"
#include "math/aabb.h"

#include <QByteArray>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef GL_FOG_COORD_ARRAY
#define GL_FOG_COORD_ARRAY 0x8457
#endif
#ifndef GL_SECONDARY_COLOR_ARRAY
#define GL_SECONDARY_COLOR_ARRAY 0x845E
#endif

namespace
{
/* Preferences. Not latched: read when a snapshot is taken or drawn. */
int g_radius = 1536;
CopiedString g_bspFlags = "";
CopiedString g_lightFlags = "-fast -patchshadows -samples 2 -filter -bounce 8";
int g_brightness = 1; // index into c_brightness

constexpr int c_brightness[] = { 1, 2, 4 }; // GL_RGB_SCALE takes only these
const char* const c_brightnessNames[] = { "1x", "2x", "4x" };

/// What one line of the file holds, see tools/quake3/q3map2/light_snapshot.cpp.
struct Vert
{
	float xyz[3];
	float st[2];
	float lm[2];
	unsigned char color[4];
};
static_assert( sizeof( Vert ) == 32 );

/// The surfaces of one shader on one lightmap page, drawn in one go.
struct Batch
{
	unsigned shader;
	int lightmap; // -1: lit by vertex colour
	std::vector<Vert> verts;
	std::vector<unsigned> indexes;
};

struct Pass
{
	unsigned number = 0;
	bool final = false;
	unsigned lightmapSize = 0;
	std::vector<unsigned char> lightmapBytes;
	std::vector<std::string> shaders;
	std::vector<Batch> batches;
};

class Reader
{
	const unsigned char* m_at;
	const unsigned char* m_end;
public:
	bool m_ok = true;
	Reader( const QByteArray& data ) : m_at( reinterpret_cast<const unsigned char*>( data.constData() ) ), m_end( m_at + data.size() ){
	}
	bool take( void* out, std::size_t size ){
		if ( !m_ok || std::size_t( m_end - m_at ) < size ) {
			return m_ok = false;
		}
		std::memcpy( out, m_at, size );
		m_at += size;
		return true;
	}
	template<typename T>
	T get(){
		T value{};
		take( &value, sizeof( T ) );
		return value;
	}
	/// A view of the next \p size bytes, or nullptr.
	const unsigned char* skip( std::size_t size ){
		if ( !m_ok || std::size_t( m_end - m_at ) < size ) {
			m_ok = false;
			return nullptr;
		}
		const unsigned char* at = m_at;
		m_at += size;
		return at;
	}
};

bool parse( const QByteArray& data, Pass& pass ){
	Reader in( data );
	char magic[4] = {};
	in.take( magic, 4 );
	if ( !in.m_ok || std::memcmp( magic, "LSN1", 4 ) != 0 ) {
		return false;
	}
	pass.number = in.get<unsigned>();
	pass.final = in.get<unsigned>() != 0;
	pass.lightmapSize = in.get<unsigned>();
	const unsigned numLightmaps = in.get<unsigned>();
	const unsigned numShaders = in.get<unsigned>();
	const unsigned numSurfaces = in.get<unsigned>();
	if ( !in.m_ok || pass.lightmapSize > 4096 || numLightmaps > 4096 || numShaders > 100000 || numSurfaces > 10000000 ) {
		return false;
	}

	const std::size_t lightmapBytes = std::size_t( numLightmaps ) * pass.lightmapSize * pass.lightmapSize * 3;
	if ( const unsigned char* bytes = in.skip( lightmapBytes ) ) {
		pass.lightmapBytes.assign( bytes, bytes + lightmapBytes );
	}

	for ( unsigned i = 0; i < numShaders && in.m_ok; ++i ) {
		const unsigned length = in.get<unsigned>();
		if ( const unsigned char* name = in.skip( length ) ) {
			pass.shaders.emplace_back( reinterpret_cast<const char*>( name ), length );
		}
	}

	std::map<std::pair<unsigned, int>, std::size_t> batchOf;
	for ( unsigned s = 0; s < numSurfaces && in.m_ok; ++s ) {
		const unsigned shader = in.get<unsigned>();
		const int lightmap = in.get<int>();
		const unsigned numVerts = in.get<unsigned>();
		const unsigned numIndexes = in.get<unsigned>();
		if ( !in.m_ok || shader >= pass.shaders.size() || numVerts > 10000000 || numIndexes > 30000000 ) {
			return false;
		}

		const auto [ it, added ] = batchOf.try_emplace( { shader, lightmap }, pass.batches.size() );
		if ( added ) {
			pass.batches.push_back( { shader, lightmap, {}, {} } );
		}
		Batch& batch = pass.batches[ it->second ];

		const unsigned base = unsigned( batch.verts.size() );
		const unsigned char* verts = in.skip( std::size_t( numVerts ) * sizeof( Vert ) );
		const unsigned char* indexes = in.skip( std::size_t( numIndexes ) * sizeof( unsigned ) );
		if ( verts == nullptr || indexes == nullptr ) {
			return false;
		}
		batch.verts.resize( base + numVerts );
		std::memcpy( batch.verts.data() + base, verts, std::size_t( numVerts ) * sizeof( Vert ) );
		const std::size_t first = batch.indexes.size();
		batch.indexes.resize( first + numIndexes );
		std::memcpy( batch.indexes.data() + first, indexes, std::size_t( numIndexes ) * sizeof( unsigned ) );
		for ( std::size_t i = first; i < batch.indexes.size(); ++i ) {
			if ( batch.indexes[i] >= numVerts ) {
				return false;
			}
			batch.indexes[i] += base;
		}
	}
	return in.m_ok;
}

/* ------------------------------------------------------------------------- */

/// Which shaders mean sky, looked up once each: the sky is what the sun comes from, so it goes in whatever the box.
class SkyCache
{
	std::unordered_map<std::string, bool> m_known;
public:
	bool isSky( const char* name ){
		const auto found = m_known.find( name );
		if ( found != m_known.end() ) {
			return found->second;
		}
		bool sky = false;
		if ( IShader* shader = GlobalShaderSystem().getShaderForName( name ) ) {
			sky = ( shader->getFlags() & QER_SKY ) != 0 || shader->getLightInfo().hasSun;
			shader->DecRef();
		}
		return m_known.emplace( name, sky ).first->second;
	}
};

class SkyProbe : public BrushVisitor
{
	SkyCache& m_cache;
public:
	mutable bool m_sky = false;
	SkyProbe( SkyCache& cache ) : m_cache( cache ){
	}
	void visit( Face& face ) const override {
		if ( !m_sky && m_cache.isSky( face.getShader().getShader() ) ) {
			m_sky = true;
		}
	}
};

/// The nodes a snapshot is made of: everything touching the box, and every brush of sky.
class BoxCollector : public scene::Graph::Walker
{
	const AABB& m_box;
	std::unordered_set<scene::Node*>& m_nodes;
	SkyCache& m_sky;
public:
	BoxCollector( const AABB& box, std::unordered_set<scene::Node*>& nodes, SkyCache& sky ) : m_box( box ), m_nodes( nodes ), m_sky( sky ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( path.size() <= 1 ) {
			return true;
		}
		scene::Node& node = path.top();

		if ( path.size() == 2 ) { // an entity
			const Entity* entity = Node_getEntity( node );
			if ( entity != nullptr && string_equal( entity->getClassName(), "worldspawn" ) ) {
				m_nodes.insert( &node );
				return true;
			}
			if ( aabb_intersects_aabb( instance.worldAABB(), m_box ) ) {
				m_nodes.insert( &node );
				return true;
			}
			return false;
		}

		if ( path.size() == 3 ) { // a primitive
			bool take = aabb_intersects_aabb( instance.worldAABB(), m_box );
			if ( !take ) {
				if ( BrushInstance* brush = Instance_getBrush( instance ) ) {
					const SkyProbe probe( m_sky );
					brush->getBrush().forEachFace( probe );
					take = probe.m_sky;
				}
			}
			if ( take ) {
				m_nodes.insert( &node );
			}
		}
		return false;
	}
};

/* ------------------------------------------------------------------------- */

struct Job
{
	std::function<void()> redraw;
	QString dir;
	QString mapFile, bspFile, lsnFile, logFile;
	std::unique_ptr<QProcess> process;
	std::unique_ptr<QTimer> poll;
	QDateTime loadedTime;
	qint64 loadedSize = -1;
	bool running = false;
	bool failed = false;
	bool lightStage = false; ///< the -meta stage is over: the log now says how far the lighting is
	bool fromBsp = false;    ///< not a snapshot of the map: a compiled bsp being read, see LightSnap_startBSP
	int bounces = 0;
	QByteArray key; ///< what this snapshot is made from, see g_cachedKey
};

std::unique_ptr<Job> g_job;
std::shared_ptr<Pass> g_pass;     // what is on screen
std::shared_ptr<Pass> g_incoming; // parsed, not yet uploaded
/* The last snapshot to finish, and what it was made from (the map it was given
   and the commands that lit it). Taking another from identical input is pointless:
   the answer is the same, so it is shown again instead. Survives switching off. */
std::shared_ptr<Pass> g_cached;
QByteArray g_cachedKey;
std::vector<IShader*> g_shaders;  // g_pass's, same order
std::vector<unsigned> g_lightmapTextures;
std::string g_status;

void release_shaders(){
	for ( IShader* shader : g_shaders ) {
		if ( shader != nullptr ) {
			shader->DecRef();
		}
	}
	g_shaders.clear();
}

void clear_pass(){
	g_pass.reset();
	g_incoming.reset();
	release_shaders();
}

void set_status( const std::string& text ){
	g_status = text;
}

int bounces_in( const char* flags ){
	std::istringstream words( flags );
	std::string word;
	while ( words >> word ) {
		if ( word == "-bounce" && ( words >> word ) ) {
			return std::max( 0, atoi( word.c_str() ) );
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------- */

/// The panel over the camera while a snapshot builds: how far it is, and a way out.
class Overlay : public QWidget
{
public:
	QLabel* m_label;
	QProgressBar* m_bar;
	Overlay( QWidget* parent, const std::function<void()>& cancel ) : QWidget( parent ){
		setObjectName( "lightSnapOverlay" );
		setAttribute( Qt::WA_StyledBackground );
		setStyleSheet( "#lightSnapOverlay { background: rgba(24, 24, 24, 220); border: 1px solid #707070; }"
		               "#lightSnapOverlay QLabel { color: white; }" );

		m_label = new QLabel( this );
		m_bar = new QProgressBar( this );
		m_bar->setRange( 0, 100 );
		m_bar->setTextVisible( true );
		auto* button = new QPushButton( "Cancel", this );
		button->setToolTip( "Stop the snapshot (the toolbar button does the same)" );
		QObject::connect( button, &QPushButton::clicked, this, [cancel](){
			cancel();
		} );

		auto* row = new QHBoxLayout;
		row->addWidget( m_bar, 1 );
		row->addWidget( button );
		auto* column = new QVBoxLayout( this );
		column->setContentsMargins( 8, 6, 8, 8 );
		column->setSpacing( 4 );
		column->addWidget( m_label );
		column->addLayout( row );

		setFixedWidth( 400 );
		move( 8, 8 );
		hide();
	}
};

QPointer<Overlay> g_overlay;

/// What a stage of q3map2's -light is worth, as a share of its pass. The numbers are rough: they only have to keep the bar moving the right way.
struct StageShare
{
	const char* name;
	double start, share;
};
constexpr StageShare c_directStages[] = {
	{ "TraceGrid", 0.00, 0.10 }, { "MapRawLightmap", 0.10, 0.10 }, { "FloodlightRawLightmap", 0.20, 0.05 },
	{ "IlluminateRawLightmap", 0.25, 0.65 }, { "IlluminateVertexes", 0.90, 0.10 },
};
constexpr StageShare c_bounceStages[] = {
	{ "BounceGrid", 0.00, 0.10 }, { "IlluminateRawLightmap", 0.10, 0.80 }, { "IlluminateVertexes", 0.90, 0.10 },
};

/// How far the lighting is, 0..1, read from the end of q3map2's log; -1 if there is nothing to read yet.
double light_progress( const Job& job, QString& text ){
	QFile log( job.logFile );
	if ( !log.open( QIODevice::ReadOnly ) ) {
		return -1;
	}
	const qint64 window = 16384;
	log.seek( std::max<qint64>( 0, log.size() - window ) );
	const QString tail = QString::fromLatin1( log.readAll() );

	static const QRegularExpression bounceRe( R"(\(bounce (\d+) of (\d+)\))" );
	static const QRegularExpression stageRe( R"(--- (\w+) ---)" );

	int pass = 0, passes = job.bounces, bounceEnd = -1;
	for ( auto it = bounceRe.globalMatch( tail ); it.hasNext(); ) {
		const QRegularExpressionMatch m = it.next();
		pass = m.captured( 1 ).toInt();
		passes = m.captured( 2 ).toInt();
		bounceEnd = m.capturedEnd();
	}

	QRegularExpressionMatch last;
	int lastEnd = -1;
	for ( auto it = stageRe.globalMatch( tail ); it.hasNext(); ) {
		last = it.next();
		lastEnd = last.capturedEnd();
	}

	double within = 0, stageFraction = 0;
	/* a stage header from before this pass began belongs to the pass before it */
	if ( lastEnd >= 0 && lastEnd > bounceEnd ) {
		const QString name = last.captured( 1 );
		/* the pacifier: a digit every tenth, dots between, "(n)" when the stage is over */
		double fraction = 0;
		int dots = 0;
		bool finished = false;
		for ( int i = lastEnd; i < tail.size() && !finished; ++i ) {
			const QChar c = tail[i];
			if ( c.isDigit() ) {
				fraction = c.digitValue() / 10.0;
				dots = 0;
			}
			else if ( c == '.' ) {
				fraction = std::min( 1.0, fraction + ( dots < 3 ? 0.025 : 0.0 ) );
				++dots;
			}
			else if ( c == '(' ) {
				finished = true;
			}
			else if ( c == '-' ) {
				break; // the next header
			}
		}
		stageFraction = finished ? 1.0 : fraction;

		if ( name == "StoreSurfaceLightmaps" ) {
			within = 1;
		}
		else {
			const auto* table = pass == 0 ? c_directStages : c_bounceStages;
			const std::size_t count = pass == 0 ? std::size( c_directStages ) : std::size( c_bounceStages );
			for ( std::size_t i = 0; i < count; ++i ) {
				if ( name == table[i].name ) {
					within = table[i].start + table[i].share * stageFraction;
				}
			}
		}
	}

	const int total = std::max( passes, 0 ) + 1;
	text = pass == 0 ? QString( "direct light" ) : QString( "bounce %1 of %2" ).arg( pass ).arg( passes );
	return std::clamp( ( pass + within ) / total, 0.0, 1.0 );
}

/// Shows, updates or hides the panel to match the job.
void update_overlay(){
	if ( g_overlay == nullptr ) {
		return;
	}
	if ( g_job == nullptr || !g_job->running ) {
		g_overlay->hide();
		return;
	}
	if ( g_job->fromBsp ) {
		g_overlay->m_label->setText( "Loading " + QFileInfo( g_job->mapFile ).fileName() + "..." );
		g_overlay->m_bar->setRange( 0, 0 ); // busy
	}
	else if ( !g_job->lightStage ) {
		g_overlay->m_label->setText( "Light snapshot: preparing the map around the camera..." );
		g_overlay->m_bar->setRange( 0, 0 ); // busy
	}
	else {
		QString what;
		const double progress = light_progress( *g_job, what );
		g_overlay->m_bar->setRange( 0, 100 );
		g_overlay->m_bar->setValue( progress < 0 ? 0 : int( progress * 100 + 0.5 ) );
		g_overlay->m_label->setText( "Light snapshot: " + ( progress < 0 ? QString( "starting q3map2..." ) : what ) );
	}
	if ( !g_overlay->isVisible() ) {
		g_overlay->show();
		g_overlay->raise();
	}
}

void describe(){
	update_overlay();
	if ( g_job == nullptr ) {
		set_status( "" );
		return;
	}
	std::ostringstream text;
	text << ( g_job->fromBsp ? "BSP lighting: " : "Light snapshot: " );
	if ( g_job->failed ) {
		text << "failed, see " << g_job->logFile.toStdString();
	}
	else if ( g_pass == nullptr && g_incoming == nullptr ) {
		text << ( g_job->running ? "building the map around the camera..." : "waiting for q3map2..." );
	}
	else if ( g_job->fromBsp ) {
		text << QFileInfo( g_job->mapFile ).fileName().toStdString();
	}
	else {
		const Pass& pass = g_incoming != nullptr ? *g_incoming : *g_pass;
		if ( pass.number == 0 ) {
			text << "direct light";
		}
		else {
			text << "bounce " << pass.number;
			if ( g_job->bounces != 0 ) {
				text << " of " << g_job->bounces;
			}
		}
		text << ( pass.final ? ", done" : ", computing..." );
	}
	set_status( text.str() );
}

/// Takes a newer snapshot file, if there is one.
void poll_file(){
	if ( g_job == nullptr ) {
		return;
	}
	update_overlay(); // the log has moved on since the last tick
	const QFileInfo info( g_job->lsnFile );
	if ( !info.exists() ) {
		return;
	}
	if ( info.lastModified() == g_job->loadedTime && info.size() == g_job->loadedSize ) {
		return;
	}

	QFile file( g_job->lsnFile );
	if ( !file.open( QIODevice::ReadOnly ) ) {
		return; // try again next tick
	}
	const QByteArray data = file.readAll();
	file.close();

	auto pass = std::make_shared<Pass>();
	if ( !parse( data, *pass ) ) {
		return; // half written, if it ever is; the next tick will see it whole
	}
	g_job->loadedTime = info.lastModified();
	g_job->loadedSize = info.size();

	if ( pass->final && !g_job->fromBsp ) {
		g_cached = pass;
		g_cachedKey = g_job->key;
	}
	g_incoming = std::move( pass );
	describe();
	if ( g_job->redraw ) {
		g_job->redraw();
	}
}

void finish_stage( int exitCode, QProcess::ExitStatus status, bool light );

void start_stage( const QString& command, bool light ){
	Job& job = *g_job;
	/* the previous stage's process is still emitting the signal that brought us here */
	if ( job.process != nullptr ) {
		job.process.release()->deleteLater();
	}
	job.process = std::make_unique<QProcess>();
	job.process->setProcessChannelMode( QProcess::MergedChannels );
	job.process->setStandardOutputFile( job.logFile, QIODevice::Append );

	Job* const self = g_job.get();
	QObject::connect( job.process.get(), QOverload<int, QProcess::ExitStatus>::of( &QProcess::finished ), job.process.get(),
	                  [self, light]( int exitCode, QProcess::ExitStatus status ){
		if ( g_job.get() == self ) {
			finish_stage( exitCode, status, light );
		}
	} );
	QObject::connect( job.process.get(), &QProcess::errorOccurred, job.process.get(), [self]( QProcess::ProcessError error ){
		if ( g_job.get() == self && error == QProcess::FailedToStart ) {
			self->running = false;
			self->failed = true;
			globalErrorStream() << "Light snapshot: q3map2 failed to start\n";
			describe();
			if ( self->redraw ) {
				self->redraw();
			}
		}
	} );

	QFile log( job.logFile );
	if ( log.open( QIODevice::Append | QIODevice::Text ) ) {
		log.write( ( "\n> " + command + "\n" ).toUtf8() );
	}
	log.close();

	job.process->start( command );
}

CopiedString expand( const char* stage, const char* flags, const QString& extra, const Job& job ){
	const std::string command = std::string( "[q3map2] " ) + stage + ' ' + flags + ' ' + extra.toStdString()
	                          + " \"" + job.mapFile.toStdString() + '"';
	return build_expand_command( command.c_str(), job.mapFile.toUtf8().constData(), job.bspFile.toUtf8().constData() );
}

void finish_stage( int exitCode, QProcess::ExitStatus status, bool light ){
	Job& job = *g_job;
	if ( status != QProcess::NormalExit || exitCode != 0 ) {
		job.running = false;
		job.failed = true;
		globalErrorStream() << "Light snapshot: q3map2 " << ( light ? "-light" : "-meta" ) << " stage failed, see " << job.logFile.toUtf8().constData() << '\n';
	}
	else if ( !light ) {
		job.lightStage = true;
		start_stage( QString::fromUtf8( expand( "-light", g_lightFlags.c_str(), "-lightsnap \"" + job.lsnFile + "\"", job ).c_str() ), true );
		describe();
		return;
	}
	else {
		job.running = false;
		poll_file(); // the last pass can land between two ticks
		globalOutputStream() << "Light snapshot: done\n";
	}
	describe();
	if ( job.redraw ) {
		job.redraw();
	}
}

/* ------------------------------------------------------------------------- */

void upload_lightmaps( const Pass& pass ){
	if ( !g_lightmapTextures.empty() ) {
		gl().glDeleteTextures( GLsizei( g_lightmapTextures.size() ), g_lightmapTextures.data() );
		g_lightmapTextures.clear();
	}
	const std::size_t pageBytes = std::size_t( pass.lightmapSize ) * pass.lightmapSize * 3;
	if ( pageBytes == 0 ) {
		return;
	}
	const std::size_t pages = pass.lightmapBytes.size() / pageBytes;
	g_lightmapTextures.assign( pages, 0 );
	gl().glGenTextures( GLsizei( pages ), g_lightmapTextures.data() );

	GLint unpack = 4;
	gl().glGetIntegerv( GL_UNPACK_ALIGNMENT, &unpack );
	gl().glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	for ( std::size_t i = 0; i < pages; ++i ) {
		gl().glBindTexture( GL_TEXTURE_2D, g_lightmapTextures[i] );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		gl().glTexImage2D( GL_TEXTURE_2D, 0, GL_RGB, pass.lightmapSize, pass.lightmapSize, 0, GL_RGB, GL_UNSIGNED_BYTE, pass.lightmapBytes.data() + i * pageBytes );
	}
	gl().glPixelStorei( GL_UNPACK_ALIGNMENT, unpack );
	gl().glBindTexture( GL_TEXTURE_2D, 0 );
}

/// A texture unit's environment: \p previous times the unit's own texture (or the vertex colour), scaled.
void combine( GLenum unit, bool withPrimaryColour, int scale ){
	gl().glActiveTexture( unit );
	gl().glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );
	gl().glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE );
	gl().glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_RGB, withPrimaryColour ? GL_TEXTURE : GL_PREVIOUS );
	gl().glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR );
	gl().glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_RGB, withPrimaryColour ? GL_PRIMARY_COLOR : GL_TEXTURE );
	gl().glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR );
	gl().glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE );
	gl().glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, withPrimaryColour ? GL_TEXTURE : GL_PREVIOUS );
	gl().glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA );
	gl().glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, float( scale ) );
}

void restore_unit( GLenum unit ){
	gl().glActiveTexture( unit );
	gl().glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, 1.f );
	gl().glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
}
}

/* ------------------------------------------------------------------------- */

void LightSnap_stop(){
	if ( g_job != nullptr ) {
		if ( g_job->process != nullptr ) {
			g_job->process->disconnect();
			g_job->process->kill();
			g_job->process->waitForFinished( 2000 );
		}
		g_job.reset();
	}
	if ( g_overlay != nullptr ) {
		g_overlay->hide();
	}
	clear_pass();
	set_status( "" );
}

void LightSnap_start( const Vector3& origin, std::function<void()> redraw, QWidget* parent, std::function<void()> cancel ){
	LightSnap_stop();

	if ( g_overlay != nullptr && g_overlay->parentWidget() != parent ) {
		delete g_overlay;
	}
	if ( g_overlay == nullptr && parent != nullptr ) {
		g_overlay = new Overlay( parent, cancel );
	}

	const QString dir = QStandardPaths::writableLocation( QStandardPaths::TempLocation ) + "/netradiant_lightsnap";
	if ( !QDir().mkpath( dir ) ) {
		globalErrorStream() << "Light snapshot: cannot create " << dir.toUtf8().constData() << '\n';
		return;
	}

	auto job = std::make_unique<Job>();
	job->redraw = std::move( redraw );
	job->dir = dir;
	job->mapFile = dir + "/snap.map";
	job->bspFile = dir + "/snap.bsp";
	job->lsnFile = dir + "/snap.lsn";
	job->logFile = dir + "/snap.log";
	job->bounces = bounces_in( g_lightFlags.c_str() );
	for ( const char* name : { "snap.bsp", "snap.lsn", "snap.lsn.tmp", "snap.prt", "snap.srf", "snap.log", "snap.map" } ) {
		QFile::remove( dir + '/' + name );
	}

	/* the part of the map to light */
	const float radius = float( std::max( g_radius, 256 ) );
	const AABB box( origin, Vector3( radius, radius, radius ) );
	std::unordered_set<scene::Node*> nodes;
	{
		SkyCache sky;
		GlobalSceneGraph().traverse( BoxCollector( box, nodes, sky ) );
	}
	if ( !Map_SaveNodes( job->mapFile.toUtf8().constData(), nodes ) ) {
		globalErrorStream() << "Light snapshot: could not write " << job->mapFile.toUtf8().constData() << '\n';
		return;
	}
	globalOutputStream() << "Light snapshot: " << nodes.size() << " nodes within " << g_radius << " units of the camera, in " << job->dir.toUtf8().constData() << '\n';

	const CopiedString metaCommand = expand( "-meta", g_bspFlags.c_str(), "", *job );
	const CopiedString lightCommand = expand( "-light", g_lightFlags.c_str(), "-lightsnap \"" + job->lsnFile + "\"", *job );
	{
		QFile written( job->mapFile );
		if ( written.open( QIODevice::ReadOnly ) ) {
			QCryptographicHash hash( QCryptographicHash::Sha1 );
			hash.addData( written.readAll() );
			hash.addData( QByteArray( metaCommand.c_str() ) );
			hash.addData( QByteArray( lightCommand.c_str() ) );
			job->key = hash.result();
		}
	}
	if ( !job->key.isEmpty() && g_cached != nullptr && job->key == g_cachedKey ) {
		globalOutputStream() << "Light snapshot: nothing it depends on has changed, showing the last one\n";
		g_incoming = g_cached;
		set_status( "Light snapshot: unchanged since last time, shown again" );
		if ( job->redraw ) {
			job->redraw();
		}
		return; // no job: nothing to run
	}

	job->running = true;
	g_job = std::move( job );

	g_job->poll = std::make_unique<QTimer>();
	g_job->poll->setInterval( 250 );
	QObject::connect( g_job->poll.get(), &QTimer::timeout, g_job->poll.get(), &poll_file );
	g_job->poll->start();

	describe();
	start_stage( QString::fromUtf8( metaCommand.c_str() ), false );
}

bool LightSnap_startBSP( std::function<void()> redraw, QWidget* parent, std::function<void()> cancel ){
	/* the map's own compile is the likeliest wanted: the build menu puts it next to the .map */
	QString start;
	if ( !Map_Unnamed( g_map ) ) {
		const QFileInfo mapInfo( QString::fromUtf8( Map_Name( g_map ) ) );
		start = mapInfo.absolutePath() + '/' + mapInfo.completeBaseName() + ".bsp";
	}
	const QString bsp = QFileDialog::getOpenFileName( parent, "Load a compiled BSP's lighting", start, "BSP (*.bsp);;All files (*)" );
	if ( bsp.isEmpty() ) {
		return false;
	}

	LightSnap_stop();

	if ( g_overlay != nullptr && g_overlay->parentWidget() != parent ) {
		delete g_overlay;
	}
	if ( g_overlay == nullptr && parent != nullptr ) {
		g_overlay = new Overlay( parent, cancel );
	}

	const QString dir = QStandardPaths::writableLocation( QStandardPaths::TempLocation ) + "/netradiant_lightsnap";
	if ( !QDir().mkpath( dir ) ) {
		globalErrorStream() << "BSP lighting: cannot create " << dir.toUtf8().constData() << '\n';
		return false;
	}

	auto job = std::make_unique<Job>();
	job->redraw = std::move( redraw );
	job->dir = dir;
	job->mapFile = bsp;
	job->bspFile = bsp;
	job->lsnFile = dir + "/bsp.lsn";
	job->logFile = dir + "/bsp.log";
	job->fromBsp = true;
	for ( const char* name : { "bsp.lsn", "bsp.lsn.tmp", "bsp.log" } ) {
		QFile::remove( dir + '/' + name );
	}

	const CopiedString command = expand( "-lightsnapbsp", "", "-lightsnap \"" + job->lsnFile + "\"", *job );
	globalOutputStream() << "BSP lighting: reading " << bsp.toUtf8().constData() << '\n';

	job->running = true;
	g_job = std::move( job );

	g_job->poll = std::make_unique<QTimer>();
	g_job->poll->setInterval( 250 );
	QObject::connect( g_job->poll.get(), &QTimer::timeout, g_job->poll.get(), &poll_file );
	g_job->poll->start();

	describe();
	start_stage( QString::fromUtf8( command.c_str() ), true ); // one stage, and it is the last
	return true;
}

bool LightSnap_active(){
	return g_pass != nullptr || g_incoming != nullptr;
}

const char* LightSnap_status(){
	return g_status.c_str();
}

void LightSnap_unrealise(){
	release_shaders();
}

void LightSnap_release(){
	if ( !g_lightmapTextures.empty() && GlobalOpenGL().contextValid ) {
		gl().glDeleteTextures( GLsizei( g_lightmapTextures.size() ), g_lightmapTextures.data() );
	}
	g_lightmapTextures.clear();
}

void LightSnap_draw( const Matrix4& modelview, const Matrix4& projection ){
	if ( g_incoming != nullptr ) {
		g_pass = std::move( g_incoming );
		upload_lightmaps( *g_pass );
		release_shaders();
		for ( const std::string& name : g_pass->shaders ) {
			g_shaders.push_back( GlobalShaderSystem().getShaderForName( name.c_str() ) );
		}
	}
	if ( g_pass == nullptr ) {
		return;
	}
	if ( g_shaders.empty() ) { // let go of at unrealise
		for ( const std::string& name : g_pass->shaders ) {
			g_shaders.push_back( GlobalShaderSystem().getShaderForName( name.c_str() ) );
		}
	}

	const int scale = c_brightness[ std::clamp( g_brightness, 0, int( std::size( c_brightness ) ) - 1 ) ];

	gl().glMatrixMode( GL_PROJECTION );
	gl().glLoadMatrixf( reinterpret_cast<const float*>( &projection ) );
	gl().glMatrixMode( GL_MODELVIEW );
	gl().glLoadMatrixf( reinterpret_cast<const float*>( &modelview ) );

	/* the scene is already there at these very depths: win ties, and a little more */
	gl().glEnable( GL_DEPTH_TEST );
	gl().glDepthFunc( GL_LEQUAL );
	gl().glDepthMask( GL_TRUE );
	gl().glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	gl().glEnable( GL_POLYGON_OFFSET_FILL );
	gl().glPolygonOffset( -2, -2 );
	gl().glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
	gl().glDisable( GL_CULL_FACE );
	gl().glDisable( GL_LIGHTING );
	gl().glDisable( GL_BLEND );
	gl().glDisable( GL_FOG );
	gl().glEnable( GL_ALPHA_TEST );
	gl().glAlphaFunc( GL_GREATER, 0.25f );
	gl().glUseProgram( 0 );
	gl().glColor4f( 1, 1, 1, 1 );

	/* The scene pass leaves whatever arrays its last bucket used switched on, pointing at
	   memory that has since moved (the camera's own 2D drawing clears them, but that comes
	   after this). A draw with one of those enabled makes the driver read from it. */
	gl().glBindBuffer( GL_ARRAY_BUFFER, 0 );
	gl().glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	for ( GLenum unit = GL_TEXTURE0; unit < GL_TEXTURE0 + 8; ++unit ) {
		gl().glClientActiveTexture( unit );
		gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	}
	gl().glClientActiveTexture( GL_TEXTURE0 );
	for ( GLenum array : { GL_NORMAL_ARRAY, GL_COLOR_ARRAY, GL_INDEX_ARRAY, GL_EDGE_FLAG_ARRAY, GL_SECONDARY_COLOR_ARRAY, GL_FOG_COORD_ARRAY } ) {
		gl().glDisableClientState( array );
	}
	for ( GLuint attribute = 1; attribute < 16; ++attribute ) {
		gl().glDisableVertexAttribArray( attribute );
	}
	gl().glEnableClientState( GL_VERTEX_ARRAY );

	bool lightmapped = false, first = true;
	for ( const Batch& batch : g_pass->batches ) {
		const bool useLightmap = batch.lightmap >= 0 && std::size_t( batch.lightmap ) < g_lightmapTextures.size();

		GLuint diffuse = 0;
		if ( batch.shader < g_shaders.size() && g_shaders[ batch.shader ] != nullptr ) {
			if ( const qtexture_t* texture = g_shaders[ batch.shader ]->getTexture() ) {
				diffuse = texture->texture_number;
			}
		}

		if ( first || useLightmap != lightmapped ) {
			first = false;
			lightmapped = useLightmap;
			if ( useLightmap ) {
				gl().glDisableClientState( GL_COLOR_ARRAY );
				gl().glActiveTexture( GL_TEXTURE0 );
				gl().glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
				gl().glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, 1.f );
				gl().glActiveTexture( GL_TEXTURE1 );
				gl().glEnable( GL_TEXTURE_2D );
				combine( GL_TEXTURE1, false, scale );
			}
			else {
				gl().glActiveTexture( GL_TEXTURE1 );
				gl().glDisable( GL_TEXTURE_2D );
				gl().glClientActiveTexture( GL_TEXTURE1 );
				gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
				gl().glEnableClientState( GL_COLOR_ARRAY );
				combine( GL_TEXTURE0, true, scale );
			}
		}

		const Vert* verts = batch.verts.data();
		gl().glClientActiveTexture( GL_TEXTURE0 );
		gl().glActiveTexture( GL_TEXTURE0 );
		gl().glEnable( GL_TEXTURE_2D );
		gl().glBindTexture( GL_TEXTURE_2D, diffuse );
		gl().glEnableClientState( GL_TEXTURE_COORD_ARRAY );
		gl().glTexCoordPointer( 2, GL_FLOAT, sizeof( Vert ), verts->st );
		if ( useLightmap ) {
			gl().glClientActiveTexture( GL_TEXTURE1 );
			gl().glActiveTexture( GL_TEXTURE1 );
			gl().glBindTexture( GL_TEXTURE_2D, g_lightmapTextures[ batch.lightmap ] );
			gl().glEnableClientState( GL_TEXTURE_COORD_ARRAY );
			gl().glTexCoordPointer( 2, GL_FLOAT, sizeof( Vert ), verts->lm );
		}
		else {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( Vert ), verts->color );
		}
		gl().glVertexPointer( 3, GL_FLOAT, sizeof( Vert ), verts->xyz );
		gl().glDrawElements( GL_TRIANGLES, GLsizei( batch.indexes.size() ), GL_UNSIGNED_INT, batch.indexes.data() );
	}

	/* hand the state back as the camera's own 2D drawing expects to find it */
	gl().glDisableClientState( GL_COLOR_ARRAY );
	gl().glClientActiveTexture( GL_TEXTURE1 );
	gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	gl().glActiveTexture( GL_TEXTURE1 );
	gl().glBindTexture( GL_TEXTURE_2D, 0 );
	gl().glDisable( GL_TEXTURE_2D );
	restore_unit( GL_TEXTURE1 );
	gl().glClientActiveTexture( GL_TEXTURE0 );
	gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	gl().glActiveTexture( GL_TEXTURE0 );
	gl().glBindTexture( GL_TEXTURE_2D, 0 );
	gl().glDisable( GL_TEXTURE_2D );
	restore_unit( GL_TEXTURE0 );
	gl().glDisable( GL_ALPHA_TEST );
	gl().glDisable( GL_POLYGON_OFFSET_FILL );
	gl().glDepthFunc( GL_LESS );
}

void LightSnap_constructPreferences( PreferencesPage& page ){
	page.appendSpinner(
	    "Light snapshot: radius", g_radius, 256, 32768
	)->setToolTip(
	    "How far around the camera the snapshot reaches, in units each way. Only\n"
	    "the brushes, patches and entities in that box (and the map's sky) are given\n"
	    "to q3map2. Smaller is quicker; larger lights more of what you can see.\n\n"
	    "Light from outside the box, and light bounced off things outside it, is\n"
	    "missing near the box's edge."
	);
	page.appendEntry( "Light snapshot: -meta options", g_bspFlags )->setToolTip(
	    "Extra options for the q3map2 -meta stage of a snapshot, for example\n"
	    "-samplesize 16. The tool, game and paths come from the build menu."
	);
	page.appendEntry( "Light snapshot: -light options", g_lightFlags )->setToolTip(
	    "Options for the q3map2 -light stage of a snapshot: use the ones your final\n"
	    "compile does. Each bounce is shown as it finishes, so -bounce 8 costs\n"
	    "nothing but time before the end."
	);
	page.appendCombo( "Light snapshot: brightness", g_brightness, StringArrayRange( c_brightnessNames ) )->setToolTip(
	    "How much the lightmaps are multiplied by when drawn, to match the game's\n"
	    "overbright setting. Lightmaps drawn too dark: raise it."
	);
}

void LightSnap_registerPreferences(){
	GlobalPreferenceSystem().registerPreference( "LightSnapRadius", IntImportStringCaller( g_radius ), IntExportStringCaller( g_radius ) );
	GlobalPreferenceSystem().registerPreference( "LightSnapMetaFlags", CopiedStringImportStringCaller( g_bspFlags ), CopiedStringExportStringCaller( g_bspFlags ) );
	GlobalPreferenceSystem().registerPreference( "LightSnapLightFlags", CopiedStringImportStringCaller( g_lightFlags ), CopiedStringExportStringCaller( g_lightFlags ) );
	GlobalPreferenceSystem().registerPreference( "LightSnapBrightness", IntImportStringCaller( g_brightness ), IntExportStringCaller( g_brightness ) );
}
