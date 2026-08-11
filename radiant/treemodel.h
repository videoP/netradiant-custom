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

class GraphTreeModel* graph_tree_model_new();
void graph_tree_model_delete( GraphTreeModel* model );

namespace scene
{
class Instance;
}
void graph_tree_model_insert( GraphTreeModel* model, const scene::Instance& instance );
void graph_tree_model_erase( GraphTreeModel* model, const scene::Instance& instance );

/// \brief Empties the model and makes insert/erase/rename no-ops until graph_tree_model_populate().
void graph_tree_model_suspend( GraphTreeModel* model );
/// \brief Rebuilds the whole model from the scene graph in one O(n log n) pass, and resumes incremental updates.
void graph_tree_model_populate( GraphTreeModel* model );
bool graph_tree_model_suspended( GraphTreeModel* model );

/* Entity List visibility hooks. No-ops unless the deferred-population
   preference is on, so stock behaviour is unaffected. */
void graph_tree_model_entitylist_shown();
void graph_tree_model_entitylist_hidden();


#include <qnamespace.h>
constexpr int c_ItemDataRole_Instance = Qt::ItemDataRole::UserRole + 1;
constexpr int c_ItemDataRole_Node = Qt::ItemDataRole::UserRole + 2;


