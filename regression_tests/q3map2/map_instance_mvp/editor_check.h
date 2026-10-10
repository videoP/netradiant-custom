// Build radiant/main.cpp with -DRADIANT_MAP_INSTANCE_TEST and open a disposable
// copy of maps/master.map. Results are written beside that map; the app exits.
#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <QApplication>
#include <QEvent>
#include <QWidget>
#include <QDockWidget>
#include <QTreeWidget>
#include <QPushButton>
#include "../../../radiant/instancespanel.h"
#include "../../../radiant/mainframe.h"
#include "ientity.h"
#include "iundo.h"
#include "scenelib.h"
#include "renderable.h"
#include "render.h"
#include "selectable.h"
#include "../../../radiant/view.h"
#include "../../../radiant/selection_render.h"
#include "../../../radiant/selection_volume.h"
#include "../../../radiant/selection_selector.h"
#ifdef WIN32
#include <windows.h>
#endif

void Scene_TestSelect_Primitive( Selector&, SelectionTest&, const VolumeTest& );
void Scene_TestSelect_Entity( Selector&, SelectionTest&, const VolumeTest& );

namespace instance_editor_test {
class HideTestWindows : public QObject {
	bool eventFilter( QObject* object, QEvent* event ) override {
		if ( event->type() == QEvent::Show && object->isWidgetType() ) {
			auto* widget = static_cast<QWidget*>( object );
			if ( widget->isWindow() )
				widget->move( -30000, -30000 );
		}
		return false;
	}
};
inline void installWindowFilter( QApplication& app ){
	static HideTestWindows filter;
	app.installEventFilter( &filter );
}
#ifdef WIN32
inline LONG WINAPI crashTrace( EXCEPTION_POINTERS* exception ){
	std::ofstream trace( "editor-crash.txt" );
	CONTEXT context = *exception->ContextRecord;
	trace << "exception " << std::hex << exception->ExceptionRecord->ExceptionCode << std::endl;
	for ( int frame = 0; frame < 24 && context.Rip != 0; ++frame ) {
		HMODULE module;
		if ( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<const char*>( context.Rip ), &module ) ) {
			char name[MAX_PATH];
			GetModuleFileNameA( module, name, MAX_PATH );
			trace << name << " rva " << ( context.Rip - reinterpret_cast<DWORD64>( module ) ) << std::endl;
		}
		DWORD64 base;
		if ( PRUNTIME_FUNCTION function = RtlLookupFunctionEntry( context.Rip, &base, nullptr ) ) {
			void* handler;
			DWORD64 establisher;
			RtlVirtualUnwind( UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handler, &establisher, nullptr );
		}
		else {
			context.Rip = *reinterpret_cast<const DWORD64*>( context.Rsp );
			context.Rsp += sizeof( DWORD64 );
		}
	}
	trace.close();
	TerminateProcess( GetCurrentProcess(), 1 );
	return EXCEPTION_EXECUTE_HANDLER;
}
inline void installCrashTrace(){
	SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX );
	SetUnhandledExceptionFilter( crashTrace );
}
#else
inline void installCrashTrace(){}
#endif
class Gather : public scene::Graph::Walker {
public:
	mutable std::vector<scene::Instance*> owners;
	mutable std::vector<scene::Instance*> previews;
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( !Instance_isInstancePreview( instance ) && path.top().get().excluded( scene::Node::eInstanceOwner ) )
			owners.push_back( &instance );
		if ( Instance_isInstancePreview( instance ) )
			previews.push_back( &instance );
		return true;
	}
};

class CountRenderer : public Renderer {
public:
	std::size_t count = 0;
	EStyle style;
	CountRenderer( EStyle value ) : style( value ){}
	void PushState() override {}
	void PopState() override {}
	void SetState( Shader*, EStyle ) override {}
	EStyle getStyle() const override { return style; }
	void Highlight( EHighlightMode, bool = true ) override {}
	void addRenderable( const OpenGLRenderable&, const Matrix4& ) override { ++count; }
};

inline std::string read( const std::filesystem::path& path ){
	std::ifstream file( path, std::ios::binary );
	return std::string( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() );
}

inline bool check(){
	const std::filesystem::path master( Map_Name( g_map ) );
	const auto source = master.parent_path() / "room.map";
	std::ofstream report( master.parent_path() / "editor-results.txt" );
	auto require = [&]( bool condition, const char* message ){
		report << ( condition? "PASS: " : "FAIL: " ) << message << std::endl;
		return condition;
	};
	Gather loaded;
	GlobalSceneGraph().traverse( loaded );
	if ( !require( loaded.owners.size() == 2 && !loaded.previews.empty(), "linked geometry loaded for both instances" ) )
		return false;
	for ( scene::Instance* preview : loaded.previews )
		if ( !require( Instance_getSelectable( *preview ) == nullptr && Instance_getTransformable( *preview ) == nullptr &&
			Instance_getComponentEditable( *preview ) == nullptr && preview->path().top().get().m_layer == nullptr,
			"preview is protected and owns no source layer pointer" ) )
			return false;

	View view;
	Matrix4 projection = g_matrix4_identity;
	projection[0] = projection[5] = projection[10] = 1.0f / 8192;
	view.Construct( projection, g_matrix4_identity, 1024, 1024 );
	CountRenderer solid( Renderer::eFullMaterials ), wire( Renderer::eWireframeOnly );
	for ( scene::Instance* preview : loaded.previews )
		if ( Renderable* renderable = InstanceTypeCast<Renderable>::cast( *preview ) ) {
			renderable->viewChanged();
			renderable->renderSolid( solid, view );
			renderable->viewChanged();
			renderable->renderWireframe( wire, view );
		}
	report << "render submissions: solid=" << solid.count << " wire=" << wire.count << std::endl;
	if ( !require( solid.count > 0 && wire.count > 0, "source geometry submits solid and wireframe rendering" ) )
		return false;

	scene::Instance& owner = *loaded.owners.front();
	const AABB originalBounds = owner.worldAABB();
	Matrix4 modelview = g_matrix4_identity;
	modelview[12] = -originalBounds.origin.x();
	modelview[13] = -originalBounds.origin.y();
	view.Construct( projection, modelview, 1024, 1024 );
	SelectionVolume volume( view );
	SelectionPool pool;
	Scene_TestSelect_Primitive( pool, volume, view );
	if ( !require( !pool.failed() && pool.begin()->second == Instance_getSelectable( owner ), "clicking source geometry selects its owner" ) )
		return false;
	SelectionPool entityPool;
	Scene_TestSelect_Entity( entityPool, volume, view );
	if ( !require( !entityPool.failed() && entityPool.begin()->second == Instance_getSelectable( owner ), "entity selection mode selects the instance owner" ) )
		return false;

	const std::string originalSource = read( source );
	const auto saved = master.parent_path() / "editor_saved.map";
	InstancesPanel_toggleShown();
	auto* dock = MainFrame_getWindow()->findChild<QDockWidget*>( "MapInstancesPanel" );
	auto* list = MainFrame_getWindow()->findChild<QTreeWidget*>( "MapInstancesList" );
	if ( !require( dock != nullptr && dock->isVisible() && list != nullptr && list->topLevelItemCount() == 2 &&
		list->topLevelItem( 0 )->text( 3 ) == "Loaded" && !list->topLevelItem( 0 )->text( 4 ).isEmpty(),
		"dock panel lists placements, loaded status and modification dates" ) ) return false;
	dock->grab().save( QString::fromStdString( ( master.parent_path() / "instances-panel.png" ).string() ) );
	auto* rowRefresh = static_cast<QPushButton*>( list->itemWidget( list->topLevelItem( 0 ), 5 ) );
	const auto beforeButtonRefresh = GlobalUndoSystem().size();
	rowRefresh->click();
	if ( !require( Map_GetInstances()[0].status == "Loaded" && Map_GetInstances()[1].status == "Loaded" &&
		GlobalUndoSystem().size() == beforeButtonRefresh, "row refresh button updates source without an undo record" ) ) return false;
	list->topLevelItem( 0 )->setSelected( true );
	if ( !require( Instance_isSelected( owner ), "panel selection selects the placement" ) ) return false;
	const bool beforeHiddenModified = Map_Modified( g_map );
	const auto beforeHiddenUndo = GlobalUndoSystem().size();
	list->topLevelItem( 0 )->setCheckState( 0, Qt::Unchecked );
	if ( !require( !owner.path().top().get().visible() && Map_Modified( g_map ) == beforeHiddenModified &&
		GlobalUndoSystem().size() == beforeHiddenUndo, "visibility checkbox hides preview without modifying map or undo" ) ) return false;
	Map_SaveFile( saved.string().c_str() );
	const std::string hiddenSaved = read( saved );
	const auto firstRef = hiddenSaved.find( "func_instance" );
	if ( !require( firstRef != std::string::npos && hiddenSaved.find( "func_instance", firstRef + 1 ) != std::string::npos &&
		read( source ) == originalSource, "hidden instances still save as references and source is untouched" ) ) return false;
	list->topLevelItem( 0 )->setCheckState( 0, Qt::Checked );
	if ( !require( owner.path().top().get().visible(), "visibility checkbox restores preview" ) ) return false;
	if ( !require( Map_SaveFile( saved.string().c_str() ) && read( saved ).find( "common/caulk" ) == std::string::npos &&
		read( source ) == originalSource, "saving the master writes references and preserves the source" ) )
		return false;
	Transformable* transform = Instance_getTransformable( owner );
	if ( !require( transform != nullptr, "instance supports the normal move tool" ) )
		return false;
	const Vector3 shift( 128, 64, 0 );
	{
		UndoableCommand undo( "instanceTestMove" );
		transform->setType( TRANSFORM_PRIMITIVE );
		transform->setTranslation( shift );
		transform->freezeTransform();
	}
	if ( !require( vector3_length( owner.worldAABB().origin - originalBounds.origin - shift ) < 0.001f, "move transforms the whole preview" ) )
		return false;
	GlobalUndoSystem().undo();
	if ( !require( vector3_length( owner.worldAABB().origin - originalBounds.origin ) < 0.001f, "undo restores placement" ) )
		return false;
	GlobalUndoSystem().redo();
	const auto undoSize = GlobalUndoSystem().size();
	const bool modified = Map_Modified( g_map );
	Map_ReloadInstances();
	if ( !require( GlobalUndoSystem().size() == undoSize && Map_Modified( g_map ) == modified &&
		vector3_length( owner.worldAABB().origin - originalBounds.origin - shift ) < 0.001f,
		"reload preserves placement, unsaved edits and undo history" ) )
		return false;

	GlobalSelectionSystem().setSelectedAll( true );
	Gather selected;
	GlobalSceneGraph().traverse( selected );
	for ( scene::Instance* preview : selected.previews )
		if ( !require( !Instance_isSelected( *preview ), "select all excludes source contents" ) )
			return false;
	GlobalSelectionSystem().setSelectedAll( false );
	NodeSmartReference duplicate( Node_Clone( owner.path().top() ) );
	Node_getTraversable( GlobalSceneGraph().root() )->insert( duplicate );
	Gather duplicated;
	GlobalSceneGraph().traverse( duplicated );
	if ( !require( duplicated.owners.size() == 3 && duplicated.previews.size() == loaded.previews.size() / 2 * 3,
		"duplicating an instance preserves its protected preview" ) )
		return false;

	// Append a brush group to the disposable source, then refresh all three.
	const auto start = originalSource.find( '{', originalSource.find( "worldspawn" ) );
	std::size_t end = start, depth = 0;
	do {
		if ( originalSource[end] == '{' ) ++depth;
		if ( originalSource[end] == '}' ) --depth;
		++end;
	} while ( depth != 0 && end < originalSource.size() );
	{
		std::ofstream edit( source, std::ios::app );
		edit << "\n{\n\"classname\" \"func_group\"\n" << originalSource.substr( start, end - start ) << "\n}\n";
	}
	if ( !require( Map_GetInstances().front().status == "Changed", "source edits appear as Changed before refresh" ) ) return false;
	Map_ReloadInstanceSources( { owner.path().top().get_pointer() } );
	Gather refreshed;
	GlobalSceneGraph().traverse( refreshed );
	if ( !require( refreshed.previews.size() == duplicated.previews.size() + 6, "saved geometry changes refresh every placement" ) )
		return false;
	const std::string validSource = read( source );
	{
		std::ofstream edit( source, std::ios::app );
		edit << "\n{\n\"classname\" \"info_null\"\n";
	}
	Map_ReloadInstances();
	Gather malformed;
	GlobalSceneGraph().traverse( malformed );
	if ( !require( Map_GetInstances().front().status == "Failed" && !Map_GetInstances().front().error.empty(),
		"malformed sources expose a failed status and error" ) ) return false;
	{
		std::ofstream restore( source, std::ios::binary );
		restore << validSource;
	}
	if ( !require( malformed.previews.size() == refreshed.previews.size(), "malformed reload retains the last complete preview" ) )
		return false;
	std::error_code error;
	std::filesystem::rename( source, source.string() + ".held", error );
	Map_ReloadInstances();
	Gather missing;
	GlobalSceneGraph().traverse( missing );
	if ( !require( Map_GetInstances().front().status == "Missing", "missing source is identified in panel status" ) ) return false;
	std::filesystem::rename( source.string() + ".held", source, error );
	if ( !require( missing.previews.size() == refreshed.previews.size(), "failed reload retains the last valid preview" ) )
		return false;
	if ( !require( Map_SaveFile( saved.string().c_str() ), "save after move and duplicate" ) )
		return false;
	Map_Free();
	Map_LoadFile( saved.string().c_str() );
	Gather reopened;
	GlobalSceneGraph().traverse( reopened );
	if ( !require( reopened.owners.size() == 3 && reopened.previews.size() == refreshed.previews.size(), "saved references reopen with current source geometry" ) )
		return false;
	const auto secondSource = source.parent_path() / "section two.map";
	std::filesystem::copy_file( source, secondSource, std::filesystem::copy_options::overwrite_existing, error );
	if ( !require( !error, "prepare a second source with spaces in its filename" ) )
		return false;
	const auto batchUndoSize = GlobalUndoSystem().size();
	if ( !require( Map_AddInstances( { source.string(), secondSource.string() }, Vector3( 768, 0, 0 ) ) == 2,
		"add multiple existing map sources in one operation" ) )
		return false;
	Gather batch;
	GlobalSceneGraph().traverse( batch );
	if ( !require( batch.owners.size() == 5 && batch.previews.size() == reopened.previews.size() / 3 * 5 &&
		GlobalSelectionSystem().countSelected() == 2 && GlobalUndoSystem().size() == batchUndoSize + 1,
		"batch insertion loads and selects both instances with one undo record" ) )
		return false;
	GlobalUndoSystem().undo();
	Gather batchUndone;
	GlobalSceneGraph().traverse( batchUndone );
	if ( !require( batchUndone.owners.size() == 3 && batchUndone.previews.size() == reopened.previews.size(),
		"one undo removes the complete batch" ) )
		return false;
	GlobalUndoSystem().redo();
	Gather batchRedone;
	GlobalSceneGraph().traverse( batchRedone );
	if ( !require( batchRedone.owners.size() == 5 && batchRedone.previews.size() == batch.previews.size(),
		"one redo restores the complete batch" ) )
		return false;
	const auto beforeRejectedBatch = GlobalUndoSystem().size();
	if ( !require( Map_AddInstances( { source.string(), Map_Name( g_map ) }, Vector3( 0 ) ) == 0 &&
		GlobalUndoSystem().size() == beforeRejectedBatch, "invalid batch adds no partial references or undo record" ) )
		return false;
	Gather rejected;
	GlobalSceneGraph().traverse( rejected );
	if ( !require( rejected.owners.size() == 5, "invalid batch leaves existing references intact" ) ) return false;
	const auto beforeRemove = Map_GetInstances();
	const auto removeUndoSize = GlobalUndoSystem().size();
	const std::vector<scene::Node*> removeNodes = { beforeRemove[0].node, beforeRemove[1].node };
	Map_RemoveInstances( removeNodes );
	if ( !require( Map_GetInstances().size() == 3 && GlobalUndoSystem().size() == removeUndoSize + 1 &&
		read( source ) == validSource && read( secondSource ) == validSource,
		"bulk removal is one undo operation and preserves source files" ) ) return false;
	GlobalUndoSystem().undo();
	if ( !require( Map_GetInstances().size() == 5, "undo restores removed references" ) ) return false;
	GlobalUndoSystem().redo();
	if ( !require( Map_GetInstances().size() == 3, "redo removes references again" ) ) return false;
	Map_SetInstanceVisible( removeNodes[0], false );
	Map_ReloadInstanceSources( removeNodes );
	if ( !require( Map_GetInstances().size() == 3, "stale panel actions safely ignore removed placements" ) ) return false;
	GlobalUndoSystem().undo();
	Map_ReloadInstances();
	const auto beforeScopedRefresh = Map_GetInstances();
	{
		std::ofstream edit( source, std::ios::app ); edit << "\n// new saved revision\n";
		std::ofstream editOther( secondSource, std::ios::app ); editOther << "\n// another saved revision\n";
	}
	scene::Node* refreshTarget = nullptr;
	for ( const auto& info : beforeScopedRefresh ) if ( info.file.find( "section two" ) == std::string::npos ) refreshTarget = info.node;
	Map_ReloadInstanceSources( { refreshTarget } );
	for ( const auto& info : Map_GetInstances() )
		if ( !require( info.status == ( info.file.find( "section two" ) == std::string::npos? "Loaded" : "Changed" ),
			"refresh updates all matching placements and leaves other sources pending" ) ) return false;
	Map_Free();
	if ( !require( Map_GetInstances().empty(), "closing master clears instance list" ) ) return false;
	Map_LoadFile( saved.string().c_str() );
	QApplication::processEvents();
	report << "panel switch rows=" << list->topLevelItemCount() << " instances=" << Map_GetInstances().size() << std::endl;
	return require( Map_GetInstances().size() == 3 && list->topLevelItemCount() == 3,
		"panel follows map switches without stale rows" );
}
} // namespace instance_editor_test
