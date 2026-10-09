/* Bounded, streaming lossless cache sections. Headers remain seekable so the
   editor can read the preview without inflating compiler-private samples. */
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <istream>
#include <ostream>
#include <zlib.h>

struct LightCacheCompressedHeader {
	std::uint64_t magic = 0x313042494c5a4c51ULL; // QLZLIB01
	std::uint64_t rawSize{}, storedSize{};
};
static_assert( sizeof( LightCacheCompressedHeader ) == 24 );

class LightCacheWriter {
	std::ostream& file;
	const std::streampos start;
	LightCacheCompressedHeader header;
	z_stream stream{};
	std::array<unsigned char, 65536> input{}, output{};
	std::size_t buffered = 0;
	bool initialized, valid;
	bool flush( int mode ) {
		stream.next_in = input.data();
		stream.avail_in = uInt( buffered );
		int result;
		do {
			stream.next_out = output.data();
			stream.avail_out = uInt( output.size() );
			result = deflate( &stream, mode );
			if ( result != Z_OK && result != Z_STREAM_END ) return valid = false;
			const auto size = output.size() - stream.avail_out;
			file.write( reinterpret_cast<const char*>( output.data() ), size );
			header.storedSize += size;
			if ( !file ) return valid = false;
		} while ( stream.avail_in || ( mode == Z_FINISH && result != Z_STREAM_END ) );
		buffered = 0;
		return true;
	}
public:
	explicit LightCacheWriter( std::ostream& file ) : file( file ), start( file.tellp() ),
		initialized( deflateInit( &stream, Z_BEST_SPEED ) == Z_OK ), valid( initialized ) {
		file.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
		valid = valid && bool( file );
	}
	~LightCacheWriter() { if ( initialized ) deflateEnd( &stream ); }
	LightCacheWriter( const LightCacheWriter& ) = delete;
	LightCacheWriter& operator=( const LightCacheWriter& ) = delete;
	bool write( const void *data, std::size_t size ) {
		if ( !valid ) return false;
		header.rawSize += size;
		auto p = static_cast<const unsigned char*>( data );
		while ( size ) {
			const auto count = std::min( size, input.size() - buffered );
			std::memcpy( input.data() + buffered, p, count );
			buffered += count; p += count; size -= count;
			if ( buffered == input.size() && !flush( Z_NO_FLUSH ) ) return false;
		}
		return true;
	}
	bool finish() {
		if ( !valid || !flush( Z_FINISH ) ) return false;
		const auto end = file.tellp();
		file.seekp( start );
		file.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
		file.seekp( end );
		return valid = bool( file );
	}
};

class LightCacheReader {
	std::istream& file;
	z_stream stream{};
	std::array<unsigned char, 65536> input{}, output{};
	std::uint64_t remaining{}, storedRemaining{};
	std::size_t first = 0, available = 0;
	bool compressed, initialized = false, valid = false, ended = false;
	bool fill() {
		first = available = 0;
		if ( ended || !valid ) return false;
		do {
			if ( !stream.avail_in ) {
				const auto count = std::min<std::uint64_t>( storedRemaining, input.size() );
				if ( !count || !file.read( reinterpret_cast<char*>( input.data() ), count ) ) return valid = false;
				storedRemaining -= count;
				stream.next_in = input.data(); stream.avail_in = uInt( count );
			}
			stream.next_out = output.data(); stream.avail_out = uInt( output.size() );
			const auto before = stream.avail_in;
			const int result = inflate( &stream, Z_NO_FLUSH );
			available = output.size() - stream.avail_out;
			if ( result == Z_STREAM_END ) {
				ended = true;
				if ( stream.avail_in || storedRemaining ) return valid = false;
			}
			else if ( result != Z_OK || ( before == stream.avail_in && !available ) ) return valid = false;
			if ( available > remaining ) return valid = false;
		} while ( !available && !ended );
		return available != 0;
	}
public:
	LightCacheReader( std::istream& file, std::uint64_t end, std::uint64_t rawSize, bool compressed = true ) :
		file( file ), remaining( rawSize ), compressed( compressed ) {
		const auto start = file.tellg();
		if ( start < 0 || std::uint64_t( start ) > end ) return;
		if ( !compressed ) { valid = end - std::uint64_t( start ) == rawSize; return; }
		LightCacheCompressedHeader header;
		if ( end - std::uint64_t( start ) < sizeof( header ) ||
		     !file.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
		     header.magic != LightCacheCompressedHeader{}.magic || header.rawSize != rawSize ||
		     header.storedSize != end - std::uint64_t( start ) - sizeof( header ) ) return;
		storedRemaining = header.storedSize;
		valid = initialized = inflateInit( &stream ) == Z_OK;
	}
	~LightCacheReader() { if ( initialized ) inflateEnd( &stream ); }
	LightCacheReader( const LightCacheReader& ) = delete;
	LightCacheReader& operator=( const LightCacheReader& ) = delete;
	explicit operator bool() const { return valid; }
	bool read( void *data, std::size_t size ) {
		if ( !valid || size > remaining ) return false;
		if ( !compressed ) {
			remaining -= size;
			return valid = bool( file.read( static_cast<char*>( data ), size ) );
		}
		auto p = static_cast<unsigned char*>( data );
		while ( size ) {
			if ( first == available && !fill() ) return false;
			const auto count = std::min( size, available - first );
			std::memcpy( p, output.data() + first, count );
			first += count; p += count; size -= count; remaining -= count;
		}
		return true;
	}
	bool finish() {
		if ( !valid || remaining || first != available ) return false;
		if ( !compressed ) return true;
		if ( !ended ) fill(); // Consume the trailer, rejecting extra output.
		return valid && ended && first == available;
	}
};
