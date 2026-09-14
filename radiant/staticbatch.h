/*
   Static geometry batching for the camera's solid pass.

   Stock rendering issues one glDrawArrays( GL_POLYGON, 3-4 client-side
   GL_DOUBLE vertices ) per face per frame. Example.map alone is ~159k draw
   calls a frame before anything large is opened.

   This pre-triangulates the faces of static, unselected worldspawn brushes
   into per-chunk vertex buffers grouped by shader, so a chunk costs one draw
   call per shader instead of one per face. Brushes covered by a batch skip
   their own submission for that view.

   Opt-in: Settings / Large Maps.
 */

#pragma once

#include <cstdint>
#include <cstddef>

class Renderer;
class VolumeTest;
class BrushInstance;

/// \brief BrushInstance::m_staticBatchChunk when the brush belongs to no chunk.
/// Chunk key 0 is a real key, so membership needs its own sentinel.
const std::uint64_t c_staticBatchNoChunk = ~std::uint64_t( 0 );

/*! \brief Draws whatever batched geometry intersects \p volume.

    Returns true if batching is active for this view, in which case brushes
    covered by a batch must not submit themselves. Always pair with
    StaticBatch_end().
 */
bool StaticBatch_begin( Renderer& renderer, const VolumeTest& volume );
void StaticBatch_end();

/// \brief True between begin/end of a view whose batches have been drawn.
bool StaticBatch_active();

/*! \brief Whether the camera's current draw mode can be served by batched
    triangles. Two modes cannot: lighting needs a tangent basis a batch does not
    carry, and wireframe would expose the triangulation's diagonals where stock
    rendering draws whole face outlines. Either disables batching for the 3D
    view outright, rather than drawing it wrongly.

    The 2D views are unaffected; they have their own outline batches.
 */
void StaticBatch_setSolidSupported( bool supported );

/*! \brief True if every instance the view traversal would visit in the chunk
    \p key is already covered by a clean batch, so the traversal can skip the
    whole cell instead of walking it to draw nothing.

    \p instanceCount is the cell's instance count as the spatial index sees it.
    A mismatch means the cell holds something batching declined (a patch, a
    hidden or oddly transformed brush), so the cell is walked normally.
 */
bool StaticBatch_cellCovered( std::uint64_t key, std::size_t instanceCount );

/// \brief The brush's geometry, selection or visibility changed. Its chunk is
/// rebuilt before it is next drawn; until then the brush draws itself.
void StaticBatch_brushChanged( const BrushInstance& instance );
/// \brief The brush is about to be destroyed. Drops it from its chunk.
void StaticBatch_brushRemoved( const BrushInstance& instance );

/// \brief Discards everything; the next frame rebuilds from the scene.
void StaticBatch_invalidate();
/// \brief Releases GL buffers. Requires a current context.
void StaticBatch_release();
