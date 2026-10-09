#pragma once
#include "lightcacheformat.h"
#include "lightcachecompression.h"
#include <QFile>
#include <limits>
#include <sstream>

// Read only the display payload. Compiler sample arrays can be much larger.
inline QByteArray ReadLightCachePreview( QFile& file ){
	LightCacheHeader header;
	if ( file.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) != sizeof( header ) ||
	     ( header.magic != LightCacheHeader{}.magic && header.magic != LightCacheLegacyMagic &&
	       header.magic != LightCachePreviousMagic ) ||
	     header.checksum != LightCacheHeaderChecksum( header ) ||
	     header.previewOffset < sizeof( header ) || header.previewOffset > uint64_t( file.size() ) ||
	     header.previewSize > uint64_t( file.size() ) - header.previewOffset ||
	     header.previewSize > uint64_t( std::numeric_limits<int>::max() ) ||
	     !file.seek( header.previewOffset ) ) return {};
	QByteArray data = file.read( header.previewSize );
	if ( uint64_t( data.size() ) != header.previewSize ) return {};
	if ( header.magic != LightCacheLegacyMagic ) {
		LightCacheCompressedHeader section;
		if ( data.size() < int( sizeof( section ) ) ) return {};
		memcpy( &section, data.constData(), sizeof( section ) );
		if ( section.rawSize > uint64_t( std::numeric_limits<int>::max() ) ) return {};
		std::istringstream stream( std::string( data.constData(), data.size() ), std::ios::binary );
		LightCacheReader reader( stream, data.size(), section.rawSize );
		if ( !reader ) return {};
		data.resize( int( section.rawSize ) );
		if ( !reader.read( data.data(), data.size() ) || !reader.finish() ) return {};
	}
	if ( LightCacheChecksum( data.constData(), data.size() ) != header.previewChecksum ) return {};
	return data;
}
