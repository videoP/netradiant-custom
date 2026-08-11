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

void LargeMap_Construct();
