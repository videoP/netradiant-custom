/*
   Allocation strategies for the two things a map load makes millions of.

   A 280k brush map builds ~2M Faces and ~2M copies of shader names, each its
   own heap allocation. Both are opt-in from Settings / Large Maps; with the
   options off these behave exactly like new/delete and a private string copy.
 */

#pragma once

#include <cstddef>

/// rief Mirror of g_largemap_lazyComponents; see the comment in brush.h.
extern bool g_brush_lazyComponents;

/// \brief Bytes the face pool has taken from the system, block rounding and all.
/// Exact, unlike sizeof( Face ) x faces, which misses the tail of each block.
std::size_t FaceStorage_reservedBytes();

/*! \brief Takes ownership of a copy of \p name for a face to hold.

    Shader names repeat enormously - a terrain map has hundreds of thousands of
    faces sharing a handful of names - so the copies can be shared. Returned
    pointers stay valid for the life of the process.
 */
const char* ShaderName_store( const char* name );
/// \brief Releases a name from ShaderName_store(). A no-op while sharing.
void ShaderName_release( const char* name );

/*! \brief Face storage, handed out from large blocks rather than one at a time.

    Faces are all the same size and are created and destroyed in great numbers,
    which is the case a general-purpose allocator serves worst: every one of
    them otherwise carries its own bookkeeping header.
 */
void* FaceStorage_allocate( std::size_t size );
void FaceStorage_release( void* face );
