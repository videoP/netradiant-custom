/*
   Copyright (C) 2001-2006, William Joseph.
   All Rights Reserved.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

#include "math/matrix.h"
#include "math/vector.h"

void ShaderCache_setBumpEnabled( bool enabled );
void ShaderCache_extensionsInitialised();

/*! \brief Picks this frame's simulated lights, and draws the shadow maps they need.

    Call first thing in the camera's draw, before anything is walked into the
    render states: the shadow maps are drawn through those same states.
    False if the simulated lights cannot run on this driver, in which case the
    camera should draw as usual and leave RENDER_SIMLIGHTS off.
 */
bool SimLights_prepare( const Matrix4& modelview, const Matrix4& projection, const Vector3& viewer, const Vector3& viewDir, int width, int height );

/// \brief One line for View / Show Stats: how many lights this frame used, of how many reached the view.
const char* SimLights_getStats();
