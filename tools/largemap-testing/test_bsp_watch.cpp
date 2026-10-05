/* QtCore-only regression: no editor window or GL context needed. */
#include "radiant/bspwatch.h"
#include "radiant/lightcachepreview.h"
#include <QCoreApplication>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <cstdlib>

void check( bool condition, const char *message ){
	if ( !condition ) { std::fprintf( stderr, "FAIL: %s\n", message ); std::exit( 1 ); }
}

void events( int milliseconds ){
	QElapsedTimer timer;
	timer.start();
	do {
		QCoreApplication::processEvents();
		QThread::msleep( 5 );
	} while ( timer.elapsed() < milliseconds );
}

void write( const QString& path, const QByteArray& data ){
	QFile file( path );
	check( file.open( QIODevice::WriteOnly ), "open fixture" );
	check( file.write( data ) == data.size(), "write fixture" );
}

bool awaitUpdate( BspFileWatch& watch ){
	QElapsedTimer timer;
	timer.start();
	while ( timer.elapsed() < 2000 ) {
		events( 10 );
		if ( watch.takeUpdate() ) return true;
	}
	return false;
}

int main( int argc, char **argv ){
	QCoreApplication app( argc, argv );
	QTemporaryDir dir;
	check( dir.isValid(), "create temporary directory" );
	const QString path = dir.path() + "/preview.bsp";
	write( path, "initial BSP" );
	BspFileWatch watch( path, 100 );
	check( !watch.takeUpdate(), "initial read waits for a stable file" );
	check( awaitUpdate( watch ), "initial load" );
	const auto initial = watch.revision();
	events( 150 );
	check( !watch.takeUpdate(), "idle file is not reloaded" );
	write( dir.path() + "/unrelated.log", "log" );
	events( 150 );
	check( !watch.takeUpdate(), "unrelated directory writes are ignored" );

	// Same size and mtime: notifications must catch what stat polling cannot.
	const auto modified = QFileInfo( path ).lastModified();
	write( path, "changed BSP" );
	QFile file( path );
	check( file.open( QIODevice::ReadWrite ), "open timestamp fixture" );
	check( file.setFileTime( modified, QFileDevice::FileModificationTime ), "restore timestamp" );
	file.close();
	events( 20 );
	check( !watch.isCurrent( initial ), "discard export superseded during conversion" );
	check( !watch.takeUpdate(), "debounce a source write" );
	check( awaitUpdate( watch ), "reload same-size timestamp-preserving edit" );

	for ( int i = 0; i < 2; ++i ) {
		QSaveFile replacement( path );
		check( replacement.open( QIODevice::WriteOnly ), "open atomic replacement" );
		replacement.write( "replacement" );
		check( replacement.commit(), "commit atomic replacement" );
		check( awaitUpdate( watch ), "watch survives repeated atomic replacement" );
	}
	check( QFile::remove( path ), "remove BSP" );
	events( 150 );
	check( !watch.takeUpdate(), "missing BSP is not loaded" );
	write( path, "" );
	events( 150 );
	check( !watch.takeUpdate(), "empty BSP is not loaded" );
	write( path, "returned BSP" );
	check( awaitUpdate( watch ), "reload reappearing BSP" );

	for ( int i = 0; i < 3; ++i ) {
		watch.retry();
		check( awaitUpdate( watch ), "retry transient conversion failure" );
	}
	watch.retry();
	events( 150 );
	check( !watch.takeUpdate(), "stop retrying a permanently invalid BSP" );
	write( path, "repaired BSP" );
	check( awaitUpdate( watch ), "resume after a later successful build" );

	const QByteArray preview( "LSN1test preview" );
	LightCacheHeader header;
	header.magic = LightCacheLegacyMagic;
	header.previewOffset = sizeof( header );
	header.previewSize = preview.size();
	header.previewChecksum = LightCacheChecksum( preview.constData(), preview.size() );
	header.checksum = LightCacheHeaderChecksum( header );
	QByteArray cache( reinterpret_cast<const char*>( &header ), sizeof( header ) );
	cache += preview;
	const QString cachePath = dir.path() + "/preview.lightcache";
	write( cachePath, cache );
	QFile cacheFile( cachePath );
	check( cacheFile.open( QIODevice::ReadOnly ), "open cache preview" );
	check( ReadLightCachePreview( cacheFile ) == preview, "read preview directly without BSP" );
	cacheFile.close();
	cache[cache.size() - 1] = char( cache.back() ^ 1 );
	write( cachePath, cache );
	check( cacheFile.open( QIODevice::ReadOnly ), "open corrupt preview" );
	check( ReadLightCachePreview( cacheFile ).isEmpty(), "reject corrupt preview" );
	cacheFile.close();
	write( cachePath, cache.left( sizeof( header ) - 1 ) );
	check( cacheFile.open( QIODevice::ReadOnly ), "open incomplete cache" );
	check( ReadLightCachePreview( cacheFile ).isEmpty(), "reject incomplete cache" );
	cacheFile.close();
	std::ostringstream compressed( std::ios::binary );
	LightCacheWriter writer( compressed );
	check( writer.write( preview.constData(), preview.size() ) && writer.finish(), "compress preview fixture" );
	header.magic = LightCacheHeader{}.magic;
	header.previewSize = compressed.str().size();
	header.checksum = LightCacheHeaderChecksum( header );
	cache = QByteArray( reinterpret_cast<const char*>( &header ), sizeof( header ) );
	cache += QByteArray::fromStdString( compressed.str() );
	write( cachePath, cache );
	check( cacheFile.open( QIODevice::ReadOnly ), "open compressed preview" );
	check( ReadLightCachePreview( cacheFile ) == preview, "read compressed preview directly" );
	cacheFile.close();
	cache[cache.size() - 1] = char( cache.back() ^ 1 );
	write( cachePath, cache );
	check( cacheFile.open( QIODevice::ReadOnly ), "open corrupt compressed preview" );
	check( ReadLightCachePreview( cacheFile ).isEmpty(), "reject corrupt compressed preview" );
	cacheFile.close();
	if ( argc > 1 ) {
		QFile realCache( QString::fromLocal8Bit( argv[1] ) );
		check( realCache.open( QIODevice::ReadOnly ), "open compiler-generated cache" );
		check( ReadLightCachePreview( realCache ).startsWith( "LSN1" ), "read compiler-generated preview" );
	}
	std::puts( "PASS BSP watcher: debounce, same-size edits, stale exports, replacement, deletion, bounded retries" );
	std::puts( "PASS cache preview: compressed and legacy loading, checksums, incomplete writes" );
}
