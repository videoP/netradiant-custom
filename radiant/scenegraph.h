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

#include "iscenegraph.h"

class VolumeTest;

/*! \brief Depth-first traversal that may skip instances lying outside \p volume.

    Equivalent to scene::Graph::traverse() except that sets of sibling leaf
    instances large enough to be worth indexing are visited through a sparse
    spatial grid, so whole cells outside the view volume are rejected without
    touching the instances inside them.

    Only ever skips instances a volume-culling walker would have discarded
    anyway, so it is interchangeable with traverse() for rendering. Falls back
    to a plain traverse() when the spatial index preference is off, and while
    the scene is being modified.
 */
void Scene_traverseVisible( scene::Graph& graph, const VolumeTest& volume, const scene::Graph::Walker& walker );
