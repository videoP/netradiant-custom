/*
   Large map performance options.

   Two independent opt-in changes aimed at maps with hundreds of thousands of
   primitives. Both are latched preferences (Settings / Large Maps) so they can
   be A/B tested against stock behaviour; both default off, and with both off
   the code paths are byte-for-byte the stock ones.
 */

#pragma once

#include "preferences.h"

/*! \brief Defer scene tree model (Entity List) population until the Entity List
    window is actually shown.

    Stock behaviour inserts every primitive into a sorted std::vector as it is
    created, which is O(n) per insert and therefore O(n^2) over a map load. The
    model is only ever read by the Entity List window, which is usually closed.
 */
extern LatchedBool g_largemap_deferEntityList;

/*! \brief Cull large sets of sibling primitives through a sparse spatial grid
    during view traversal.

    Stock behaviour walks every instance in the scene, every frame, in every
    view. Worldspawn holds practically the whole map, so its bounds always
    intersect the view volume and nothing is ever pruned above leaf level.
 */
extern LatchedBool g_largemap_spatialIndex;

/*! \brief Batch static worldspawn geometry into per-chunk vertex buffers for the
    camera's solid pass.

    Stock behaviour issues one glDrawArrays over 3-4 client-side vertices per
    face per frame. Batching turns that into one draw call per chunk per shader.
 */
extern LatchedBool g_largemap_staticBatch;

/*! \brief Cache the union of a large container's child bounds instead of
    recomputing it from every child.

    A bounds change propagates to the parent, so worldspawn - which holds
    practically the whole map - otherwise re-walks every brush after any edit,
    and on every frame of a drag. Between structural changes the cached union is
    only grown, which keeps it conservative.
 */
extern LatchedBool g_largemap_incrementalBounds;

void LargeMap_Construct();
