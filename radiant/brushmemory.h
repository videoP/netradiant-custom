/*
   Where a loaded map's memory actually goes.

   Measured on a 1 GB map: 9855 MB working set for 972,961 brushes, ~10.6 KB
   each. That number is real (memreport.h), but its composition has only ever
   been derived from sizeof() and a guessed allocator overhead - and the last
   two things derived rather than measured in this codebase were both wrong.

   This walks the loaded scene once and adds up what is actually there, so the
   candidates for what to attack next can be ranked by measurement instead of
   by arithmetic. It also counts heap allocations, which
   at ~20 per brush is its own line item on Windows.

   Printed after a load, next to the working-set figure, so the two can be
   compared: whatever the walk does not account for is allocator overhead plus
   everything that is not a brush.
 */

#pragma once

#include <cstddef>

struct BrushMemory
{
	std::size_t m_brushes = 0;
	std::size_t m_instances = 0;
	std::size_t m_faces = 0;
	std::size_t m_windingVertices = 0;
	std::size_t m_allocations = 0;   // separate heap blocks, for the overhead estimate

	// Brush side
	std::size_t m_brushFixed = 0;         // sizeof( BrushNode ), which contains Brush
	std::size_t m_facesVector = 0;        // Brush::m_faces
	std::size_t m_faceFixed = 0;          // sizeof( Face ) x faces
	std::size_t m_faceTransformScratch = 0;   // subset of m_faceFixed: the *Transformed copies
	std::size_t m_winding = 0;            // winding vertices
	std::size_t m_windingTangents = 0;    // subset of m_winding: tangent + bitangent
	std::size_t m_brushArrays = 0;        // the b-rep render/selection arrays on Brush

	// Instance side
	std::size_t m_instanceFixed = 0;      // sizeof( BrushInstance )
	std::size_t m_instanceArrays = 0;     // its face/edge/vertex instances and render arrays

	/* Scene-graph overhead per instance, none of which is inside Brush or
	   BrushInstance and all of which was missing from the first version of this
	   report: the node object that holds the Brush is larger than the Brush,
	   every Instance's scene::Path heap-allocates, and the graph files each one
	   in a std::map node of its own. */
	std::size_t m_nodeExtra = 0;          // sizeof( BrushNode ) beyond sizeof( Brush )
	std::size_t m_paths = 0;              // scene::Path storage, one block per instance
	std::size_t m_instanceMap = 0;        // the graph's std::map nodes

	std::size_t total() const {
		return m_brushFixed + m_facesVector + m_faceFixed + m_winding
		     + m_brushArrays + m_instanceFixed + m_instanceArrays
		     + m_nodeExtra + m_paths + m_instanceMap;
	}
};

/// \brief Walks the scene and reports the breakdown to the console.
void BrushMemory_report();
