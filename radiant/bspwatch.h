/* Watches a compiled BSP, including compilers that replace it by rename. */
#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFileSystemWatcher>

class BspFileWatch
{
	const QString m_path;
	QFileSystemWatcher m_watcher;
	QElapsedTimer m_quiet;
	QDateTime m_modified;
	qint64 m_size = -1;
	quint64 m_revision = 0;
	int m_retries = 0;
	bool m_pending = true;
	const int m_settleMilliseconds;

	void changed(){
		++m_revision;
		m_retries = 0;
		m_pending = true;
		m_quiet.restart();
	}

	void observe(){
		const QFileInfo info( m_path );
		const qint64 size = info.isFile() ? info.size() : -1;
		if ( size != m_size || info.lastModified() != m_modified ) {
			m_size = size;
			m_modified = info.lastModified();
			changed();
		}
		// A replacement removes Qt's file watch. The directory watch notices
		// its return; polling also recovers if the directory itself disappears.
		if ( !m_watcher.directories().contains( info.absolutePath() ) )
			m_watcher.addPath( info.absolutePath() );
		if ( size >= 0 && !m_watcher.files().contains( m_path ) ) {
			if ( m_watcher.addPath( m_path ) ) changed();
		}
	}

public:
	explicit BspFileWatch( const QString& path, int settleMilliseconds = 750 ) :
		m_path( QFileInfo( path ).absoluteFilePath() ), m_settleMilliseconds( settleMilliseconds ){
		m_quiet.start();
		QObject::connect( &m_watcher, &QFileSystemWatcher::fileChanged, &m_watcher, [this](){
			changed(); // also catches writes that preserve size and timestamp
			observe();
		} );
		QObject::connect( &m_watcher, &QFileSystemWatcher::directoryChanged, &m_watcher, [this](){ observe(); } );
		observe();
	}

	/// Consume a settled update. The caller serializes conversions.
	bool takeUpdate(){
		observe();
		if ( !m_pending || m_size <= 0 || m_quiet.elapsed() < m_settleMilliseconds )
			return false;
		m_pending = false;
		return true;
	}

	quint64 revision() const { return m_revision; }

	/// A conversion is publishable only while its source revision still exists.
	bool isCurrent( quint64 revision ){
		observe();
		return m_size > 0 && revision == m_revision;
	}

	/// Retry transient sharing/read failures, without looping on a broken BSP.
	void retry(){
		if ( ++m_retries <= 3 ) {
			m_pending = true;
			m_quiet.restart();
		}
	}
};
