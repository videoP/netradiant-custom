/*
   See brushalloc.h.
 */

#include "brushalloc.h"

#include "largemap.h"
#include "string/string.h"
#include "debugging/debugging.h"

#include <unordered_set>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

namespace
{

/* Both options are latched, so their value cannot change within a session.
   That is what lets the release paths below decide which scheme was used
   without recording it per allocation. */

/* Transparent so the table can be probed with the caller's const char* as-is.
   emplace() would otherwise build a std::string to look with and throw it away
   again on every hit, which is a malloc/free pair per face - and a hit is what
   nearly every lookup is, since a map names a handful of shaders across
   millions of faces. */
struct ShaderNameHash
{
	using is_transparent = void;
	std::size_t operator()( const std::string_view name ) const {
		return std::hash<std::string_view>()( name );
	}
};

struct ShaderNameEqual
{
	using is_transparent = void;
	bool operator()( const std::string_view one, const std::string_view other ) const {
		return one == other;
	}
};

typedef std::unordered_set<std::string, ShaderNameHash, ShaderNameEqual> ShaderNames;

ShaderNames& shaderNames(){
	static ShaderNames names; // node-based, so c_str() stays put
	return names;
}


const std::size_t c_facesPerBlock = 2048;

std::vector<void*> g_blocks;
void* g_freeList = nullptr;   // singly linked through the first word of each free slot
void* g_block = nullptr;      // block currently being handed out
std::size_t g_slotSize = 0;
std::size_t g_slotsUsed = 0;

} // namespace


const char* ShaderName_store( const char* name ){
	if ( !g_largemap_shareShaderNames.m_value ) {
		return string_clone( name );
	}
	ShaderNames& names = shaderNames();
	const auto i = names.find( std::string_view( name ) );
	if ( i != names.end() ) {
		return i->c_str();
	}
	return names.emplace( name ).first->c_str();
}

void ShaderName_release( const char* name ){
	if ( !g_largemap_shareShaderNames.m_value && name != nullptr ) {
		string_release( const_cast<char*>( name ), string_length( name ) );
	}
}


void* FaceStorage_allocate( std::size_t size ){
	if ( !g_largemap_poolFaces.m_value ) {
		return ::operator new( size );
	}

	if ( g_slotSize == 0 ) {
		g_slotSize = std::max( size, sizeof( void* ) ); // room for the free-list link
	}
	ASSERT_MESSAGE( size <= g_slotSize, "FaceStorage_allocate: unexpected size" );

	if ( g_freeList != nullptr ) {
		void* slot = g_freeList;
		g_freeList = *static_cast<void**>( slot );
		return slot;
	}

	if ( g_block == nullptr || g_slotsUsed == c_facesPerBlock ) {
		g_block = ::operator new( g_slotSize * c_facesPerBlock );
		g_blocks.push_back( g_block );
		g_slotsUsed = 0;
	}

	return static_cast<char*>( g_block ) + g_slotSize * g_slotsUsed++;
}

void FaceStorage_release( void* face ){
	if ( face == nullptr ) {
		return;
	}
	if ( !g_largemap_poolFaces.m_value ) {
		::operator delete( face );
		return;
	}
	/* Returned to the free list rather than to the system. Blocks are never
	   handed back, which suits an editor: the peak is reached once, on load. */
	*static_cast<void**>( face ) = g_freeList;
	g_freeList = face;
}
