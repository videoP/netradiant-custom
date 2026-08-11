/*
   Static geometry batching for the camera's solid pass. See staticbatch.h.
 */

#include "staticbatch.h"

#include "brush.h"
#include "map.h"
#include "largemap.h"
#include "chunkgrid.h"
#include "scopetimer.h"

#include "ientity.h"
#include "ieclass.h"
#include "eclasslib.h"

#include "igl.h"
#include "irender.h"
#include "renderable.h"
#include "iscenegraph.h"
#include "cullable.h"
#include "math/frustum.h"
#include "math/aabb.h"
#include "math/matrix.h"

#include <unordered_map>
#include <vector>
#include <memory>
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cmath>

namespace
{

/// \brief 32 bytes. Deliberately no tangent/bitangent: those are only wanted by
/// the RENDER_BUMP path, which batching declines to serve (see BatchRange::render).
struct BatchVertex
{
	Vertex3f vertex;
	TexCoord2f texcoord;
	Normal3f normal;
};

/// \brief One shader's worth of triangles inside a chunk's vertex buffer.
class BatchRange : public OpenGLRenderable
{
public:
	Shader* m_shader = nullptr;
	GLuint m_vbo = 0;
	GLint m_first = 0;
	GLsizei m_count = 0;

	void render( RenderStateFlags state ) const override {
		if ( ( state & RENDER_BUMP ) != 0 ) {
			return; // no tangent basis in a batch; the unbatched path serves this mode
		}

		gl().glBindBuffer( GL_ARRAY_BUFFER, m_vbo );

		gl().glVertexPointer( 3, GL_FLOAT, sizeof( BatchVertex ),
		                      reinterpret_cast<const GLvoid*>( offsetof( BatchVertex, vertex ) ) );
		if ( ( state & RENDER_TEXTURE ) != 0 ) {
			gl().glTexCoordPointer( 2, GL_FLOAT, sizeof( BatchVertex ),
			                        reinterpret_cast<const GLvoid*>( offsetof( BatchVertex, texcoord ) ) );
		}
		if ( ( state & RENDER_LIGHTING ) != 0 ) {
			gl().glNormalPointer( GL_FLOAT, sizeof( BatchVertex ),
			                      reinterpret_cast<const GLvoid*>( offsetof( BatchVertex, normal ) ) );
		}

		gl().glDrawArrays( GL_TRIANGLES, m_first, m_count );

		// leave the client-array path undisturbed for everything else in the bucket
		gl().glBindBuffer( GL_ARRAY_BUFFER, 0 );
	}
};


/*! \brief One chunk's brush outlines for a single view direction.

    The 2D views draw the edges of front-facing faces only, which for an
    orthographic view depends on the view direction alone - not the position.
    So there are six possible answers per chunk, built on demand, sharing one
    vertex buffer and differing only in which edges they index.
 */
class WireBatch : public OpenGLRenderable
{
public:
	GLuint m_vbo = 0;   // shared with the chunk; positions only
	GLuint m_ibo = 0;
	GLsizei m_count = 0;
	bool m_built = false;

	void render( RenderStateFlags state ) const override {
		gl().glBindBuffer( GL_ARRAY_BUFFER, m_vbo );
		gl().glVertexPointer( 3, GL_FLOAT, sizeof( Vertex3f ), nullptr );
		gl().glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, m_ibo );
		gl().glDrawElements( GL_LINES, m_count, RenderIndexTypeID, nullptr );
		gl().glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
		gl().glBindBuffer( GL_ARRAY_BUFFER, 0 );
	}

	void release(){
		if ( m_ibo != 0 && GlobalOpenGL().contextValid ) {
			gl().glDeleteBuffers( 1, &m_ibo );
		}
		m_ibo = 0;
		m_count = 0;
		m_built = false;
	}
};

/// \brief Index of the view direction, 0..5, for +-X +-Y +-Z.
inline int wire_direction( const Vector3& dir ){
	int axis = 0;
	if ( fabs( dir[1] ) > fabs( dir[axis] ) ) axis = 1;
	if ( fabs( dir[2] ) > fabs( dir[axis] ) ) axis = 2;
	return axis * 2 + ( dir[axis] < 0 ? 1 : 0 );
}

const int c_wireDirections = 6;


class Chunk
{
public:
	AABB m_bounds;

	/* Vertices are world space and drawn with an identity transform, exactly
	   as an unbatched worldspawn brush is. Chunk-relative coordinates would be
	   more precise, but the skybox program is set up once per bucket with an
	   identity model matrix (Renderables_flush), so anything drawn under a
	   different transform gets its sky direction computed wrong and the sky
	   slides with the camera instead of sitting at infinity. Matching stock
	   removes that whole class of assumption. */
	std::vector<BrushInstance*> m_brushes;
	// pointers are handed to the render buckets and must stay put for the frame
	std::vector<std::unique_ptr<BatchRange>> m_ranges;
	GLuint m_vbo = 0;
	bool m_dirty = true;

	/// \brief How many of m_brushes the last build actually took. Less than
	/// m_brushes.size() when one is selected, which is what stops the cell being
	/// skipped so the selected brush can draw its own highlight.
	std::size_t m_batchedCount = 0;

	/* 2D outlines take their colour from the owning entity, and one chunk can
	   hold brushes from several. Only chunks that are purely worldspawn's get
	   an outline batch; the rest draw their outlines individually, in the right
	   colour. The solid pass has no such constraint - it colours by shader. */
	bool m_wireEligible = true;

	// 2D outlines: one vertex buffer, one index set per view direction
	GLuint m_wireVbo = 0;
	std::vector<std::size_t> m_wireBase; // first vertex of each brush, parallel to m_brushes
	WireBatch m_wire[ c_wireDirections ];

	/* No destructor: chunks outlive the GL context at process exit, and calling
	   into GL from a static destructor is not safe. StaticBatchCache::clear()
	   releases the buffers while a context is still current. */

	void releaseGL(){
		const bool live = GlobalOpenGL().contextValid;
		if ( m_vbo != 0 && live ) {
			gl().glDeleteBuffers( 1, &m_vbo );
		}
		m_vbo = 0;
		m_ranges.clear();

		for ( WireBatch& wire : m_wire )
		{
			wire.release();
		}
		if ( m_wireVbo != 0 && live ) {
			gl().glDeleteBuffers( 1, &m_wireVbo );
		}
		m_wireVbo = 0;
		m_wireBase.clear();
	}

	void buildWire( int direction, const VolumeTest& volume );
	bool renderWire( Renderer& renderer, const VolumeTest& volume, int direction, Shader* shader );

	/// \brief Marks the chunk for rebuild and hands its brushes back to the
	/// unbatched path in the meantime, so nothing is ever drawn by neither.
	void soil();

	void build();
	void render( Renderer& renderer, const VolumeTest& volume ) const;
};


class StaticBatchCache
{
public:
	std::unordered_map<std::uint64_t, Chunk> m_chunks;
	bool m_valid = false;
	bool m_active = false;
	bool m_solidSupported = true;
	bool m_wireStyle = false;    // the view being drawn is a 2D outline view
	int m_wireDirection = 0;

	void clear(){
		for ( auto& [ key, chunk ] : m_chunks )
		{
			for ( BrushInstance* brush : chunk.m_brushes )
			{
				brush->m_staticBatched = false;
				brush->m_staticBatchChunk = c_staticBatchNoChunk;
			}
			chunk.releaseGL(); // here rather than in ~Chunk: a live GL context is needed
		}
		m_chunks.clear();
		m_valid = false;
	}

	Chunk* findChunk( std::uint64_t key ){
		const auto i = m_chunks.find( key );
		return i == m_chunks.end() ? nullptr : &i->second;
	}

	void build();
};

StaticBatchCache g_cache;


void Chunk::soil(){
	m_dirty = true;
	m_batchedCount = 0;
	for ( BrushInstance* brush : m_brushes )
	{
		brush->m_staticBatched = false;
	}
}

/*! \brief Whether a brush may be drawn as part of a batch right now.

    Re-tested on every rebuild rather than only at collection, because the
    answer changes during editing: a selected brush draws highlighted and so
    has to draw itself. */
inline bool brush_batchable( BrushInstance& instance ){
	return !instance.isSelected()
	    && !instance.parentSelected()
	    && instance.path().top().get().visible();
}


/*! \brief Brushes radiant could not build a bounding box for.

    Not a batching limitation: such a brush has no winding, so nothing draws it
    on any path. Extreme slivers do this - radiant builds each face by clipping
    a winding the width of the world down, and past roughly 100:1 the result
    collapses. Reported because a map full of them is worth knowing about. */
std::size_t g_reject_aabb;

/* Enough to tell "the map is unusual" from "the walker is wrong". Reported
   only when something was turned away, or when nothing was taken at all. */
std::size_t g_offered, g_accepted, g_reject_notbrush, g_reject_selected,
            g_reject_hidden, g_reject_transform, g_entities_seen;

/*! \brief Collects the batchable brushes of the map.

    Any entity's brushes are taken, not only worldspawn's. Restricting it to
    worldspawn stood in for two notifications that did not exist: a parent
    moving, and parentSelected() changing, neither of which touches the brushes
    underneath. scene::Graph::instanceRenderChanged() now supplies both, and a
    parent carrying a transform of its own is excluded here anyway.

    Measured on a 339k primitive map whose geometry lives under a brush entity
    rather than worldspawn: 6 chunks batched, 155k draw calls a frame, ~5fps.
 */
class BatchableWalker : public scene::Graph::Walker
{
	StaticBatchCache& m_cache;
	scene::Node* m_worldspawn;
	mutable bool m_inWorldspawn = false;
public:
	BatchableWalker( StaticBatchCache& cache, scene::Node* worldspawn )
		: m_cache( cache ), m_worldspawn( worldspawn ){
	}

	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( path.size() == 1 ) {
			return true; // the scene root itself; descend to the entities
		}
		if ( path.size() == 2 ) {
			++g_entities_seen;
			m_inWorldspawn = ( &path.top().get() == m_worldspawn );
			return true; // descend into any entity's primitives
		}
		if ( path.size() != 3 ) {
			return false;
		}

		++g_offered;

		BrushInstance* brush = InstanceTypeCast<BrushInstance>::cast( instance );
		if ( brush == nullptr ) {
			++g_reject_notbrush;
			return false;
		}
		if ( brush->isSelected() || instance.parentSelected() ) {
			++g_reject_selected;
			return false;
		}
		if ( !path.top().get().visible() ) {
			++g_reject_hidden;
			return false;
		}
		if ( !matrix4_affine_equal( instance.localToWorld(), g_matrix4_identity ) ) {
			++g_reject_transform;
			return false;
		}

		const AABB& aabb = instance.worldAABB();
		if ( !aabb_valid( aabb ) ) {
			++g_reject_aabb;
			return false;
		}
		++g_accepted;

		const std::uint64_t key = chunk_key( aabb.origin );
		Chunk& chunk = m_cache.m_chunks[ key ];
		aabb_extend_by_aabb_safe( chunk.m_bounds, aabb );
		chunk.m_brushes.push_back( brush );
		chunk.m_wireEligible = chunk.m_wireEligible && m_inWorldspawn;
		brush->m_staticBatchChunk = key;

		return false;
	}
};


/// \brief Triangulates a brush's unfiltered faces into per-shader vertex lists.
class FaceCollector : public BrushVisitor
{
	std::unordered_map<Shader*, std::vector<BatchVertex>>& m_groups;
public:
	FaceCollector( std::unordered_map<Shader*, std::vector<BatchVertex>>& groups )
		: m_groups( groups ){
	}

	void visit( Face& face ) const override {
		if ( face.isFiltered() ) {
			return;
		}
		const Winding& winding = face.getWinding();
		if ( winding.numpoints < 3 ) {
			return; // collapsed by the b-rep build; nothing draws it either
		}
		Shader* shader = face.getShader().state();
		if ( shader == nullptr ) {
			return;
		}

		const DoubleVector3& n = face.plane3().normal();
		const Normal3f normal( static_cast<float>( n.x() ), static_cast<float>( n.y() ), static_cast<float>( n.z() ) );
		std::vector<BatchVertex>& out = m_groups[ shader ];

		const auto emit = [&]( std::size_t index ){
			const WindingVertex& w = winding.points[ index ];
			out.push_back( BatchVertex{
				Vertex3f( static_cast<float>( w.vertex.x() ),
				          static_cast<float>( w.vertex.y() ),
				          static_cast<float>( w.vertex.z() ) ),
				TexCoord2f( w.texcoord.x(), w.texcoord.y() ),
				normal } );
		};

		for ( std::size_t i = 1; i + 1 < winding.numpoints; ++i ) // fan; brush faces are convex
		{
			emit( 0 );
			emit( i );
			emit( i + 1 );
		}
	}
};


void Chunk::build(){
	releaseGL();

	std::unordered_map<Shader*, std::vector<BatchVertex>> groups;
	m_bounds = AABB();
	m_batchedCount = 0;

	/* Hand every brush back to the unbatched path up front. Only brushes that
	   make it into a range below get taken back, so no brush can end up drawn
	   by neither path. evaluateBRep() below can also re-enter soil() through
	   connectivityChanged(), which this makes harmless. */
	for ( BrushInstance* brush : m_brushes )
	{
		brush->m_staticBatched = false;
	}

	std::vector<BrushInstance*> included;
	included.reserve( m_brushes.size() );

	for ( BrushInstance* brush : m_brushes )
	{
		if ( !brush_batchable( *brush ) ) {
			continue; // draws itself, so its highlight shows
		}
		brush->getBrush().evaluateBRep();
		brush->getBrush().forEachFace( FaceCollector( groups ) );
		aabb_extend_by_aabb_safe( m_bounds, brush->worldAABB() );
		included.push_back( brush );
	}

	std::size_t total = 0;
	for ( const auto& [ shader, verts ] : groups )
		total += verts.size();

	m_dirty = false;

	if ( total == 0 ) {
		return; // nothing to draw; brushes stay unbatched
	}

	std::vector<BatchVertex> buffer;
	buffer.reserve( total );
	m_ranges.reserve( groups.size() );

	for ( const auto& [ shader, verts ] : groups )
	{
		if ( verts.empty() ) {
			continue;
		}
		auto range = std::make_unique<BatchRange>();
		range->m_shader = shader;
		range->m_first = static_cast<GLint>( buffer.size() );
		range->m_count = static_cast<GLsizei>( verts.size() );
		buffer.insert( buffer.end(), verts.begin(), verts.end() );
		m_ranges.push_back( std::move( range ) );
	}

	gl().glGenBuffers( 1, &m_vbo );
	gl().glBindBuffer( GL_ARRAY_BUFFER, m_vbo );
	gl().glBufferData( GL_ARRAY_BUFFER, buffer.size() * sizeof( BatchVertex ), buffer.data(), GL_STATIC_DRAW );
	gl().glBindBuffer( GL_ARRAY_BUFFER, 0 );

	for ( auto& range : m_ranges )
	{
		range->m_vbo = m_vbo;
	}

	for ( BrushInstance* brush : included )
	{
		brush->m_staticBatched = true;
	}
	m_batchedCount = included.size();
}


/*! \brief Gathers this chunk's brush outlines as seen from \p direction.

    The vertex buffer is shared across directions and built once. Face
    visibility is evaluated with the real volume, which is valid to cache per
    direction only because the caller guarantees an orthographic view.
 */
void Chunk::buildWire( int direction, const VolumeTest& volume ){
	WireBatch& wire = m_wire[ direction ];
	wire.release();

	const bool needVertices = ( m_wireVbo == 0 );
	std::vector<Vertex3f> vertices;
	if ( needVertices ) {
		m_wireBase.clear();
		m_wireBase.reserve( m_brushes.size() );
	}

	std::vector<RenderIndex> indices;

	for ( std::size_t b = 0; b < m_brushes.size(); ++b )
	{
		BrushInstance& instance = *m_brushes[ b ];
		Brush& brush = instance.getBrush();
		brush.evaluateBRep();

		const std::size_t base = needVertices ? vertices.size() : m_wireBase[ b ];
		if ( needVertices ) {
			m_wireBase.push_back( base );
			for ( const auto& point : brush.getUniqueVertexPoints() )
			{
				vertices.push_back( point.vertex );
			}
		}

		/* Kept in step with the solid batch: a brush drawing itself must not
		   also have its outline in here, or a selected brush would show both
		   its highlight and a plain outline over it. */
		if ( !instance.m_staticBatched ) {
			continue;
		}

		bool faces_visible[ c_brush_maxFaces ];
		{
			std::size_t f = 0;
			for ( const auto& face : brush )
			{
				faces_visible[ f++ ] = face->intersectVolume( volume, instance.localToWorld() );
			}
		}

		const auto& edgeIndices = brush.getEdgeIndices();
		const auto& edgeFaces = brush.getEdgeFaces();
		for ( std::size_t e = 0; e < edgeFaces.size(); ++e )
		{
			if ( faces_visible[ edgeFaces[ e ].first ] || faces_visible[ edgeFaces[ e ].second ] ) {
				indices.push_back( static_cast<RenderIndex>( base + edgeIndices[ e ].first ) );
				indices.push_back( static_cast<RenderIndex>( base + edgeIndices[ e ].second ) );
			}
		}
	}

	if ( needVertices ) {
		if ( vertices.empty() ) {
			return;
		}
		gl().glGenBuffers( 1, &m_wireVbo );
		gl().glBindBuffer( GL_ARRAY_BUFFER, m_wireVbo );
		gl().glBufferData( GL_ARRAY_BUFFER, vertices.size() * sizeof( Vertex3f ), vertices.data(), GL_STATIC_DRAW );
		gl().glBindBuffer( GL_ARRAY_BUFFER, 0 );
	}

	wire.m_vbo = m_wireVbo;
	wire.m_built = true;
	if ( indices.empty() ) {
		return;
	}

	gl().glGenBuffers( 1, &wire.m_ibo );
	gl().glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, wire.m_ibo );
	gl().glBufferData( GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof( RenderIndex ), indices.data(), GL_STATIC_DRAW );
	gl().glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	wire.m_count = static_cast<GLsizei>( indices.size() );
}

bool Chunk::renderWire( Renderer& renderer, const VolumeTest& volume, int direction, Shader* shader ){
	if ( m_dirty || m_brushes.empty() || !m_wireEligible ) {
		return false;
	}
	/* Cull before building: an off-screen chunk is never asked about coverage,
	   so building it would only stall the first frame of a new direction. */
	if ( volume.TestAABB( m_bounds ) == c_volumeOutside ) {
		return false;
	}
	if ( !m_wire[ direction ].m_built ) {
		buildWire( direction, volume );
	}
	if ( m_wire[ direction ].m_count != 0 ) {
		renderer.SetState( shader, Renderer::eWireframeOnly );
		renderer.addRenderable( m_wire[ direction ], g_matrix4_identity );
	}
	return true;
}

void Chunk::render( Renderer& renderer, const VolumeTest& volume ) const {
	/* A soiled chunk's ranges still describe the old brush set, and its brushes
	   have already been handed back to the unbatched path, so drawing them here
	   too would double up. Waiting for the rebuild costs nothing but a frame. */
	if ( m_dirty || m_ranges.empty() || volume.TestAABB( m_bounds ) == c_volumeOutside ) {
		return;
	}
	for ( const auto& range : m_ranges )
	{
		renderer.SetState( range->m_shader, Renderer::eFullMaterials );
		renderer.addRenderable( *range, g_matrix4_identity );
	}
}


void StaticBatchCache::build(){
	ScopeTimer timer( "  static batch build" );
	clear();

	scene::Node* worldspawn = Map_GetWorldspawn( g_map );
	if ( worldspawn == nullptr ) {
		/* Deliberately left invalid so this is retried. Worldspawn is only
		   identified after the scene root is inserted, and the loading progress
		   window pumps the event loop in between - so a repaint lands here
		   mid-load, and marking the empty result valid would cache "nothing is
		   batchable" for the rest of the session. */
		return;
	}

	g_reject_aabb = g_offered = g_accepted = g_reject_notbrush = g_reject_selected
	              = g_reject_hidden = g_reject_transform = g_entities_seen = 0;

	GlobalSceneGraph().traverse( BatchableWalker( *this, worldspawn ) );

	std::size_t ranges = 0;
	for ( auto& [ key, chunk ] : m_chunks )
	{
		chunk.build();
		ranges += chunk.m_ranges.size();
	}

	m_valid = true;

	globalOutputStream() << "static batches: " << Unsigned( m_chunks.size() ) << " chunks, "
	                     << Unsigned( ranges ) << " draw calls for the whole map\n";
	if ( g_reject_aabb != 0 ) {
		globalOutputStream() << "  " << Unsigned( g_reject_aabb )
		                     << " brushes have no valid bounds, so nothing draws them at all"
		                        " - check for extreme slivers\n";
	}
}

} // namespace


bool StaticBatch_begin( Renderer& renderer, const VolumeTest& volume ){
	if ( !g_largemap_staticBatch.m_value || !GlobalOpenGL().contextValid ) {
		return false;
	}

	/* eWireframeOnly means one of the 2D views. Those are orthographic, so
	   which faces front-face depends on the view direction alone, which is
	   what makes a cached outline per direction valid. */
	const bool wireStyle = ( renderer.getStyle() == Renderer::eWireframeOnly );
	if ( !wireStyle && !g_cache.m_solidSupported ) {
		return false;
	}

	if ( !g_cache.m_valid ) {
		g_cache.build();
	}

	Shader* wireShader = nullptr;
	if ( wireStyle ) {
		/* 2D outlines inherit their colour from the owning entity, which for
		   batched geometry is always worldspawn. */
		scene::Node* worldspawn = Map_GetWorldspawn( g_map );
		Entity* entity = worldspawn != nullptr ? Node_getEntity( *worldspawn ) : nullptr;
		if ( entity == nullptr ) {
			return false;
		}
		wireShader = entity->getEntityClass().m_state_wire;
		if ( wireShader == nullptr ) {
			return false;
		}
	}

	g_cache.m_wireStyle = wireStyle;
	g_cache.m_wireDirection = wire_direction( volume.getViewDir() );

	/* Select-all soils every chunk at once. Rebuilding them all in one frame
	   would freeze; a chunk left dirty simply draws unbatched for another frame,
	   so the map catches up over a few frames instead of stalling. */
	const std::size_t c_rebuildBudget = 128;
	std::size_t rebuilt = 0;

	renderer.PushState();
	for ( auto& [ key, chunk ] : g_cache.m_chunks )
	{
		if ( chunk.m_dirty ) {
			if ( rebuilt >= c_rebuildBudget ) {
				continue;
			}
			chunk.build();
			++rebuilt;
		}
		if ( wireStyle ) {
			chunk.renderWire( renderer, volume, g_cache.m_wireDirection, wireShader );
		}
		else{
			chunk.render( renderer, volume );
		}
	}
	renderer.PopState();

	g_cache.m_active = true;
	return true;
}

void StaticBatch_end(){
	g_cache.m_active = false;
}

bool StaticBatch_active(){
	return g_cache.m_active;
}

void StaticBatch_setSolidSupported( bool supported ){
	g_cache.m_solidSupported = supported;
}

bool StaticBatch_cellCovered( std::uint64_t key, std::size_t instanceCount ){
	if ( !g_cache.m_active ) {
		return false;
	}

	const Chunk* chunk = g_cache.findChunk( key );
	if ( chunk == nullptr || chunk->m_dirty ) {
		return false;
	}
	/* The count test is the safety net: anything in the cell that batching
	   declined to take - a patch, a hidden brush, or a selected one that has to
	   draw its own highlight - makes the counts disagree, and the cell gets
	   walked so that thing gets its chance to draw. */
	if ( chunk->m_batchedCount != instanceCount ) {
		return false;
	}
	return g_cache.m_wireStyle
	     ? ( chunk->m_wireEligible && chunk->m_wire[ g_cache.m_wireDirection ].m_built )
	     : !chunk->m_ranges.empty();
}

void StaticBatch_brushChanged( const BrushInstance& instance ){
	/* Keyed on chunk membership, not on m_staticBatched. A brush that left the
	   batch because it was selected has that flag clear already, so testing it
	   would swallow the deselect and the chunk would never take the brush back. */
	if ( instance.m_staticBatchChunk == c_staticBatchNoChunk ) {
		return;
	}
	if ( Chunk* chunk = g_cache.findChunk( instance.m_staticBatchChunk ) ) {
		chunk->soil();
	}
}

void StaticBatch_instanceChanged( scene::Instance& instance ){
	if ( g_cache.m_chunks.empty() ) {
		return; // nothing batched, so nothing can be stale
	}
	if ( BrushInstance* brush = InstanceTypeCast<BrushInstance>::cast( instance ) ) {
		StaticBatch_brushChanged( *brush );
	}
}

void StaticBatch_brushRemoved( const BrushInstance& instance ){
	if ( instance.m_staticBatchChunk == c_staticBatchNoChunk ) {
		return;
	}
	if ( Chunk* chunk = g_cache.findChunk( instance.m_staticBatchChunk ) ) {
		chunk->soil();
		const auto i = std::find( chunk->m_brushes.begin(), chunk->m_brushes.end(),
		                          const_cast<BrushInstance*>( &instance ) );
		if ( i != chunk->m_brushes.end() ) {
			chunk->m_brushes.erase( i );
		}
	}
}

void StaticBatch_invalidate(){
	g_cache.clear();
}

void StaticBatch_release(){
	g_cache.clear();
}
