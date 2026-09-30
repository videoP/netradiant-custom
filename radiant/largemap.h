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

/*! \brief Share one copy of each shader name between the faces using it,
    instead of every face holding its own.

    A terrain map has hundreds of thousands of faces naming a handful of
    shaders. See brushalloc.h.
 */
extern LatchedBool g_largemap_shareShaderNames;

/*! \brief Hand Faces out of large blocks rather than allocating each one.

    See brushalloc.h.
 */
extern LatchedBool g_largemap_poolFaces;

/*! \brief Take the short-cuts through map parsing: the tokeniser's fast path
    and the allocation-free shader-cache lookup.

    Neither changes what is produced - the tokeniser was checked token for token
    against the stock one over 215 million tokens, and the shader lookup is the
    same hash table probed without building a string to probe with. This exists
    so that if a map ever reads oddly, it can be ruled in or out in one restart
    rather than by rebuilding.

    Note it does not cover the number parsing in libs/stringio.h, which is
    compiled into the module DLLs as well and so cannot be switched at runtime
    without adding to the module ABI. That one has a compile-time switch at the
    top of the file.
 */
extern LatchedBool g_largemap_fastParse;

/*! \brief Build each brush's vertex/edge editing data only when something asks
    for it, rather than for every brush in the map at load.

    Measured on a 1 GB map: the edge and vertex instances come to ~600 B a brush
    and five of the twenty-five allocations, and nothing reads them until you
    enter vertex or edge mode.
 */
extern LatchedBool g_largemap_lazyComponents;

/*! \brief Pass -cullgrid to q3map2, so that its CullSides stage pairs brushes
    through a spatial grid instead of testing every brush against every other.

    Unlike the rest of this page this one is not a setting of the editor at all:
    radiant and q3map2 are separate programs, so all a tick here can do is put a
    switch on the compiler's command line. It reaches any build command that
    runs q3map2, the same way -fs_pakpath does, and so it is not latched - the
    next compile picks it up.

    It needs a q3map2 that knows the switch; an older one prints "Unknown option"
    and compiles as it always did.
 */
extern bool g_largemap_cullGrid;

/*! \brief Pass -tjgrid to q3map2, so that its FixTJunctions stage finds the edge
    line an edge belongs to through a spatial grid rather than by scanning every
    edge line in the map.

    Same arrangement as g_largemap_cullGrid: a command line switch, not a
    setting of the editor.
 */
extern bool g_largemap_tjGrid;

/*! \brief Most lights the simulated-lights view considers at once.

    Every light that reaches the view is used up to this; past it the ones
    furthest from the camera are dropped. Not latched - it is read each frame.
 */
extern int g_largemap_simLightsMax;

/// \brief Whether q3map_surfacelight faces light the simulated-lights view. Not latched.
extern bool g_largemap_simSurfaceLights;
/// \brief How many of the nearest lights get a shadow cube. Not latched.
extern int g_largemap_simShadowLights;
/// \brief Edge of a shadow cube face in texels. Not latched.
extern int g_largemap_simShadowSize;
/// \brief World units the sun's shadow map covers each side of the camera. Not latched.
extern int g_largemap_simSunShadowRange;

void LargeMap_Construct();

/// \brief Prints which options the running session is actually using.
/// They are latched, so this is not necessarily what Settings shows.
void LargeMap_reportActive();
