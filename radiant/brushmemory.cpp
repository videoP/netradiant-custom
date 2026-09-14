/*
   See brushmemory.h.
 */

#include "brushmemory.h"

#include "brush.h"
#include "brushnode.h"
#include "brushalloc.h"
#include "memreport.h"

#include "iscenegraph.h"
#include "itextstream.h"
#include "stream/textstream.h"

#include <unordered_set>

namespace
{

/*! \brief Adds up every brush and brush instance in the scene.

    Brushes are deduplicated by pointer: a node can in principle carry more
    than one instance, and counting its faces twice would quietly inflate the
    answer. The set costs a few MB while the walk runs and is dropped before
    anything is printed.
 */
class BrushMemoryWalker : public scene::Graph::Walker
{
	BrushMemory& m_memory;
	std::unordered_set<const Brush*>& m_seen;
public:
	BrushMemoryWalker( BrushMemory& memory, std::unordered_set<const Brush*>& seen )
		: m_memory( memory ), m_seen( seen ){
	}

	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( BrushInstance* brushInstance = InstanceTypeCast<BrushInstance>::cast( instance ) ) {
			brushInstance->accountMemory( m_memory );

			const Brush& brush = brushInstance->getBrush();
			if ( m_seen.insert( &brush ).second ) {
				brush.accountMemory( m_memory );
				/* accountMemory() adds sizeof( Brush ); the object actually
				   allocated is the node that contains it. */
				m_memory.m_nodeExtra += sizeof( BrushNode ) - sizeof( Brush );
				++m_memory.m_allocations;
			}

			/* scene::Path is a Stack, which allocates; and the graph keeps a
			   std::map entry per instance. Neither is inside anything the
			   walker above measures. */
			m_memory.m_paths += path.size() * sizeof( void* );
			m_memory.m_instanceMap += sizeof( void* ) * 5; // key, value, 3 links + colour
			m_memory.m_allocations += 2;
			return false; // brushes are leaves
		}
		return true;
	}
};

void report_line( const char* what, std::size_t bytes, std::size_t brushes ){
	const std::size_t c_mb = 1024 * 1024;
	globalOutputStream() << "    " << what << ' ' << Unsigned( bytes / c_mb ) << " MB";
	if ( brushes != 0 ) {
		globalOutputStream() << ", " << Unsigned( bytes / brushes ) << " B/brush";
	}
	globalOutputStream() << '\n';
}

} // namespace


void BrushMemory_report(){
	BrushMemory memory;
	{
		std::unordered_set<const Brush*> seen;
		GlobalSceneGraph().traverse( BrushMemoryWalker( memory, seen ) );
	}

	if ( memory.m_brushes == 0 ) {
		return;
	}

	const std::size_t c_mb = 1024 * 1024;
	const std::size_t brushes = memory.m_brushes;

	globalOutputStream() << "brush memory: " << Unsigned( memory.m_brushes ) << " brushes, "
	                     << Unsigned( memory.m_faces ) << " faces, "
	                     << Unsigned( memory.m_windingVertices ) << " winding vertices\n";

	if ( memory.m_windingVertices == 0 ) {
		/* Every brush has a winding once its b-rep has been evaluated, and that
		   happens lazily - so this report was taken before anything asked. The
		   figures below are real but incomplete, and the peak is higher. */
		globalOutputStream() << "  (b-rep not evaluated yet, so windings and the arrays "
		                        "derived from them are not in these figures)\n";
	}

	report_line( "Face objects        ", memory.m_faceFixed, brushes );
	report_line( "  of which transform scratch", memory.m_faceTransformScratch, brushes );
	report_line( "winding vertices    ", memory.m_winding, brushes );
	report_line( "  of which tangent/bitangent", memory.m_windingTangents, brushes );
	report_line( "Brush objects       ", memory.m_brushFixed, brushes );
	report_line( "Brush face lists    ", memory.m_facesVector, brushes );
	report_line( "Brush b-rep arrays  ", memory.m_brushArrays, brushes );
	report_line( "BrushInstance       ", memory.m_instanceFixed, brushes );
	report_line( "BrushInstance arrays", memory.m_instanceArrays, brushes );
	report_line( "scene node overhead ", memory.m_nodeExtra, brushes );
	report_line( "scene paths         ", memory.m_paths, brushes );
	report_line( "scene instance map  ", memory.m_instanceMap, brushes );

	/* The pool takes whole blocks, so it holds more than sizeof( Face ) x faces.
	   Reported separately rather than folded in, since it is the one figure
	   here that is exact rather than derived. */
	const std::size_t pool = FaceStorage_reservedBytes();
	if ( pool != 0 ) {
		globalOutputStream() << "    face pool reserved " << Unsigned( pool / c_mb )
		                     << " MB (vs " << Unsigned( memory.m_faceFixed / c_mb )
		                     << " MB of Face objects)\n";
	}

	/* Every separate block carries a header and is rounded up. 16 bytes is the
	   conservative end for the Windows heap; the real figure is larger, so
	   treat this as a floor rather than an estimate. */
	const std::size_t c_allocOverhead = 16;
	const std::size_t overhead = memory.m_allocations * c_allocOverhead;

	globalOutputStream() << "    " << Unsigned( memory.m_allocations ) << " allocations, "
	                     << Unsigned( memory.m_allocations / brushes ) << "/brush, at least "
	                     << Unsigned( overhead / c_mb ) << " MB of headers\n";

	const std::size_t accounted = memory.total() + overhead;
	globalOutputStream() << "  accounted " << Unsigned( accounted / c_mb ) << " MB, "
	                     << Unsigned( memory.total() / brushes ) << " B/brush before headers\n";

	/* Against private bytes, not the working set. Windows trims the working set
	   whenever it feels like it - two runs of the same map measured 13608 MB
	   and 11632 MB resident while committed memory stayed at 14.5 GB both
	   times - so the resident figure says nothing stable about what a map
	   costs. Committed is what runs out. */
	const MemoryUse use = MemoryUse_get();
	if ( use.m_privateBytes > accounted ) {
		/* Patches, entities, the scene graph's own map nodes, the shader and
		   texture caches, Qt, and whatever the allocator is holding but not
		   handing out. Worth watching: if this grows faster than the brush
		   total on a bigger map, the next thing to account for is in here. */
		globalOutputStream() << "  not accounted for: "
		                     << Unsigned( ( use.m_privateBytes - accounted ) / c_mb )
		                     << " MB of committed memory\n";
	}
}
