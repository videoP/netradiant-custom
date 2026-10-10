#include "instancespanel.h"
#include "map.h"
#include "signal/isignal.h"

#include <QDockWidget>
#include <QMainWindow>
#include <QTreeWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QDateTime>
#include <QSignalBlocker>

namespace {
class InstancesPanel : public QDockWidget {
	QTreeWidget* m_tree;
	QLabel* m_summary;
	QPushButton *m_add, *m_open, *m_focus, *m_refresh, *m_remove, *m_refreshAll;
	bool m_syncing = false;
	bool m_pending = false;

	static scene::Node* node( QTreeWidgetItem* item ){
		return static_cast<scene::Node*>( item->data( 0, Qt::UserRole ).value<void*>() );
	}
	std::vector<scene::Node*> selected() const {
		std::vector<scene::Node*> nodes;
		for ( auto* item : m_tree->selectedItems() ) nodes.push_back( node( item ) );
		return nodes;
	}
	void actions(){
		const int count = m_tree->selectedItems().size();
		m_add->setEnabled( Map_Valid( g_map ) && !Map_Unnamed( g_map ) );
		m_open->setEnabled( count == 1 );
		m_focus->setEnabled( count != 0 );
		m_refresh->setEnabled( count != 0 );
		m_remove->setEnabled( count != 0 );
		m_refreshAll->setEnabled( m_tree->topLevelItemCount() != 0 );
	}
	void later(){
		requestSync();
	}
public:
	QSize sizeHint() const override { return QSize( 760, 220 ); }
	void requestSync(){
		if ( m_pending || !isVisible() ) return;
		m_pending = true;
		QTimer::singleShot( 0, this, [this]{ m_pending = false; sync(); } );
	}
	explicit InstancesPanel( QMainWindow* parent ) : QDockWidget( "Map Instances", parent ){
		setObjectName( "MapInstancesPanel" );
		auto* body = new QWidget( this );
		auto* layout = new QVBoxLayout( body );
		layout->setContentsMargins( 6, 6, 6, 6 );
		auto* top = new QHBoxLayout;
		const auto button = [&]( const char* text, QHBoxLayout* row ) {
			auto* result = new QPushButton( text, body );
			row->addWidget( result );
			return result;
		};
		m_add = button( "Add…", top );
		m_open = button( "Open Source", top );
		m_focus = button( "Focus", top );
		layout->addLayout( top );
		m_tree = new QTreeWidget( body );
		m_tree->setObjectName( "MapInstancesList" );
		m_tree->setHeaderLabels( { "Visible", "Source map", "Placement / origin", "Status", "Modified", "" } );
		m_tree->setRootIsDecorated( false );
		m_tree->setSelectionMode( QAbstractItemView::ExtendedSelection );
		m_tree->setUniformRowHeights( true );
		m_tree->header()->setSectionResizeMode( QHeaderView::ResizeToContents );
		m_tree->header()->setSectionResizeMode( 1, QHeaderView::Stretch );
		m_tree->setMinimumWidth( 340 );
		m_tree->setMinimumHeight( 90 );
		layout->addWidget( m_tree );
		auto* bottom = new QHBoxLayout;
		m_refresh = button( "Refresh Selected", bottom );
		m_refreshAll = button( "Refresh All", bottom );
		m_remove = button( "Remove", bottom );
		m_remove->setToolTip( "Remove selected references from the master (undoable). Source files are preserved." );
		m_refresh->setToolTip( "Refresh every placement using the selected source files." );
		layout->addLayout( bottom );
		m_summary = new QLabel( body );
		layout->addWidget( m_summary );
		setWidget( body );
		connect( m_add, &QPushButton::clicked, this, [this]{ CreateMapInstance(); later(); } );
		connect( m_open, &QPushButton::clicked, this, [this]{
			const auto nodes = selected();
			if ( nodes.size() == 1 ) { Map_SelectInstances( nodes ); OpenMapInstanceSource(); }
			later();
		} );
		connect( m_focus, &QPushButton::clicked, this, [this]{ Map_SelectInstances( selected(), true ); later(); } );
		connect( m_refresh, &QPushButton::clicked, this, [this]{ Map_ReloadInstanceSources( selected() ); later(); } );
		connect( m_refreshAll, &QPushButton::clicked, this, [this]{ Map_ReloadInstances(); later(); } );
		connect( m_remove, &QPushButton::clicked, this, [this]{ Map_RemoveInstances( selected() ); later(); } );
		connect( m_tree, &QTreeWidget::itemSelectionChanged, this, [this]{
			if ( m_syncing ) return;
			Map_SelectInstances( selected() );
			actions();
		} );
		connect( m_tree, &QTreeWidget::itemDoubleClicked, this, [this]( QTreeWidgetItem*, int column ){
			if ( column != 0 ) Map_SelectInstances( selected(), true );
		} );
		connect( m_tree, &QTreeWidget::itemChanged, this, [this]( QTreeWidgetItem* item, int column ){
			if ( !m_syncing && column == 0 ) {
				Map_SetInstanceVisible( node( item ), item->checkState( 0 ) == Qt::Checked );
				later();
			}
		} );
		auto* timer = new QTimer( this );
		timer->setInterval( 750 );
		connect( timer, &QTimer::timeout, this, [this]{ if ( isVisible() ) sync(); } );
		timer->start();
		connect( this, &QDockWidget::visibilityChanged, this, [this]( bool visible ){ if ( visible ) sync(); } );
	}

	void sync(){
		m_syncing = true;
		const QSignalBlocker blocker( m_tree );
		const auto instances = Map_GetInstances();
		bool rebuild = m_tree->topLevelItemCount() != static_cast<int>( instances.size() );
		if ( !rebuild )
			for ( std::size_t i = 0; i < instances.size(); ++i )
				if ( node( m_tree->topLevelItem( i ) ) != instances[i].node ) { rebuild = true; break; }
		if ( rebuild ) m_tree->clear();
		for ( std::size_t i = 0; i < instances.size(); ++i ) {
			const auto& info = instances[i];
			auto* item = rebuild? new QTreeWidgetItem( m_tree ) : m_tree->topLevelItem( i );
			if ( rebuild ) {
				item->setData( 0, Qt::UserRole, QVariant::fromValue( static_cast<void*>( info.node ) ) );
				item->setFlags( item->flags() | Qt::ItemIsUserCheckable );
				auto* refresh = new QPushButton( "Refresh", m_tree );
				refresh->setToolTip( "Refresh all placements of this source" );
				connect( refresh, &QPushButton::clicked, this, [this, target = info.node]{
					Map_ReloadInstanceSources( { target } ); later();
				} );
				m_tree->setItemWidget( item, 5, refresh );
			}
			item->setCheckState( 0, info.visible? Qt::Checked : Qt::Unchecked );
			item->setToolTip( 0, "Editor visibility only. Hidden instances remain included in compilation." );
			item->setText( 1, QString::fromLatin1( info.file.c_str() ) );
			item->setToolTip( 1, QString::fromLatin1( info.source.c_str() ) );
			item->setText( 2, QString::fromLatin1( info.placement.c_str() ) );
			item->setText( 3, QString::fromLatin1( info.status.c_str() ) );
			item->setToolTip( 3, QString::fromLatin1( info.error.c_str() ) );
			item->setText( 4, info.modified? QDateTime::fromMSecsSinceEpoch( info.modified ).toString( "yyyy-MM-dd HH:mm:ss" ) : QString( "—" ) );
			item->setSelected( info.selected );
		}
		m_summary->setText( Map_Unnamed( g_map )? "Save the master map to add instances." :
			QString( "%1 placement(s). Visibility affects the editor only." ).arg( instances.size() ) );
		actions();
		m_syncing = false;
	}
};
InstancesPanel* g_panel = nullptr;
void sceneChanged(){
	if ( g_panel != nullptr ) g_panel->requestSync();
}
}

void InstancesPanel_constructWindow( QMainWindow* parent ){
	g_panel = new InstancesPanel( parent );
	// The callback has application lifetime; it never retains a destroyed panel.
	static bool connected = false;
	if ( !connected ) {
		GlobalSceneGraph().addSceneChangedCallback( FreeCaller<void(), sceneChanged>() );
		connected = true;
	}
	parent->addDockWidget( Qt::BottomDockWidgetArea, g_panel );
	g_panel->hide();
}

void InstancesPanel_destroyWindow(){
	delete g_panel;
	g_panel = nullptr;
}

void InstancesPanel_toggleShown(){
	if ( g_panel == nullptr ) return;
	g_panel->setVisible( !g_panel->isVisible() );
	if ( g_panel->isVisible() ) g_panel->raise();
}
