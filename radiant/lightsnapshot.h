/*
   Light snapshot: the surfaces around the camera, lit by the real q3map2.

   The simulated lights are fast but only q3map2's direct light, approximately.
   A snapshot instead hands q3map2 a small part of the map (a box around the
   camera, plus the sky) and runs its own -meta and -light stages on it, bounce
   included. q3map2 writes what it has after the direct pass and after every
   bounce (-lightsnap, see tools/quake3/q3map2/light_snapshot.cpp), and each of
   those is drawn over the camera view as it arrives, so the picture brightens
   pass by pass and ends as the compile would have it.

   What it is not: the whole map. Only the box is lit, and light from outside
   it, or bounced off geometry outside it, is missing near its edges. The
   lightmaps do not depend on the camera, so the result stays put as the camera
   moves; it describes the map as it was when the snapshot was taken.
 */

#pragma once

#include "preferences.h"
#include "math/matrix.h"
#include "math/vector.h"

#include <functional>

class QWidget;

/// \brief Takes a snapshot around \p origin. \p redraw is called when a new pass is ready to draw.
/// While it builds, a progress panel with a Cancel button is shown over \p parent; Cancel calls \p cancel.
void LightSnap_start( const Vector3& origin, std::function<void()> redraw, QWidget* parent, std::function<void()> cancel );

/// \brief Asks for a compiled .bsp and shows the lighting it holds (its lightmaps, or vertex colours), in place of a snapshot.
/// Same panel and callbacks as LightSnap_start. False if nothing was chosen or it could not start.
bool LightSnap_startBSP( std::function<void()> redraw, QWidget* parent, std::function<void()> cancel );

/// \brief Hides the snapshot and stops a running compile.
void LightSnap_stop();

/// \brief Whether there is anything to draw: a pass has arrived.
bool LightSnap_active();

/// \brief Draws the snapshot into the camera view, over the scene already drawn. Needs the camera's matrices loaded.
void LightSnap_draw( const Matrix4& modelview, const Matrix4& projection );

/// \brief One line for the view: what the snapshot is doing, or empty.
const char* LightSnap_status();

/// \brief Lets go of the shaders it holds: the shader system is about to free them. They are fetched again when drawing resumes.
void LightSnap_unrealise();

/// \brief Drops every GL object. Needs the context that made them.
void LightSnap_release();

void LightSnap_constructPreferences( PreferencesPage& page );
void LightSnap_registerPreferences();
