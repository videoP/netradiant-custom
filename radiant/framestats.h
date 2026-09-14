/*
   Where a frame's time actually goes, split three ways.

   The stats overlay already said how much was drawn, but not what the time was
   spent on. Measured on a 1 GB map: ~7fps with 10,890 chunks and 10,890 draw
   calls, which works out at ~13us per draw call - far too slow to be the calls
   themselves, and the same signature that last time turned out to be the
   per-brush walk rather than the drawing. Guessing between those two has a
   poor record here, so measure instead.

   Read them from View > Show Stats:

     batch  collecting batched chunks and handing them to the renderer
     walk   traversing the scene graph and submitting everything unbatched
     flush  the GL work - buckets sorted, state set, geometry drawn

   If all three are small but f2f is not, the time is behind the driver: the
   GPU, or the swap. That is an answer too, and a different fix.
 */

#pragma once

#include "timer.h"

#include <cstddef>

extern double g_frametime_batch;
extern double g_frametime_walk;
extern double g_frametime_flush;

/// \brief Chunks looked at, drawn, and draw calls issued, for the last frame.
extern std::size_t g_count_chunks_visited;
extern std::size_t g_count_chunks_drawn;
extern std::size_t g_count_ranges;

/*! \brief Full recomputes of a container's child bounds, and what they cost.

    Measured: a frame where only 3 instances were walked and 153,880 were
    skipped by cell coverage still spent 140ms inside the traversal. The skip
    loop is O(1) per cell, so the time is not there. The other thing the walk
    can do is CompiledGraph::childBounds(), which without a cached answer walks
    every child of the container - and worldspawn's children are the whole map.

    "cb" in the overlay is that: how many full recomputes happened this frame,
    how many instances they visited between them, and how long they took. If
    the count is nonzero on a frame where nothing was edited, something is
    dirtying m_childBoundsChanged every frame and that is the bug.
 */
extern std::size_t g_count_childbounds_full;
extern std::size_t g_count_childbounds_walked;
extern double g_frametime_childbounds;

/*! \brief Draws \p text in black, offset a pixel in each direction, to sit
    underneath the real text as an outline.

    The stats sit directly on the scene, which over a textured 3D view or a busy
    grid leaves them hard to read. Expects an orthographic projection already
    set up in window coordinates. Leaves the current colour black; the caller
    sets its own and draws the text at ( x, y ) afterwards.
 */
void FrameStats_drawStringOutline( float x, float y, const char* text );

/// \brief Adds the lifetime of the scope to one of the three totals above.
class FrameTimerScope
{
	double& m_total;
	Timer m_timer;
public:
	FrameTimerScope( double& total ) : m_total( total ){
	}
	~FrameTimerScope(){
		m_total += m_timer.elapsed_sec();
	}
};
