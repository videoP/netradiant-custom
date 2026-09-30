/*
   Shadow maps for the simulated map lights. See simlights.h.

   Each of the nearest few lights gets a cube shadow map, and the sun gets one
   orthographic map that follows the camera. A map is the scene drawn depth-only
   from the light, through the same scene walk and render states the camera
   uses, so batched geometry, patches, models and alpha-tested textures all
   cast without a second code path. Only opaque textured fill states are drawn
   (see RENDER_SHADOWPASS): sky, translucent surfaces, clips and entity boxes do
   not cast.

   The fragment shader (gl/simlights_fp.glsl) compares a lit point's distance
   from the light with what the map holds in that direction.

   Maps are redrawn when the scene changes, at most one per frame once they
   have been drawn at all; a map that has never been drawn is drawn at once,
   because the alternative is a light shining through walls for a few frames.
 */

#pragma once

#include "simlights.h"

/*! \brief Chooses which lights get a cube, draws the maps that are out of date,
    and fills in the shadow fields of \p frame (and each light's shadowSlot).

    Must run before the camera has walked the scene: drawing a map uses the
    shader cache's render-state buckets, which the camera's walk fills.

    \param viewDir where the camera looks, so the sun's map can be centred ahead of it.
 */
void SimShadows_frame( SimLightsFrame& frame, const Vector3& viewer, const Vector3& viewDir );

/// \brief Maps were left out of date by this frame's budget; the view should be drawn again.
bool SimShadows_pending();

/// \brief Drops every GL object. Needs the context that made them, or none at all if it is gone.
void SimShadows_release();
