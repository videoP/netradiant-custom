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

#include "parse.h"

#include <list>
#include <chrono>

#include "ientity.h"
#include "ibrush.h"
#include "ipatch.h"
#include "ieclass.h"
#include "iscriplib.h"
#include "scenelib.h"
#include "string/string.h"
#include "stringio.h"
#include "eclasslib.h"
#include "layers.h"

class LayersParser
{
	Layers& m_layers;
	std::vector<Layer::iterator> m_layersVec; // vector to access layers by index
	int m_currentLayer = LAYERIDX0;
public:
	LayersParser( scene::Node& root ) : m_layers( *Node_getLayers( root ) ){
	}
	void construct_tree(){
		{
			auto& list = m_layers.m_children;

			if( list.size() > 1 ) // something was parsed, remove default inserted layer
				list.pop_front();

			m_layersVec.reserve( list.size() );
			for( auto it = list.begin(); it != list.end(); ++it )
				m_layersVec.push_back( it );
			// check safety // note possible circular reference is not checked
			if( std::ranges::find( list, LAYERIDXPARENT, &Layer::m_parentIndex ) == list.cend() ){
				globalErrorStream() << "No layers in the root\n";
				list.front().m_parentIndex = LAYERIDXPARENT;
			}
		}
		for( auto it : m_layersVec ){
			// check safety
			if( it->m_parentIndex < LAYERIDXPARENT || it->m_parentIndex >= static_cast<int>( m_layersVec.size() ) ){
				globalErrorStream() << it->m_parentIndex << " parent layer index out of bounds\n";
				it->m_parentIndex = LAYERIDXPARENT;
			}
			if( it->m_parentIndex > LAYERIDXPARENT && m_layersVec[ it->m_parentIndex ] == it ){
				globalErrorStream() << it->m_parentIndex << " parent layer index self reference\n";
				it->m_parentIndex = LAYERIDXPARENT;
			}
			// link & reference parent
			Layer& parent = it->m_parentIndex == LAYERIDXPARENT? m_layers : *m_layersVec[ it->m_parentIndex ];
			parent.m_children.splice( parent.m_children.cend(), m_layers.m_children, it );
			it->m_parent = &parent;
		}

		m_layers.m_currentLayer = &m_layers.m_children.front();
	}
	bool read_layers( Tokeniser& tokeniser ){
		for (;; )
		{
			tokeniser.nextLine();
			const char *token;
			if( !( token = tokeniser.getToken() ) )
				return false;
			if( !string_equal( token, "layerdef" ) ){
				tokeniser.ungetToken();
				return true;
			}
			if( !( token = tokeniser.getToken() ) ){
				Tokeniser_unexpectedError( tokeniser, token, "#layer-name" );
				return false;
			}
			auto& layer = m_layers.m_children.emplace_back( token, nullptr ); // initially construct plain list

			if( !Tokeniser_getInteger( tokeniser, layer.m_parentIndex )
			|| !Tokeniser_getInteger( tokeniser, layer.m_color[ 0 ] )
			|| !Tokeniser_getInteger( tokeniser, layer.m_color[ 1 ] )
			|| !Tokeniser_getInteger( tokeniser, layer.m_color[ 2 ] ) )
				return false;

			Tokeniser_skipToNextLine( tokeniser ); // skip possible format extension
		}
	}
	bool read_layer( Tokeniser& tokeniser ){
		const char *token = tokeniser.getToken();
		if ( token == nullptr )
			return false;
		if( !string_equal( token, "layer" ) ){
			tokeniser.ungetToken();
			return true;
		}
		if( !Tokeniser_getInteger( tokeniser, m_currentLayer ) ){
			return false;
		}
		else if( m_currentLayer <= LAYERIDXPARENT || m_currentLayer >= static_cast<int>( m_layersVec.size() ) ){
			globalErrorStream() << m_currentLayer << " primitive layer index out of bounds\n";
			m_currentLayer = LAYERIDX0;
		}
		Tokeniser_skipToNextLine( tokeniser ); // skip possible format extension
		return true;
	}
	Layer* getCurrentLayer(){
		return m_layersVec[ m_currentLayer ].operator->();
	}
};

inline MapImporter* Node_getMapImporter( scene::Node& node ){
	return NodeTypeCast<MapImporter>::cast( node );
}


/*! \brief Where the time inside a map parse goes.

    Parsing is about 80% of opening a map, and it is three quite different
    jobs: making the primitive's objects, reading its text and building its
    faces from it, and filing it under its entity. Each wants a different fix,
    so measure before choosing one.

    Two clock reads per primitive, against hundreds of thousands of primitives:
    tens of milliseconds on a multi-second parse, small enough not to distort
    what it is measuring.
 */
struct ParseStats
{
	class Accumulator
	{
		std::chrono::steady_clock::time_point m_begin;
		std::chrono::steady_clock::duration m_total{};
	public:
		void start(){
			m_begin = std::chrono::steady_clock::now();
		}
		void stop(){
			m_total += std::chrono::steady_clock::now() - m_begin;
		}
		double seconds() const {
			return std::chrono::duration<double>( m_total ).count();
		}
		void reset(){
			m_total = {};
		}
	};

	Accumulator m_create;   // constructing the primitive's node
	Accumulator m_read;     // tokenising its text and building its faces
	Accumulator m_insert;   // filing it under its entity
	std::size_t m_primitives = 0;

	void reset(){
		m_create.reset();
		m_read.reset();
		m_insert.reset();
		m_primitives = 0;
	}
	void report() const {
		globalOutputStream() << "  parse detail: node create " << FloatFormat( m_create.seconds(), 5, 2 )
		                     << "s, read+faces " << FloatFormat( m_read.seconds(), 5, 2 )
		                     << "s, entity insert " << FloatFormat( m_insert.seconds(), 5, 2 )
		                     << "s, over " << Unsigned( m_primitives ) << " primitives\n";
	}
};

ParseStats g_parseStats;


typedef std::list< std::pair<CopiedString, CopiedString> > KeyValues;

NodeSmartReference g_nullNode( NewNullNode() );


NodeSmartReference Entity_create( EntityCreator& entityTable, EntityClass* entityClass, const KeyValues& keyValues ){
	scene::Node& entity( entityTable.createEntity( entityClass ) );
	for ( const auto& [ key, value ] : keyValues )
	{
		Node_getEntity( entity )->setKeyValue( key.c_str(), value.c_str() );
	}
	return NodeSmartReference( entity );
}

NodeSmartReference Entity_parseTokens( Tokeniser& tokeniser, EntityCreator& entityTable, const PrimitiveParser& parser, int index, LayersParser& layersParser ){
	NodeSmartReference entity( g_nullNode );
	KeyValues keyValues;
	const char* classname = "";

	int count_primitives = 0;
	while ( true )
	{
		tokeniser.nextLine();
		const char* token;
		if ( !layersParser.read_layer( tokeniser ) || !( token = tokeniser.getToken() ) ) {
			Tokeniser_unexpectedError( tokeniser, token, "#entity-token" );
			return g_nullNode;
		}
		if ( !strcmp( token, "}" ) ) { // end entity
			if ( entity == g_nullNode ) {
				// entity does not have brushes
				entity = Entity_create( entityTable, GlobalEntityClassManager().findOrInsert( classname, false ), keyValues );
			}
			return entity;
		}
		else if ( !strcmp( token, "{" ) ) { // begin primitive
			if ( entity == g_nullNode ) {
				// entity has brushes
				entity = Entity_create( entityTable, GlobalEntityClassManager().findOrInsert( classname, true ), keyValues );
			}

			tokeniser.nextLine();

			g_parseStats.m_create.start();
			NodeSmartReference primitive( parser.parsePrimitive( tokeniser ) );
			g_parseStats.m_create.stop();

			bool parsed = ( primitive != g_nullNode );
			if ( parsed ) {
				g_parseStats.m_read.start();
				parsed = Node_getMapImporter( primitive )->importTokens( tokeniser );
				g_parseStats.m_read.stop();
			}
			if ( !parsed ) {
				globalErrorStream() << "brush " << count_primitives << ": parse error\n";
				return g_nullNode;
			}
			primitive.get().m_layer = layersParser.getCurrentLayer();

			scene::Traversable* traversable = Node_getTraversable( entity );
			if ( Node_getEntity( entity )->isContainer() && traversable != 0 ) {
				g_parseStats.m_insert.start();
				traversable->insert( primitive );
				g_parseStats.m_insert.stop();
			}
			else
			{
				globalErrorStream() << "entity " << index << ": type " << classname << ": discarding brush " << count_primitives << '\n';
			}
			++count_primitives;
			++g_parseStats.m_primitives;
		}
		else // epair
		{
			CopiedString key( token );
			token = tokeniser.getToken();
			if ( token == 0 ) {
				Tokeniser_unexpectedError( tokeniser, token, "#epair-value" );
				return g_nullNode;
			}
			keyValues.push_back( KeyValues::value_type( key, token ) );
			if ( string_equal( key.c_str(), "classname" ) ) {
				classname = keyValues.back().second.c_str();
			}
		}
	}
	// unreachable code
	return g_nullNode;
}

void Map_Read( scene::Node& root, Tokeniser& tokeniser, EntityCreator& entityTable, const PrimitiveParser& parser ){
	g_parseStats.reset();

	LayersParser layersParser( root );
	if( !layersParser.read_layers( tokeniser ) ){
		layersParser.construct_tree(); // construct anytime to have at least one layer, e.g. when empty .map
		return;
	}
	layersParser.construct_tree();

	int count_entities = 0;
	for (;; )
	{
		tokeniser.nextLine();
		if ( !layersParser.read_layer( tokeniser ) || !tokeniser.getToken() ) { // { or 0
			break;
		}
		Layer *entityLayer = layersParser.getCurrentLayer();

		NodeSmartReference entity( Entity_parseTokens( tokeniser, entityTable, parser, count_entities, layersParser ) );

		if ( entity == g_nullNode ) {
			globalErrorStream() << "entity " << count_entities << ": parse error\n";
			return;
		}

		Node_getTraversable( root )->insert( entity );
		if( !Node_getEntity( entity )->isContainer() )
			entity.get().m_layer = entityLayer;

		++count_entities;
	}

	g_parseStats.report();
}
