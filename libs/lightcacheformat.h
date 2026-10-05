/* Shared container header. Compressed compiler samples precede a separately
   compressed LSN1 preview (previewOffset/Size refer to its stored section).
   The preview can be read without loading a BSP or starting q3map2. */
#pragma once
#include <cstddef>
#include <cstdint>

struct LightCacheHeader {
	std::uint64_t magic = 0x3348434143544c51ULL; // QLTCACH3
	std::uint64_t settings{}, resources{}, geometry{}, map{}, payload{}, checksum{};
	std::uint64_t charts{}, grid{}, pages{};
	std::uint64_t previewOffset{}, previewSize{}, previewChecksum{};
};
static_assert( sizeof( LightCacheHeader ) == 104 );
inline constexpr std::uint64_t LightCacheLegacyMagic = 0x3248434143544c51ULL;

inline std::uint64_t LightCacheChecksum( const void *data, std::size_t size ){
	std::uint64_t hash = 14695981039346656037ULL;
	for ( auto p = static_cast<const unsigned char*>( data ); size; --size, ++p )
		hash = ( hash ^ *p ) * 1099511628211ULL;
	return hash;
}

inline std::uint64_t LightCacheHeaderChecksum( LightCacheHeader header ){
	header.checksum = 0;
	return LightCacheChecksum( &header, sizeof( header ) );
}
