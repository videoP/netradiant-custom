/*
   Shared world chunking.

   The spatial index (scenegraph.cpp) and static batching (staticbatch.cpp) both
   bucket world geometry on this grid, and batching answers "is this whole cell
   already drawn?" by chunk key. They must agree exactly, so the key lives here
   rather than being written out twice.
 */

#pragma once

#include "math/vector.h"

#include <cstdint>
#include <cmath>

const double c_chunkSize = 1024.0;
const std::int64_t c_chunkAxisMask = 0x1fffff; // 21 bits per axis

inline std::uint64_t chunk_key( const Vector3& point ){
	const auto coord = []( double v ){
		return static_cast<std::int64_t>( std::floor( v / c_chunkSize ) ) & c_chunkAxisMask;
	};
	return static_cast<std::uint64_t>( coord( point.x() ) )
	     | ( static_cast<std::uint64_t>( coord( point.y() ) ) << 21 )
	     | ( static_cast<std::uint64_t>( coord( point.z() ) ) << 42 );
}

/// \brief The world-space corner of the chunk containing \p point.
inline Vector3 chunk_origin( const Vector3& point ){
	return Vector3( std::floor( point.x() / c_chunkSize ) * c_chunkSize,
	                std::floor( point.y() / c_chunkSize ) * c_chunkSize,
	                std::floor( point.z() / c_chunkSize ) * c_chunkSize );
}
