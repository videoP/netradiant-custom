#include "brush_builder.h"

#include "debugging/debugging.h"
#include "ibrush.h"
#include "ientity.h"
#include "ieclass.h"
#include "iscenegraph.h"
#include "qerplugin.h"
#include "scenelib.h"

const unsigned int BRUSH_DETAIL_FLAG = 27;
const unsigned int BRUSH_DETAIL_MASK = ( 1 << BRUSH_DETAIL_FLAG );

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void fill_face( _QERFaceData& face,
                        double x0, double y0, double z0,
                        double x1, double y1, double z1,
                        double x2, double y2, double z2,
                        const char* shader ){
	face.m_p0[0] = x0; face.m_p0[1] = y0; face.m_p0[2] = z0;
	face.m_p1[0] = x1; face.m_p1[1] = y1; face.m_p1[2] = z1;
	face.m_p2[0] = x2; face.m_p2[1] = y2; face.m_p2[2] = z2;
	face.m_shader   = shader;
	face.m_texdef.scale[0] = 0.03125f;  // matches ( ( 0.03125 0 0 ) ( 0 0.03125 0 ) ) in .map
	face.m_texdef.scale[1] = 0.03125f;
	face.m_texdef.shift[0] = 0;
	face.m_texdef.shift[1] = 0;
	face.m_texdef.rotate   = 0;
	face.contents = BRUSH_DETAIL_MASK;
	face.flags    = 0;
	face.value    = 0;
}

static scene::Node& create_func_group(){
	EntityClass* ec = GlobalEntityClassManager().findOrInsert( "func_group", true );
	NodeSmartReference entity( GlobalEntityCreator().createEntity( ec ) );
	Node_getTraversable( GlobalSceneGraph().root() )->insert( entity );
	return entity;
}

// Select a generated func_group so the user can immediately move it. For a group
// node Entity_setSelected selects its child brushes (what the manipulator acts
// on), which is how Radiant makes a group movable — selecting the entity instance
// directly leaves it unmovable.
static void select_generated( scene::Node& node ){
	scene::Path path( makeReference( GlobalSceneGraph().root() ) );
	path.push( makeReference( node ) );
	if ( scene::Instance* instance = GlobalSceneGraph().find( path ) )
		Entity_setSelected( *instance, true );
}

// bl/tl/br/tr carry each corner's own X/Y position along with its sampled
// height: bl.x==x, tl.x==x, br.x==mx, tr.x==mx etc.
static void insert_brush_into( scene::Node& entity, double min_z, double base_max_z,
                                const GridPoint& bl, const GridPoint& tl,
                                const GridPoint& br, const GridPoint& tr,
                                const char* top_tex, const char* caulk,
                                bool split_diagonally, bool alt_dir ){
	if ( !split_diagonally ) {
		NodeSmartReference brush( GlobalBrushCreator().createBrush() );
		_QERFaceData face;
		fill_face( face, bl.x, bl.y, base_max_z,   tl.x, tl.y, base_max_z,   br.x, br.y, base_max_z,   top_tex );
		GlobalBrushCreator().Brush_addFace( brush, face );
		fill_face( face, bl.x, bl.y, min_z,        br.x, br.y, min_z,        tl.x, tl.y, min_z,         caulk );
		GlobalBrushCreator().Brush_addFace( brush, face );
		fill_face( face, br.x, br.y, min_z,        br.x, br.y, base_max_z,   tr.x, tr.y, min_z,         caulk );
		GlobalBrushCreator().Brush_addFace( brush, face );
		fill_face( face, bl.x, bl.y, min_z,        tl.x, tl.y, min_z,        bl.x, bl.y, base_max_z,    caulk );
		GlobalBrushCreator().Brush_addFace( brush, face );
		fill_face( face, tl.x, tl.y, min_z,        tr.x, tr.y, min_z,        tl.x, tl.y, base_max_z,    caulk );
		GlobalBrushCreator().Brush_addFace( brush, face );
		fill_face( face, bl.x, bl.y, min_z,        bl.x, bl.y, base_max_z,   br.x, br.y, min_z,         caulk );
		GlobalBrushCreator().Brush_addFace( brush, face );
		Node_getTraversable( entity )->insert( brush );
	}
	else if ( !alt_dir ) {
		// Standard diagonal: BL→TR split
		// Triangle 1: BL, TL, BR
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			_QERFaceData face;
			fill_face( face, bl.x, bl.y, bl.z,   tl.x, tl.y, tl.z,   br.x, br.y, br.z,   top_tex );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, bl.x, bl.y, min_z,  br.x, br.y, min_z,  tl.x, tl.y, min_z,   caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, bl.x, bl.y, min_z,  tl.x, tl.y, min_z,  bl.x, bl.y, base_max_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, bl.x, bl.y, min_z,  bl.x, bl.y, base_max_z, br.x, br.y, min_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, br.x, br.y, min_z,  br.x, br.y, base_max_z, tl.x, tl.y, min_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
		// Triangle 2: TR, BR, TL
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			_QERFaceData face;
			fill_face( face, tr.x, tr.y, tr.z,   br.x, br.y, br.z,   tl.x, tl.y, tl.z,   top_tex );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, tr.x, tr.y, min_z,  tl.x, tl.y, min_z,  br.x, br.y, min_z,   caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, br.x, br.y, min_z,  br.x, br.y, base_max_z, tr.x, tr.y, min_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, tl.x, tl.y, min_z,  tr.x, tr.y, min_z,  tl.x, tl.y, base_max_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, tl.x, tl.y, min_z,  tl.x, tl.y, base_max_z, br.x, br.y, min_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
	}
	else {
		// Alternate diagonal: TL→BR split (checkerboard pattern)
		// Triangle 1: TL, TR, BL
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			_QERFaceData face;
			fill_face( face, tl.x, tl.y, tl.z,   tr.x, tr.y, tr.z,   bl.x, bl.y, bl.z,   top_tex );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, tl.x, tl.y, min_z,  bl.x, bl.y, min_z,  tr.x, tr.y, min_z,   caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, bl.x, bl.y, min_z,  tl.x, tl.y, min_z,  bl.x, bl.y, base_max_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, tl.x, tl.y, min_z,  tr.x, tr.y, min_z,  tl.x, tl.y, base_max_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, tr.x, tr.y, min_z,  bl.x, bl.y, min_z,  bl.x, bl.y, base_max_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
		// Triangle 2: TR, BR, BL
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			_QERFaceData face;
			fill_face( face, tr.x, tr.y, tr.z,   br.x, br.y, br.z,   bl.x, bl.y, bl.z,   top_tex );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, tr.x, tr.y, min_z,  bl.x, bl.y, min_z,  br.x, br.y, min_z,   caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, br.x, br.y, min_z,  br.x, br.y, base_max_z, tr.x, tr.y, min_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, bl.x, bl.y, min_z,  bl.x, bl.y, base_max_z, br.x, br.y, min_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, bl.x, bl.y, min_z,  tr.x, tr.y, min_z,  bl.x, bl.y, base_max_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
	}
}

// ---------------------------------------------------------------------------
// Standard terrain
// ---------------------------------------------------------------------------

void build_terrain_brushes( const BrushData& target, double step_x, double step_y,
                             const TerrainMap& height_map, const char* top_texture,
                             bool split_diagonally ){
	// Undo is started by the caller so deletion + generation form one step.
	scene::Node& entity = create_func_group();

	const char* caulk = "textures/common/caulk";
	double min_z      = target.min_z;
	double base_max_z = target.max_z;

	auto r2 = []( double v ) { return std::round( v * 100.0 ) / 100.0; };

	int x_index = 0;
	for ( double x = target.min_x; x < target.max_x - 0.01; x += step_x, ++x_index ) {
		int y_index = 0;
		for ( double y = target.min_y; y < target.max_y - 0.01; y += step_y, ++y_index ) {
			double mx = x + step_x < target.max_x ? x + step_x : target.max_x;
			double my = y + step_y < target.max_y ? y + step_y : target.max_y;

			auto lookup = [&]( double kx, double ky ) -> GridPoint {
				auto it = height_map.find({ r2( kx ), r2( ky ) });
				return it != height_map.end() ? it->second : GridPoint{ kx, ky, min_z };
			};

			GridPoint bl = lookup( x,  y  );
			GridPoint tl = lookup( x,  my );
			GridPoint br = lookup( mx, y  );
			GridPoint tr = lookup( mx, my );

			bool alt_dir = ( ( x_index + y_index ) % 2 ) != 0;

			insert_brush_into( entity, min_z, base_max_z,
			                   bl, tl, br, tr,
			                   top_texture, caulk, split_diagonally, alt_dir );
		}
	}

	select_generated( entity );
	SceneChangeNotify();
}

// ---------------------------------------------------------------------------
// Tunnel terrain
// ---------------------------------------------------------------------------

static void insert_floor_ceil_brush( scene::Node& entity,
                                      double x,  double y,  double mx, double my,
                                      double z_bl, double z_tl, double z_br, double z_tr,
                                      double min_z, double solid_top,
                                      const char* top_tex, const char* caulk,
                                      bool is_ceiling, bool alt_dir ){
	_QERFaceData face;

	if ( !alt_dir ) {
		// BL→TR split
		// Triangle 1: BL, TL, BR
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fill_face( face, x,  y,  z_bl,      x,  my, z_tl,      mx, y,  z_br,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, x,  y,  min_z,     mx, y,  min_z,     x,  my, min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fill_face( face, x,  y,  z_bl,      mx, y,  z_br,      x,  my, z_tl,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, x,  y,  solid_top, x,  my, solid_top, mx, y,  solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fill_face( face, x,  y,  min_z, x,  my, min_z,    x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  y,  min_z, x,  y,  solid_top, mx, y,  min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, mx, y,  min_z, mx, y,  solid_top, x,  my, min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
		// Triangle 2: TR, BR, TL
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fill_face( face, mx, my, z_tr,      mx, y,  z_br,      x,  my, z_tl,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, mx, my, min_z,     x,  my, min_z,     mx, y,  min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fill_face( face, mx, my, z_tr,      x,  my, z_tl,      mx, y,  z_br,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, mx, my, solid_top, mx, y,  solid_top, x,  my, solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fill_face( face, mx, y,  min_z, mx, y,  solid_top, mx, my, min_z,     caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  my, min_z, mx, my, min_z,     x,  my, solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  my, min_z, x,  my, solid_top, mx, y,  min_z,     caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
	}
	else {
		// TL→BR split
		// Triangle 1: TL, TR, BL
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fill_face( face, x,  my, z_tl,      mx, my, z_tr,      x,  y,  z_bl,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, x,  my, min_z,     x,  y,  min_z,     mx, my, min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fill_face( face, x,  my, z_tl,      x,  y,  z_bl,      mx, my, z_tr,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, x,  my, solid_top, mx, my, solid_top, x,  y,  solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fill_face( face, x,  y,  min_z, x,  my, min_z,    x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  my, min_z, mx, my, min_z,    x,  my, solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, mx, my, min_z, x,  y,  min_z,    x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
		// Triangle 2: TR, BR, BL
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fill_face( face, mx, my, z_tr,      mx, y,  z_br,      x,  y,  z_bl,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, mx, my, min_z,     x,  y,  min_z,     mx, y,  min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fill_face( face, mx, my, z_tr,      x,  y,  z_bl,      mx, y,  z_br,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, mx, my, solid_top, mx, y,  solid_top, x,  y,  solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fill_face( face, mx, y,  min_z, mx, y,  solid_top, mx, my, min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  y,  min_z, x,  y,  solid_top, mx, y,  min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  y,  min_z, mx, my, min_z,     x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
	}
}

static void insert_wall_brush( scene::Node& entity,
                                double x_bl, double x_tl, double x_br, double x_tr,
                                double gu, double gmx_u, double gz, double gmx_z,
                                double outer, double limit,
                                const char* top_tex, const char* caulk,
                                bool is_left, bool alt_dir, Axis axis ){
	// Wall geometry is floor/ceiling rotated 90°: world(fz,fx,fy) for a
	// Y-running tunnel. That's a proper rotation, so winding is unchanged; for
	// an X-running tunnel the same axis swap is a mirror image instead, so
	// v1/v2 are swapped per face below to keep normals pointing outward.
	_QERFaceData face;
	const bool   is_ceiling = !is_left;
	const double min_z      = is_left ? outer : limit;
	const double solid_top  = is_left ? limit  : outer;

	// Aliases to match insert_floor_ceil_brush variable names exactly.
	const double x = gu, y = gz, mx = gmx_u, my = gmx_z;
	const double z_bl = x_bl, z_tl = x_tl, z_br = x_br, z_tr = x_tr;

	// fw: emit a face using floor coordinate order but with world axes permuted.
	auto fw = [&]( double fx0, double fy0, double fz0,
	               double fx1, double fy1, double fz1,
	               double fx2, double fy2, double fz2,
	               const char* shader ){
		if ( axis == Axis::Y )
			fill_face( face, fz0, fx0, fy0, fz1, fx1, fy1, fz2, fx2, fy2, shader );
		else
			fill_face( face, fx0, fz0, fy0, fx2, fz2, fy2, fx1, fz1, fy1, shader );
	};

	if ( !alt_dir ) {
		// BL→TR split
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fw( x,  y,  z_bl,      x,  my, z_tl,      mx, y,  z_br,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( x,  y,  min_z,     mx, y,  min_z,     x,  my, min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fw( x,  y,  z_bl,      mx, y,  z_br,      x,  my, z_tl,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( x,  y,  solid_top, x,  my, solid_top, mx, y,  solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fw( x,  y,  min_z, x,  my, min_z,    x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( x,  y,  min_z, x,  y,  solid_top, mx, y,  min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( mx, y,  min_z, mx, y,  solid_top, x,  my, min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fw( mx, my, z_tr,      mx, y,  z_br,      x,  my, z_tl,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( mx, my, min_z,     x,  my, min_z,     mx, y,  min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fw( mx, my, z_tr,      x,  my, z_tl,      mx, y,  z_br,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( mx, my, solid_top, mx, y,  solid_top, x,  my, solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fw( mx, y,  min_z, mx, y,  solid_top, mx, my, min_z,     caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( x,  my, min_z, mx, my, min_z,     x,  my, solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( x,  my, min_z, x,  my, solid_top, mx, y,  min_z,     caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
	}
	else {
		// TL→BR split
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fw( x,  my, z_tl,      mx, my, z_tr,      x,  y,  z_bl,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( x,  my, min_z,     x,  y,  min_z,     mx, my, min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fw( x,  my, z_tl,      x,  y,  z_bl,      mx, my, z_tr,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( x,  my, solid_top, mx, my, solid_top, x,  y,  solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fw( x,  y,  min_z, x,  my, min_z,    x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( x,  my, min_z, mx, my, min_z,    x,  my, solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( mx, my, min_z, x,  y,  min_z,    x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fw( mx, my, z_tr,      mx, y,  z_br,      x,  y,  z_bl,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( mx, my, min_z,     x,  y,  min_z,     mx, y,  min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fw( mx, my, z_tr,      x,  y,  z_bl,      mx, y,  z_br,      top_tex ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( mx, my, solid_top, mx, y,  solid_top, x,  y,  solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fw( mx, y,  min_z, mx, y,  solid_top, mx, my, min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( x,  y,  min_z, x,  y,  solid_top, mx, y,  min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( x,  y,  min_z, mx, my, min_z,     x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
	}
}

void build_tunnel_brushes( const BrushData& target, double step_x, double step_y,
                            const TunnelMaps& maps, const char* top_texture,
                            double cave_height, double slope_height, Axis axis ){
	// Undo is started by the caller so deletion + generation form one step.
	scene::Node& floor_entity  = create_func_group();
	scene::Node& ceil_entity   = create_func_group();
	scene::Node& lwall_entity  = create_func_group();
	scene::Node& rwall_entity  = create_func_group();

	const char* caulk    = "textures/common/caulk";
	double min_z         = target.min_z;
	// Highest ceiling point — slope_height may be negative (downward slope),
	// so the ceiling peak is always at the high end of the slope.
	double max_ceil_z    = target.max_z + cave_height + std::max( 0.0, slope_height );
	double ceil_solid_top = max_ceil_z + target.height_z;

	auto r2 = []( double v ) { return std::round( v * 100.0 ) / 100.0; };

	// Floor and ceiling
	int x_index = 0;
	for ( double x = target.min_x; x < target.max_x - 0.01; x += step_x, ++x_index ) {
		int y_index = 0;
		for ( double y = target.min_y; y < target.max_y - 0.01; y += step_y, ++y_index ) {
			double mx = x + step_x < target.max_x ? x + step_x : target.max_x;
			double my = y + step_y < target.max_y ? y + step_y : target.max_y;

			auto safe_at = [&]( const HeightMap& m, double kx, double ky, double fallback ) -> double {
				auto it = m.find( { r2( kx ), r2( ky ) } );
				if ( it == m.end() ) {
					globalErrorStream() << "TerrainGenerator: height map missing key ("
					                    << kx << ", " << ky << ") - using fallback\n";
					return fallback;
				}
				return it->second;
			};

			double f_bl = safe_at( maps.floor_map,   x,  y,  min_z );
			double f_tl = safe_at( maps.floor_map,   x,  my, min_z );
			double f_br = safe_at( maps.floor_map,   mx, y,  min_z );
			double f_tr = safe_at( maps.floor_map,   mx, my, min_z );
			double c_bl = safe_at( maps.ceiling_map, x,  y,  max_ceil_z );
			double c_tl = safe_at( maps.ceiling_map, x,  my, max_ceil_z );
			double c_br = safe_at( maps.ceiling_map, mx, y,  max_ceil_z );
			double c_tr = safe_at( maps.ceiling_map, mx, my, max_ceil_z );

			bool alt_dir = ( ( x_index + y_index ) % 2 ) != 0;

			insert_floor_ceil_brush( floor_entity, x, y, mx, my,
			                         f_bl, f_tl, f_br, f_tr,
			                         min_z, max_ceil_z, top_texture, caulk, false, alt_dir );
			insert_floor_ceil_brush( ceil_entity, x, y, mx, my,
			                         c_bl, c_tl, c_br, c_tr,
			                         min_z, ceil_solid_top, top_texture, caulk, true, alt_dir );
		}
	}

	// Walls — must match the range generated by terrain_engine. Grid runs along
	// Y (left/right walls at fixed X) for a Y-running tunnel, or along X
	// (near/far walls at fixed Y) for an X-running tunnel.
	// slope_height may be negative (downward slope), so clamp accordingly.
	double wall_min_z   = target.max_z + std::min( 0.0, slope_height );
	double wall_max_z   = wall_min_z + cave_height + std::abs( slope_height );
	double step_z       = maps.step_z;

	const bool   along_y    = ( axis == Axis::Y );
	const double along_min  = along_y ? target.min_y : target.min_x;
	const double along_max  = along_y ? target.max_y : target.max_x;
	const double along_step = along_y ? step_y       : step_x;
	const double outer_lo   = along_y ? target.min_x - step_x : target.min_y - step_y;
	const double outer_hi   = along_y ? target.max_x + step_x : target.max_y + step_y;
	const double limit      = along_y ? ( target.min_x + target.max_x ) / 2.0
	                                   : ( target.min_y + target.max_y ) / 2.0;

	int gu_index = 0;
	for ( double gu = along_min; gu < along_max - 0.01; gu += along_step, ++gu_index ) {
		int gz_index = 0;
		for ( double gz = wall_min_z; gz < wall_max_z - 0.01; gz += step_z, ++gz_index ) {
			double gmx_u = gu + along_step < along_max ? gu + along_step : along_max;
			double gmx_z = gz + step_z    < wall_max_z ? gz + step_z    : wall_max_z;

			auto safe_wall = [&]( const HeightMap& m, double ku, double kz, double fallback ) -> double {
				auto it = m.find( { r2( ku ), r2( kz ) } );
				if ( it == m.end() ) {
					globalErrorStream() << "TerrainGenerator: wall map missing key ("
					                    << ku << ", " << kz << ") - using fallback\n";
					return fallback;
				}
				return it->second;
			};

			double lx_bl = safe_wall( maps.left_wall_map,  gu,     gz,     limit );
			double lx_tl = safe_wall( maps.left_wall_map,  gu,     gmx_z,  limit );
			double lx_br = safe_wall( maps.left_wall_map,  gmx_u,  gz,     limit );
			double lx_tr = safe_wall( maps.left_wall_map,  gmx_u,  gmx_z,  limit );

			double rx_bl = safe_wall( maps.right_wall_map, gu,     gz,     limit );
			double rx_tl = safe_wall( maps.right_wall_map, gu,     gmx_z,  limit );
			double rx_br = safe_wall( maps.right_wall_map, gmx_u,  gz,     limit );
			double rx_tr = safe_wall( maps.right_wall_map, gmx_u,  gmx_z,  limit );

			bool alt_dir = ( ( gu_index + gz_index ) % 2 ) != 0;

			insert_wall_brush( lwall_entity, lx_bl, lx_tl, lx_br, lx_tr,
			                   gu, gmx_u, gz, gmx_z, outer_lo, limit, top_texture, caulk, true,  alt_dir, axis );
			insert_wall_brush( rwall_entity, rx_bl, rx_tl, rx_br, rx_tr,
			                   gu, gmx_u, gz, gmx_z, outer_hi, limit, top_texture, caulk, false, alt_dir, axis );
		}
	}

	select_generated( floor_entity );
	select_generated( ceil_entity );
	select_generated( lwall_entity );
	select_generated( rwall_entity );
	SceneChangeNotify();
}
