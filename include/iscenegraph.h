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

#include <cstddef>
#include "generic/constant.h"
#include "signal/signalfwd.h"

class AABB;

template<typename value_type>
class Stack;
template<typename Contained>
class Reference;

namespace scene
{
class Instance;
const Instance* const nullInstancePointer = 0;
inline const Instance& nullInstance(){
	return *nullInstancePointer;
}

class Node;
const Node* const nullNodePointer = 0;
inline const Node& nullNode(){
	return *nullNodePointer;
}
}

typedef Reference<scene::Node> NodeReference;

typedef std::size_t TypeId;

const TypeId NODETYPEID_MAX = 64;
const TypeId NODETYPEID_NONE = NODETYPEID_MAX;

const TypeId INSTANCETYPEID_MAX = 64;
const TypeId INSTANCETYPEID_NONE = INSTANCETYPEID_MAX;

class Layer;

namespace scene
{
/// \brief A unique key to an instance of a node in the scene-graph.
typedef Stack<NodeReference> Path;

/// \brief A scene-graph - a Directed Acyclic Graph (DAG).
///
/// - Each node may refer to zero or more 'child' nodes (directed).
/// - A node may never have itself as one of its ancestors (acyclic).
/// - Each node may have more than one 'parent', thus having more than one 'instance' in the graph.
/// - Each instance is uniquely identified by the list of its ancestors plus itself, known as a 'path'.
class Graph
{
public:
	INTEGER_CONSTANT( Version, 1 );
	STRING_CONSTANT( Name, "scenegraph" );

	class Walker
	{
	public:
		/// \brief Called before traversing the first child-instance of 'instance'. If the return value is false, the children of the current instance are not traversed.
		virtual bool pre( const Path& path, Instance& instance ) const = 0;
		/// \brief Called after traversing the last child-instance of 'instance'.
		virtual void post( const Path& path, Instance& instance ) const {
		}
	};

	/// \brief Returns the root-node of the graph.
	virtual Node& root() = 0;
	/// \brief Sets the root-node of the graph to be 'node'.
	virtual void insert_root( Node& root ) = 0;
	/// \brief Clears the root-node of the graph.
	virtual void erase_root() = 0;
	/// \brief Returns current layer or 0 when no root is inserted.
	virtual Layer* currentLayer() = 0;
	/// \brief Traverses all nodes in the graph depth-first, starting from the root node.
	virtual void traverse( const Walker& walker ) = 0;
	/// \brief Traverses all nodes in the graph depth-first, starting from 'start'.
	virtual void traverse_subgraph( const Walker& walker, const Path& start ) = 0;
	/// \brief Returns the instance at the location identified by 'path', or 0 if it does not exist.
	virtual scene::Instance* find( const Path& path ) = 0;

	/// \brief Invokes all scene-changed callbacks. Called when any part of the scene changes the way it will appear when the scene is rendered.
	/// \todo Move to a separate class.
	virtual void sceneChanged() = 0;
	/// \brief Add a \p callback to be invoked when the scene changes.
	/// \todo Move to a separate class.
	virtual void addSceneChangedCallback( const SignalHandler& handler ) = 0;

	/// \brief Invokes all bounds-changed callbacks. Called when the bounds of any instance in the scene change.
	/// \todo Move to a separate class.
	virtual void boundsChanged() = 0;
	/// \brief Add a \p callback to be invoked when the bounds of any instance in the scene change.
	virtual SignalHandlerId addBoundsChangedCallback( const SignalHandler& boundsChanged ) = 0;
	/// \brief Remove a \p callback to be invoked when the bounds of any instance in the scene change.
	virtual void removeBoundsChangedCallback( SignalHandlerId id ) = 0;

	virtual TypeId getNodeTypeId( const char* name ) = 0;
	virtual TypeId getInstanceTypeId( const char* name ) = 0;

	/*! \brief Computes the union of the world bounds of \p instance's immediate
	    children into \p bounds.

	    Exists so the graph can cache the answer. Done naively this walks every
	    child, and since a bounds change propagates to the parent, worldspawn -
	    which holds practically the whole map - recomputes from scratch after
	    any edit, and on every frame of a drag.

	    Appended at the end of the interface deliberately: existing vtable slots
	    keep their indices. */
	virtual void childBounds( Instance& instance, AABB& bounds ) = 0;

	/*! \brief \p instance's bounds have changed.

	    The unqualified boundsChanged() cannot say what moved, which leaves a
	    spatial index no choice but to rebuild wholesale. Knowing the instance
	    lets the index repair just the cell holding it.
	 */
	virtual void instanceBoundsChanged( Instance& instance ) = 0;

	/*! \brief \p instance's appearance or placement changed in a way that
	    anything holding pre-combined geometry for it would not reflect.

	    Raised when an ancestor is selected or moved, neither of which touches
	    the instance itself, so nothing else tells it that a cached copy of its
	    geometry has gone stale.
	 */
	virtual void instanceRenderChanged( Instance& instance ) = 0;
};

class Traversable
{
public:
	STRING_CONSTANT( Name, "scene::Traversable" );

	class Observer
	{
	public:
		/// \brief Called when a node is added to the container.
		virtual void insert( Node& node ) = 0;
		/// \brief Called when a node is removed from the container.
		virtual void erase( Node& node ) = 0;
	};

	class Walker
	{
	public:
		/// \brief Called before traversing the first child-node of 'node'. If the return value is false, the children of the current node are not traversed.
		virtual bool pre( Node& node ) const = 0;
		/// \brief Called after traversing the last child-node of 'node'.
		virtual void post( Node& node ) const {
		}
	};
	/// \brief Adds a node to the container.
	virtual void insert( Node& node ) = 0;
	/// \brief Removes a node from the container.
	virtual void erase( Node& node ) = 0;
	/// \brief Traverses the subgraphs rooted at each node in the container, depth-first.
	virtual void traverse( const Walker& walker ) = 0;
	/// \brief Returns true if the container contains no nodes.
	virtual bool empty() const = 0;
};

class Instantiable
{
public:
	STRING_CONSTANT( Name, "scene::Instantiable" );

	class Observer
	{
	public:
		/// \brief Called when an instance is added to the container.
		virtual void insert( scene::Instance* instance ) = 0;
		/// \brief Called when an instance is removed from the container.
		virtual void erase( scene::Instance* instance ) = 0;
	};

	class Visitor
	{
	public:
		virtual void visit( Instance& instance ) const = 0;
	};

	/// \brief Returns a new instance uniquely identified by 'path'.
	virtual scene::Instance* create( const scene::Path& path, scene::Instance* parent ) = 0;
	/// \brief Calls Visitor::visit(instance) for each instance in the container.
	virtual void forEachInstance( const Visitor& visitor ) = 0;
	/// \brief Adds an instance to the container.
	virtual void insert( Observer* observer, const Path& path, scene::Instance* instance ) = 0;
	/// \brief Returns an instance removed from the container.
	virtual scene::Instance* erase( Observer* observer, const Path& path ) = 0;
};

class Cloneable
{
public:
	STRING_CONSTANT( Name, "scene::Cloneable" );

	/// \brief Returns a copy of itself.
	virtual scene::Node& clone() const = 0;
};
} // namespace scene

#include "modulesystem.h"

template<typename Type>
class GlobalModule;
typedef GlobalModule<scene::Graph> GlobalSceneGraphModule;

template<typename Type>
class GlobalModuleRef;
typedef GlobalModuleRef<scene::Graph> GlobalSceneGraphModuleRef;

inline scene::Graph& GlobalSceneGraph(){
	return GlobalSceneGraphModule::getTable();
}

inline void AddSceneChangeCallback( const SignalHandler& handler ){
	GlobalSceneGraph().addSceneChangedCallback( handler );
}
inline void SceneChangeNotify(){
	GlobalSceneGraph().sceneChanged();
}
