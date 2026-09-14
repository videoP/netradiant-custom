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

#include "scenegraph.h"

#include "debugging/debugging.h"

#include <map>

#include "string/string.h"
#include "signal/signal.h"
#include "scenelib.h"
#include "instancelib.h"
#include "treemodel.h"
#include "layers.h"
#include "entitylist.h"
#include "largemap.h"
#include "staticbatch.h"

#include "cullable.h"
#include "chunkgrid.h"
#include "math/frustum.h"
#include "math/aabb.h"

/* Per-frame diagnostics, shown by View / Show Stats. Defined in renderstate.cpp
   alongside the other counters, and reset with them. */
extern std::size_t g_count_instances;      // instances handed to the view walker
extern std::size_t g_count_cells_batched;  // instances skipped because their cell is batched
extern bool g_index_used;                  // false when the traversal fell back to the full walk

#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cmath>

namespace
{
// declared here so CompiledGraph's inline members can see it; defined once below
GraphTreeModel* g_tree_model;
}


/*! \brief A sparse uniform grid over a set of sibling leaf instances.

    Each instance is filed in the single cell containing its AABB centre, and
    that cell's bounds are grown to contain the instance outright. Culling then
    tests the grown cell bounds, which is conservative, so no instance is ever
    wrongly rejected and nothing needs to be stored twice.
 */
class InstanceGrid
{
	struct Cell
	{
		AABB bounds;
		std::vector<scene::Instance*> instances;
	};

	std::unordered_map<std::uint64_t, Cell> m_cells;

public:
	void insert( scene::Instance* instance ){
		const AABB& aabb = instance->worldAABB();
		Cell& cell = m_cells[ chunk_key( aabb.origin ) ];
		aabb_extend_by_aabb_safe( cell.bounds, aabb ); // handles a still-invalid cell AABB
		cell.instances.push_back( instance );
	}

	std::size_t cellCount() const {
		return m_cells.size();
	}

	void traverse( const scene::Graph::Walker& walker, const VolumeTest& volume ) const {
		for ( const auto& [ key, cell ] : m_cells )
		{
			if ( volume.TestAABB( cell.bounds ) == c_volumeOutside ) {
				continue;
			}
			/* Everything here is already drawn as one batch, so walking it
			   would spend a per-instance cull, state push/pop and virtual call
			   each, only for the instance to decline to draw. */
			if ( StaticBatch_cellCovered( key, cell.instances.size() ) ) {
				g_count_cells_batched += cell.instances.size();
				continue;
			}
			for ( scene::Instance* instance : cell.instances )
			{
				// gridded instances are leaves, so pre()'s return value has nothing to prune
				++g_count_instances;
				walker.pre( instance->path(), *instance );
				walker.post( instance->path(), *instance );
			}
		}
	}
};

/// \brief Minimum sibling leaf count before a grid is worth building for a parent.
const std::size_t c_minChildrenForGrid = 1024;

template<std::size_t SIZE>
class TypeIdMap
{
	typedef const char* TypeName;
	typedef TypeName TypeNames[SIZE];
	TypeNames m_typeNames;
	TypeName* m_typeNamesEnd;

public:
	TypeIdMap() : m_typeNamesEnd( m_typeNames ){
	}
	TypeId getTypeId( const char* name ){
		TypeName* i = std::find_if( m_typeNames, m_typeNamesEnd, [name]( const char* other ){ return string_equal( name, other ); } );
		if ( i == m_typeNamesEnd ) {
			ASSERT_MESSAGE( m_typeNamesEnd != m_typeNames + SIZE, "reached maximum number of type names supported (" << SIZE << ')' );
			*m_typeNamesEnd++ = name;
		}
		return i - m_typeNames;
	}
};

class CompiledGraph final : public scene::Graph, public scene::Instantiable::Observer
{
	typedef std::map<PathConstReference, scene::Instance*> InstanceMap;

	InstanceMap m_instances;
	scene::Instantiable::Observer* m_observer;
	Signal0 m_boundsChanged;
	scene::Path m_rootpath;
	Signal0 m_sceneChangedCallbacks;
	Layer *m_currentLayer0 = nullptr;
	Layer **m_currentLayer = &m_currentLayer0;

	TypeIdMap<NODETYPEID_MAX> m_nodeTypeIds;
	TypeIdMap<INSTANCETYPEID_MAX> m_instanceTypeIds;

	std::unordered_map<scene::Instance*, InstanceGrid> m_grids;
	bool m_indexDirty = true;          // instances added/removed: grids unusable until rebuilt
	bool m_indexBoundsDirty = false;   // something moved: fall back this frame, rebuild after
	bool m_indexRebuildPending = false;

public:

	CompiledGraph( scene::Instantiable::Observer* observer )
		: m_observer( observer ){
	}

	void addSceneChangedCallback( const SignalHandler& handler ) override {
		m_sceneChangedCallbacks.connectLast( handler );
	}
	void sceneChanged() override {
		m_sceneChangedCallbacks();
	}

	scene::Node& root() override {
		ASSERT_MESSAGE( !m_rootpath.empty(), "scenegraph root does not exist" );
		return m_rootpath.top();
	}
	void insert_root( scene::Node& root ) override {
		//globalOutputStream() << "insert_root\n";

		ASSERT_MESSAGE( m_rootpath.empty(), "scenegraph root already exists" );

		root.IncRef();

		/* Streaming a whole map through the tree model one insert at a time is
		   O(n^2); suspend it and rebuild in one pass afterwards, and only if
		   anything is actually looking at it. */
		const bool defer = g_largemap_deferEntityList.m_value;
		if ( defer ) {
			graph_tree_model_suspend( g_tree_model );
		}

		StaticBatch_invalidate();

		Node_traverseSubgraph( root, InstanceSubgraphWalker( this, scene::Path(), 0 ) );

		m_rootpath.push( makeReference( root ) );

		m_currentLayer = &Node_getLayers( root )->m_currentLayer;

		if ( defer && EntityList_visible() ) {
			graph_tree_model_populate( g_tree_model );
		}
	}
	void erase_root() override {
		//globalOutputStream() << "erase_root\n";

		ASSERT_MESSAGE( !m_rootpath.empty(), "scenegraph root does not exist" );

		scene::Node& root = m_rootpath.top();

		m_rootpath.pop();

		StaticBatch_invalidate(); // drops every brush pointer before the instances go

		/* likewise: erasing one at a time is O(n^2) */
		if ( g_largemap_deferEntityList.m_value ) {
			graph_tree_model_suspend( g_tree_model );
		}

		Node_traverseSubgraph( root, UninstanceSubgraphWalker( this, scene::Path() ) );

		root.DecRef();

		m_currentLayer = &m_currentLayer0;
	}
	Layer* currentLayer() override {
		return *m_currentLayer;
	}
	void boundsChanged() override {
		m_indexBoundsDirty = true;
		m_boundsChanged();
	}

	void traverse( const Walker& walker ) override {
		traverse_subgraph( walker, m_instances.begin() );
	}

	/* --- spatial index (see largemap.h) --- */

	void traverse_visible( const Walker& walker, const VolumeTest& volume ){
		g_index_used = false;

		if ( !g_largemap_spatialIndex.m_value || m_instances.empty() ) {
			traverse( walker );
			return;
		}

		if ( m_indexDirty ) {
			/* Instances were added or removed, so the grids may hold dangling
			   pointers - they cannot be used at all until rebuilt. */
			rebuildIndex();
		}
		else if ( m_indexBoundsDirty ) {
			/* Something moved. Rather than rebuild mid-drag, fall back to the
			   stock traversal for this frame and rebuild once it settles. */
			m_indexBoundsDirty = false;
			m_indexRebuildPending = true;
			traverse( walker );
			return;
		}
		else if ( m_indexRebuildPending ) {
			rebuildIndex();
		}

		g_index_used = true;

		InstanceMap::iterator i = m_instances.begin();
		while ( i != m_instances.end() )
		{
			traverse_visible_recursive( walker, i, volume );
		}
	}

	void traverse_subgraph( const Walker& walker, const scene::Path& start ) override {
		if ( !m_instances.empty() ) {
			traverse_subgraph( walker, m_instances.find( PathConstReference( start ) ) );
		}
	}

	scene::Instance* find( const scene::Path& path ) override {
		InstanceMap::iterator i = m_instances.find( PathConstReference( path ) );
		if ( i == m_instances.end() ) {
			return 0;
		}
		return ( *i ).second;
	}

	void insert( scene::Instance* instance ) override {
		m_instances.insert( InstanceMap::value_type( PathConstReference( instance->path() ), instance ) );
		m_indexDirty = true;

		m_observer->insert( instance );
	}
	void erase( scene::Instance* instance ) override {
		m_observer->erase( instance );

		m_instances.erase( PathConstReference( instance->path() ) );
		m_indexDirty = true; // grids now hold a dangling pointer; must rebuild before next use
	}

	SignalHandlerId addBoundsChangedCallback( const SignalHandler& boundsChanged ) override {
		return m_boundsChanged.connectLast( boundsChanged );
	}
	void removeBoundsChangedCallback( SignalHandlerId id ) override {
		m_boundsChanged.disconnect( id );
	}

	TypeId getNodeTypeId( const char* name ) override {
		return m_nodeTypeIds.getTypeId( name );
	}

	TypeId getInstanceTypeId( const char* name ) override {
		return m_instanceTypeIds.getTypeId( name );
	}

private:

	bool pre( const Walker& walker, const InstanceMap::iterator& i ){
		++g_count_instances;
		return walker.pre( i->first, *i->second );
	}

	void post( const Walker& walker, const InstanceMap::iterator& i ){
		walker.post( i->first, *i->second );
	}

	/*! \brief Visits the subtree rooted at \p i, advancing \p i past it.

	    Children of an instance are the following entries whose path is exactly
	    one longer; the map being path-sorted keeps a whole subtree contiguous.
	    Where a parent has a grid, its children are skipped in the map and
	    visited through the grid instead.
	 */
	void traverse_visible_recursive( const Walker& walker, InstanceMap::iterator& i, const VolumeTest& volume ){
		const InstanceMap::iterator self = i;
		const std::size_t depth = self->first.get().size();
		++i;

		++g_count_instances;
		if ( walker.pre( self->first, *self->second ) ) {
			const auto grid = m_grids.find( self->second );
			if ( grid != m_grids.end() ) {
				while ( i != m_instances.end() && i->first.get().size() > depth )
					++i; // gridded children are leaves; skip the run in the map
				grid->second.traverse( walker, volume );
			}
			else{
				while ( i != m_instances.end() && i->first.get().size() > depth )
					traverse_visible_recursive( walker, i, volume );
			}
		}
		else{
			while ( i != m_instances.end() && i->first.get().size() > depth )
				++i; // subtree skipped
		}

		walker.post( self->first, *self->second );
	}

	/// \brief Collects the leaf children of \p i's subtree, building a grid for any
	/// parent with enough of them to be worth it. Advances \p i past the subtree.
	void buildGrids( InstanceMap::iterator& i ){
		const InstanceMap::iterator self = i;
		const std::size_t depth = self->first.get().size();
		++i;

		std::vector<scene::Instance*> leafChildren;
		bool allLeaves = true;

		while ( i != m_instances.end() && i->first.get().size() > depth )
		{
			const InstanceMap::iterator child = i;
			buildGrids( i );
			if ( std::next( child ) == i ) { // consumed exactly one entry, so it is a leaf
				leafChildren.push_back( child->second );
			}
			else{
				allLeaves = false;
			}
		}

		if ( allLeaves && leafChildren.size() >= c_minChildrenForGrid ) {
			InstanceGrid& grid = m_grids[ self->second ];
			for ( scene::Instance* instance : leafChildren )
			{
				grid.insert( instance );
			}
		}
	}

	void rebuildIndex(){
		m_grids.clear();

		InstanceMap::iterator i = m_instances.begin();
		while ( i != m_instances.end() )
		{
			buildGrids( i );
		}

		/* Evaluating instance bounds during the build dirties the bounds flag;
		   clear it last so the index is not immediately considered stale. */
		m_indexDirty = false;
		m_indexBoundsDirty = false;
		m_indexRebuildPending = false;

		std::size_t cells = 0;
		for ( const auto& [ parent, grid ] : m_grids )
			cells += grid.cellCount();
		globalOutputStream() << "spatial index: " << Unsigned( m_grids.size() ) << " indexed parents, "
		                     << Unsigned( cells ) << " cells\n";
	}

	void traverse_subgraph( const Walker& walker, InstanceMap::iterator i ){
		Stack<InstanceMap::iterator> stack;
		if ( i != m_instances.end() ) {
			const std::size_t startSize = ( *i ).first.get().size();
			do
			{
				if ( i != m_instances.end()
				     && stack.size() < ( ( *i ).first.get().size() - startSize + 1 ) ) {
					stack.push( i );
					++i;
					if ( !pre( walker, stack.top() ) ) {
						// skip subgraph
						while ( i != m_instances.end()
						        && stack.size() < ( ( *i ).first.get().size() - startSize + 1 ) )
						{
							++i;
						}
					}
				}
				else
				{
					post( walker, stack.top() );
					stack.pop();
				}
			}
			while ( !stack.empty() );
		}
	}
};

namespace
{
CompiledGraph* g_sceneGraph;
}

GraphTreeModel* scene_graph_get_tree_model(){
	return g_tree_model;
}

void Scene_traverseVisible( scene::Graph& graph, const VolumeTest& volume, const scene::Graph::Walker& walker ){
	if ( &graph == g_sceneGraph ) {
		g_sceneGraph->traverse_visible( walker, volume );
	}
	else{
		graph.traverse( walker );
	}
}


class SceneGraphObserver : public scene::Instantiable::Observer
{
public:
	void insert( scene::Instance* instance ) override {
		g_sceneGraph->sceneChanged();
		graph_tree_model_insert( g_tree_model, *instance );
	}
	void erase( scene::Instance* instance ) override {
		g_sceneGraph->sceneChanged();
		graph_tree_model_erase( g_tree_model, *instance );
	}
};

SceneGraphObserver g_SceneGraphObserver;

void SceneGraph_Construct(){
	g_tree_model = graph_tree_model_new();

	g_sceneGraph = new CompiledGraph( &g_SceneGraphObserver );
}

void SceneGraph_Destroy(){
	delete g_sceneGraph;

	graph_tree_model_delete( g_tree_model );
}


#include "modulesystem/singletonmodule.h"
#include "modulesystem/moduleregistry.h"

class SceneGraphAPI
{
	scene::Graph* m_scenegraph;
public:
	typedef scene::Graph Type;
	STRING_CONSTANT( Name, "*" );

	SceneGraphAPI(){
		SceneGraph_Construct();

		m_scenegraph = g_sceneGraph;
	}
	~SceneGraphAPI(){
		SceneGraph_Destroy();
	}
	scene::Graph* getTable(){
		return m_scenegraph;
	}
};

typedef SingletonModule<SceneGraphAPI> SceneGraphModule;
typedef Static<SceneGraphModule> StaticSceneGraphModule;
StaticRegisterModule staticRegisterSceneGraph( StaticSceneGraphModule::instance() );
