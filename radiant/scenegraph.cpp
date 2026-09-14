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
#include "iselection.h"
#include "scopetimer.h"

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
#include <algorithm>
#include <iterator>
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
	std::unordered_map<scene::Instance*, std::uint64_t> m_membership;

public:
	void insert( scene::Instance* instance ){
		const AABB& aabb = instance->worldAABB();
		const std::uint64_t key = chunk_key( aabb.origin );
		Cell& cell = m_cells[ key ];
		aabb_extend_by_aabb_safe( cell.bounds, aabb ); // handles a still-invalid cell AABB
		cell.instances.push_back( instance );
		m_membership.emplace( instance, key );
	}

	/*! \brief Repairs the cell holding \p instance after it moved.

	    The instance stays in the cell it was filed in - moving it between cells
	    would mean an erase from a vector - and the cell simply grows to keep
	    containing it. Cells are already conservative by construction, so a
	    slightly larger one costs a little culling accuracy and nothing else.
	    Returns false if the instance is not in this grid.
	 */
	bool moved( scene::Instance* instance ){
		const auto i = m_membership.find( instance );
		if ( i == m_membership.end() ) {
			return false;
		}
		aabb_extend_by_aabb_safe( m_cells[ i->second ].bounds, instance->worldAABB() );
		return true;
	}

	/// \brief Repairs every cell after the container owning this grid moved.
	void contentsMoved(){
		for ( const auto& [ instance, key ] : m_membership )
		{
			aabb_extend_by_aabb_safe( m_cells[ key ].bounds, instance->worldAABB() );
		}
	}

	/*! \brief Removes \p instance from its cell. False if it is not in this grid.

	    The cell's bounds are left as they are. They are conservative by
	    construction, so one that is larger than its contents costs a little
	    culling accuracy and nothing else - the same trade moved() already makes.
	 */
	bool erase( scene::Instance* instance ){
		const auto i = m_membership.find( instance );
		if ( i == m_membership.end() ) {
			return false;
		}
		std::vector<scene::Instance*>& instances = m_cells[ i->second ].instances;
		instances.erase( std::remove( instances.begin(), instances.end(), instance ), instances.end() );
		m_membership.erase( i );
		return true;
	}

	bool contains( scene::Instance* instance ) const {
		return m_membership.find( instance ) != m_membership.end();
	}

	std::size_t cellCount() const {
		return m_cells.size();
	}

	void traverse( const scene::Graph::Walker& walker, const VolumeTest& volume,
	               const scene::Instance& owner ) const {
		for ( const auto& [ key, cell ] : m_cells )
		{
			if ( volume.TestAABB( cell.bounds ) == c_volumeOutside ) {
				continue;
			}
			/* Everything here is already drawn as one batch, so walking it
			   would spend a per-instance cull, state push/pop and virtual call
			   each, only for the instance to decline to draw. */
			if ( StaticBatch_cellCovered( key, owner, cell.instances.size() ) ) {
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

/// \brief Minimum child count before caching an instance's child bounds pays for
/// itself. Below this the walk is trivial, and caching every leaf would put an
/// entry in the table for every brush in the map.
const std::size_t c_minChildrenForBoundsCache = 256;

/// \brief Accumulates the world bounds of an instance's immediate children.
/// Counts them too, to decide whether the answer is worth caching.
class AABBAccumulateWalker : public scene::Graph::Walker
{
	AABB& m_aabb;
	std::size_t& m_count;
	mutable std::size_t m_depth;
public:
	AABBAccumulateWalker( AABB& aabb, std::size_t& count )
		: m_aabb( aabb ), m_count( count ), m_depth( 0 ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( m_depth == 1 ) {
			aabb_extend_by_aabb_safe( m_aabb, instance.worldAABB() );
			++m_count;
		}
		return ++m_depth != 2;
	}
	void post( const scene::Path& path, scene::Instance& instance ) const override {
		--m_depth;
	}
};

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

	/*! \brief A grid, plus where its parent's subtree ends in the instance map.

	    The end iterator is remembered because stepping past those children one
	    at a time is not free: the map is a path-sorted std::map, so ++ is a
	    red-black tree pointer chase, and worldspawn's children are the whole
	    map. Measured on a 1 GB map, that skip alone cost 140ms a frame - the
	    entire frame - even with cell coverage meaning only three instances were
	    actually visited.

	    Held together with the grid rather than in a table beside it so the two
	    cannot fall out of step. It is exactly as safe as the scene::Instance*
	    pointers the grid already stores: both are only valid while no instance
	    has been added or removed, and both are dropped by the same rebuild -
	    insert() and erase() set m_indexDirty, and std::map invalidates nothing
	    else.
	 */
	struct GriddedParent
	{
		InstanceGrid m_grid;
		InstanceMap::iterator m_subtreeEnd; // first map entry past the gridded children
	};

	std::unordered_map<scene::Instance*, GriddedParent> m_grids;
	bool m_indexDirty = true;          // instances added/removed: grids unusable until rebuilt
	std::vector<scene::Instance*> m_moved; // moved since the last traversal; cells to grow

	/// \brief Cached union of an instance's immediate children's world bounds.
	/// Presence in the table means valid; a structural change drops the table.
	struct ChildBounds
	{
		AABB m_bounds;
		std::size_t m_generation = 0; // m_boundsGeneration when last brought up to date
	};
	std::unordered_map<scene::Instance*, ChildBounds> m_childBounds;
	std::size_t m_boundsGeneration = 1;

	/*! \brief Drops the index whole.

	    Needed either side of a map load. The grids key on scene::Instance*, and
	    leaving emptied ones behind would let indexInsert() find no grid for the
	    new map's worldspawn, decide there was nothing to do, and never set the
	    dirty flag - so the new map would silently never get an index at all.
	 */
	void indexInvalidate(){
		m_grids.clear();
		m_moved.clear();
		m_indexDirty = true;
	}

	/// \brief Something moved. Cached unions may grow but need no full rebuild.
	void childBoundsMoved(){
		++m_boundsGeneration;
	}
	/// \brief Instances were added or removed, so a cached union may now be
	/// wrong in the direction that matters. Force full recomputes.
	void childBoundsInvalidate(){
		m_childBounds.clear();
	}

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
		indexInvalidate();

		{
			// creating a scene instance for every node, and filing it in the map
			ScopeTimer timer( "  instancing" );
			Node_traverseSubgraph( root, InstanceSubgraphWalker( this, scene::Path(), 0 ) );
		}

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
		indexInvalidate();        // likewise, and it makes every indexErase below a no-op

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
		childBoundsMoved();
		m_boundsChanged();
	}

	void instanceRenderChanged( scene::Instance& instance ) override {
		StaticBatch_instanceChanged( instance );
	}

	void instanceBoundsChanged( scene::Instance& instance ) override {
		/* Deferred, not applied here: the instance's bounds have just been
		   invalidated, so its new AABB cannot be read until something asks for
		   it. Repaired on the next traversal instead. */
		if ( !m_grids.empty() ) {
			m_moved.push_back( &instance );
		}
		boundsChanged();
	}

	/// \brief Grows the cells holding anything that moved, so the index stays
	/// usable without a rebuild. O(moved), against O(map) for a rebuild.
	void repairIndex(){
		if ( m_moved.empty() ) {
			return;
		}
		for ( scene::Instance* instance : m_moved )
		{
			/* Moving an entity invalidates every child's world bounds, but the
			   scene's bounds notification names the entity itself. It owns the
			   grid rather than belonging to it, so repair all of its cells. */
			const auto owned = m_grids.find( instance );
			if ( owned != m_grids.end() ) {
				owned->second.m_grid.contentsMoved();
				continue;
			}
			for ( auto& [ parent, gridded ] : m_grids )
			{
				if ( gridded.m_grid.moved( instance ) ) {
					break;
				}
			}
		}
		m_moved.clear();
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
		else{
			/* Movement only ever needs the affected cells grown. Rebuilding on
			   movement meant a drag rebuilt the whole index every frame - once
			   per view, in fact, since several views traverse per frame. */
			repairIndex();
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

	/*! \brief Files a newly added instance into its parent's grid.

	    Marking the whole index dirty instead costs a full rebuild - measured at
	    0.6s on a 970k brush map - and that is paid on the next traversal, which
	    is why creating one brush stalled.

	    Two cases still force a rebuild. An instance appearing *under* something
	    already filed as a leaf means that thing is not a leaf after all, and the
	    grid would silently stop traversing its children. And a parent with no
	    grid is left alone: it may have just crossed the threshold where one
	    becomes worthwhile, but nothing here can tell, and missing that only
	    costs an optimisation until the next rebuild.
	 */
	void indexInsert( scene::Instance* instance ){
		if ( m_indexDirty || m_grids.empty() ) {
			return; // a rebuild is already coming
		}
		scene::Instance* parent = instance->parent();
		if ( parent == 0 ) {
			m_indexDirty = true;
			return;
		}
		const auto grid = m_grids.find( parent );
		if ( grid != m_grids.end() ) {
			grid->second.m_grid.insert( instance );
			return;
		}
		for ( const auto& [ owner, gridded ] : m_grids )
		{
			if ( gridded.m_grid.contains( parent ) ) {
				m_indexDirty = true; // that parent has stopped being a leaf
				return;
			}
		}
	}

	/// \brief Takes an instance out of the index. Must run while it is still in
	/// the instance map, since a remembered subtree end may point at its entry.
	void indexErase( scene::Instance* instance ){
		/* Whether or not the grids are usable, this pointer is about to become
		   invalid and repairIndex() would dereference it. */
		m_moved.erase( std::remove( m_moved.begin(), m_moved.end(), instance ), m_moved.end() );

		if ( m_indexDirty || m_grids.empty() ) {
			return;
		}

		if ( m_grids.find( instance ) != m_grids.end() ) {
			m_indexDirty = true; // a gridded parent itself is going away
			return;
		}

		const InstanceMap::iterator entry = m_instances.find( PathConstReference( instance->path() ) );
		for ( auto& [ owner, gridded ] : m_grids )
		{
			/* A subtree end pointing at the entry being erased would dangle;
			   step it on to the next entry, which is where the subtree will
			   end once this one is gone. */
			if ( entry != m_instances.end() && gridded.m_subtreeEnd == entry ) {
				gridded.m_subtreeEnd = std::next( entry );
			}
			gridded.m_grid.erase( instance );
		}
	}

	void insert( scene::Instance* instance ) override {
		m_instances.insert( InstanceMap::value_type( PathConstReference( instance->path() ), instance ) );
		indexInsert( instance );
		childBoundsInvalidate();

		m_observer->insert( instance );
	}
	void erase( scene::Instance* instance ) override {
		m_observer->erase( instance );

		indexErase( instance ); // before the map entry goes; it reads it
		m_instances.erase( PathConstReference( instance->path() ) );
		childBoundsInvalidate();
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

	/*! \brief \copydoc scene::Graph::childBounds()

	    Stock behaviour recomputes from scratch, which for worldspawn walks the
	    whole map. It has to happen after a structural change, but not after a
	    brush merely moves - and moving is what happens on every frame of a drag.

	    So between structural changes the cached union is only ever grown, by
	    the bounds of the selection, since an instance can only move while it is
	    selected. Growing keeps the result conservative: a union that is too
	    large costs a little culling accuracy, one that is too small would drop
	    geometry, and only the former can happen here.
	 */
	void childBounds( scene::Instance& instance, AABB& bounds ) override {
		std::size_t count = 0;

		if ( g_largemap_incrementalBounds.m_value ) {
			const auto cached = m_childBounds.find( &instance );
			if ( cached != m_childBounds.end() ) {
				if ( cached->second.m_generation != m_boundsGeneration ) {
					const AABB selected = GlobalSelectionSystem().getBoundsSelected();
					if ( aabb_valid( selected ) ) {
						aabb_extend_by_aabb_safe( cached->second.m_bounds, selected );
					}
					cached->second.m_generation = m_boundsGeneration;
				}
				bounds = cached->second.m_bounds;
				return;
			}
		}

		bounds = AABB();
		traverse_subgraph( AABBAccumulateWalker( bounds, count ), instance.path() );

		// only containers big enough to hurt are worth an entry
		if ( g_largemap_incrementalBounds.m_value && count >= c_minChildrenForBoundsCache ) {
			ChildBounds& entry = m_childBounds[ &instance ];
			entry.m_bounds = bounds;
			entry.m_generation = m_boundsGeneration;
		}
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

		/* Looked up before the pre(), because both outcomes want it: a gridded
		   parent's children are skipped in the map whether it draws or is
		   culled, and stepping over them is the expensive part either way. */
		const auto grid = m_grids.find( self->second );
		const bool gridded = ( grid != m_grids.end() );

		++g_count_instances;
		if ( walker.pre( self->first, *self->second ) ) {
			if ( gridded ) {
				i = grid->second.m_subtreeEnd; // one step, not one per child
				grid->second.m_grid.traverse( walker, volume, *self->second );
			}
			else{
				while ( i != m_instances.end() && i->first.get().size() > depth )
					traverse_visible_recursive( walker, i, volume );
			}
		}
		else if ( gridded ) {
			i = grid->second.m_subtreeEnd; // subtree skipped
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
			GriddedParent& gridded = m_grids[ self->second ];
			for ( scene::Instance* instance : leafChildren )
			{
				gridded.m_grid.insert( instance );
			}
			/* i has just been advanced past the subtree, which is precisely
			   what the traversal needs to jump to. */
			gridded.m_subtreeEnd = i;
		}
	}

	void rebuildIndex(){
		ScopeTimer timer( "  spatial index build" );
		m_grids.clear();

		InstanceMap::iterator i = m_instances.begin();
		while ( i != m_instances.end() )
		{
			buildGrids( i );
		}

		/* Evaluating instance bounds during the build records those instances as
		   moved; drop that last, since the freshly built cells already contain
		   them. */
		m_indexDirty = false;
		m_moved.clear();

		std::size_t cells = 0;
		for ( const auto& [ parent, gridded ] : m_grids )
			cells += gridded.m_grid.cellCount();
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
