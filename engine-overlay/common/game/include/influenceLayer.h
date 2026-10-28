/*
* Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/

#pragma once
#include "influenceGrid.h"
#include "../../worldNavigation/include/influenceMapCell.h"
#include "../../worldNavigation/include/influenceMapHelpers.h"
#include "../../redContainers/include/queue.h"

namespace game
{
	namespace influence
	{
		enum class DrawingRule : Uint8
		{
			Equal,
			Greater,
			Less
		};

		template<typename T>
		class DefaultComparator
		{

		public:
			DefaultComparator() = default;
			~DefaultComparator() = default;
			DefaultComparator( const T& v ) : defaultValue( v )	{}
		
			DefaultComparator& operator=( const DefaultComparator& v ) { defaultValue = v.defaultValue; return *this; }
			Bool operator()( const T& cmp ) const { return defaultValue == cmp; }

			void GetDefaultValue() const { return defaultValue; }
		private:
			const T defaultValue = {};

		};

		// IMPORTANT!!!
		// The only functions that can modify cells in the LayerStorage data should be 
		// Layer( const Layer& other );
		// void SetValue( const CellID& cell, const T& val )
		// Bool TryToAddCell( CellID id, const T& value )
		// void RemoveCells( const GridMask& mask )
		// void ModifyValue( const GridMask& cells, F&& modifyCall )
		// void ModifyValues( F&& call, Uint32 offset, Uint32 limit )
		// void DecreaseValue( const T& limit, const Q& value )
		//
		// Manipulating the storage directly might corrupt the cache state!!!

		template< typename T, typename DefaultValue = DefaultComparator<T> >
		class Layer : public ILayer
		{

		public:

			using value_type = T;
			using IsBetter = red::FixedSizeFunction< Bool( const CellID& best, T bestValue, const CellID& tested, T testedValue ) >;

			Layer( Grid& grid );
			Layer( Grid& grid, const DefaultValue& customDefault );

			Layer( const Layer& other );
			virtual ~Layer();

			struct Mask
			{
				struct LayerCell
				{
					LayerCell( CellID id, const T& v );

					Bool operator==( const LayerCell& cell ) const { return id == cell.id; }
					Bool operator==( const CellID& otherId ) const { return id == otherId; }

					CellID id;
					T value;
				};

				Mask GetDifferenceMask( const GridMask& second, const red::memory::Pool& pool ) const;
				void GetDifferenceMask( const GridMask& second, GridMask& outMask ) const;

				void DrawDebugMask( rend::IDebugDrawer& dd, const Grid& grid, const Color& color ) const;
				void PrintDebugMaskFloat( rend::IDebugDrawer& dd, const Grid& grid, const ILayer* layerPtr ) const;
				void PrintDebugMaskIndexes( rend::IDebugDrawer& dd, const Grid& grid, const ILayer* layerPtr ) const;

				red::DynArray< LayerCell > m_cells = { AI::PoolAI_InfluenceMap() };;
			};

			virtual void CacheUpdate( Float dTime ) override;

			virtual Uint32 GetSize() const override;

			const Grid& GetGrid() const { return m_grid; }

			void SetValue( const CellID& cell, const T& val );
			void SetValue( const GridMask& cells, const T& val );
			void SetValue( const Mask& cells, const T& val );
			void SetValue( const Mask& cells );
			void SetValueNoNavmeshCheck( const CellID& id, const T& val );

			// Does the following for each cell in the mask:
			// 1. If cell is not added to the layer yet, tries to add it. If that fails (e. g. not on navmesh), cell is skipped.
			// 2. If cell is added to the layer, it's value is compared to "comparedValue". If same, it is assigned a value of the previous assigned cell + step.
			// lowerValue is the lowest value assigned to a cell this way.
			// upperValue is the highest value.
			// If previous cell already has the highest value, all the subsequent cells are assigned the highest value.
			void SetValueAscending( const GridMask& cells, const T& lowerVal, const T& upperValue, const T& step, const T& comparedValue );

			void SetValueLerp( const GridMask& cells, const  CellID& coords, Float radius, const T& minVal, const T& maxValue );
			void SetValueLerpFloodFill( const Vector3& position, Float radius, const T& minVal, const T& maxVal, Mask& outView );
			void SetValueInNeighbourhood( const Vector3& position, Float radius, const T& val );
			void AddValueNoNavmeshCheck( const GridMask& cells, const T& val );
			void AddValueNoNavmeshCheck( const CellID& cell, const T& val );
			void AddValue( const GridMask& cells, const T& val );
			void AddValue( const CellID& cell, const T& val );
			void SubstractValueNoNavmeshCheck( const GridMask& cells, const T& val );
			void SubstractValueNoNavmeshCheck( const CellID& cell, const T& val );
			void SubstractValue( const GridMask& cells, const T& val );
			void SubstractValue( const CellID& cell, const T& val );
			void SubstractValue( const Mask& mask );

			template<typename F>
			void ModifyValue( const GridMask& cells, F&& modifyCall );

			template<typename F>
			void VisitValues( const GridMask& cells, F&& visitCall )const;

			template<typename F>
			void VisitValues( const GridMask& cells, F&& visitCall, Uint32 offset, Uint32 limit )const;

			template<typename F>
			void VisitValues( F&& visitCall )const;

			template<typename F>
			void VisitValues( F&& visitCall, Uint32 offset, Uint32 limit )const;

			template<typename F>
			void ModifyValues( F&& modifyCall, Uint32 offset, Uint32 limit );

			template<typename F>
			void ModifyValues( F&& modifyCall );

			template<typename F>
			Bool FindValue( F&& predicate )const;

			// If one cell has different value, return false.
			Bool CompareValue( const GridMask& cells, const T& val );

			// If one cell has different value, then add.
			void CompareValue( const GridMask& cells, const T& val, GridMask& outCells );

			template<typename Q>
			void DecreaseValue( const T& limit, const Q& value );

			// Returns true if the value is stored in the layer.
			// If the value is not stored, outValue is not modified.
			Bool GetValue( const CellID& coords, T& outValue ) const;

			// Returns true if the value is stored in the layer.
			// If the value is not stored, outValue is not modified.
			Bool GetValue( const red::DynArray< CachedCell >& cachedCells, const CellID& coords, T& outValue ) const;

			template<typename F>
			Bool VisitValue( const CellID& coords, F&& outValue ) const;

			//Returns true if has the cell with the value
			//Returns true if cell doesn't exist and initializes with value
			//Returns false if has different value
			Bool CheckOrAdd( const CellID& id, const T& value );

			Bool GetOrCreateCell( const CellID& id, T& initValue );

			Bool TryToAddCell( CellID id, const T& value );
			Bool FindBestPosition( const Vector3& position, Float radius, const T& val, Vector3& outBestPosition );
			Bool FindCell( const IsBetter& check, CellID& outBestCell ) const;
			Bool FindCell( const  CellID& coords, TCoord levelRange, Float desirableDistance, const T& val, CellID& outBestCell, TCoord maxSearchDistance = 10 );
			Bool FindCellHighestValue( const  CellID& coords, TCoord levelRange, Float desirableDistance, Float maxDistance, const T& limit, CellID& outBestCell ) const;
            Bool FindCellHighestValue( const  CellID& coords, TCoord levelRange, Float desirableDistance, Float maxDistance, const T& limit, const  CellID& minBBCoord, const  CellID& maxBBCoord, CellID& outBestCell ) const;
			Bool FindNeighbours( const  CellID& coords, const T& val, GridMask& outCells );
			Bool FindNeighbours( const  CellID& coords, GridMask& outCells );

			Bool CheckValueHorizontal( const TCoord& y, const TCoord& level, const TCoord& startX, const TCoord& endX, const T& val, CellID& outBestCell );
			Bool CheckValueVertical( const TCoord& x, const TCoord& level, const TCoord& startY, const TCoord& endY, const T& val, CellID& outBestCell );

			//check if there's a value other then default in the tested line
			Bool TestLine( const Vector3& start, const Vector3& end ) const;

			//Astar
			using Connectivity = red::FixedSizeFunction< Bool( const Layer< T >& layer, const CellID& previous, const CellID& current, T value ) >;
			using Scoring = red::FixedSizeFunction< Float( const CellID& current ) >; //Smaller number is better
			using Success = red::FixedSizeFunction< Bool( const CellID& current ) >;
			using DestinationFunc = red::FixedSizeFunction< Vector3( const GridMask& mask ) >;

			struct Node
			{
				Node() = default;

				Node( Float distanceToEnd, Float distanceToNode, Float distanceToPrevNode, CellID cellId, CellID previousCell )
					: m_distanceToEnd( distanceToEnd )
					, m_distanceToNode( distanceToNode )
					, m_distanceToPrevNode( distanceToPrevNode )
					, m_cellId( cellId )
					, m_previousCell( previousCell )
					, m_position( ToWorld( m_cellId ) )
				{
				};

				Float m_distanceToEnd;
				Float m_distanceToNode;
				Float m_distanceToPrevNode;
				CellID m_cellId;
				CellID m_previousCell;
				Vector3 m_position;

				Bool operator ==( const Node& other ) const { return other.m_cellId == m_cellId; };
			};

			struct DefaultListPredicate
			{
				Bool operator()( const Node& ln, const Node& rn ) const { return ln.m_distanceToEnd + static_cast< Float >( ln.m_distanceToNode ) < rn.m_distanceToEnd + static_cast< Float >( rn.m_distanceToNode ); };
			};

			Bool AStar( const Vector3& startPosition
						, const DestinationFunc& destination
						, const Scoring& scoring
						, const Success& success
						, red::DynArray< Vector3 >& outPath
						, const Connectivity& connectivity
						, Uint32 processedNodesLimit = 1000u );

			Bool AStar( const Vector3& startPosition
						, const CellID& end
						, GridMask& outPath
						, const Connectivity& connectivity
						, Float maxPathLength = 100.f
						, Uint32 processedNodesLimit = 1000u );

			template< typename OpenListPredicate = DefaultListPredicate >
			Bool AStar( const Vector3& startPosition
						, GridMask& outPath
						, const Scoring& scoring
						, const Success& success
						, const Connectivity& connectivity
						, Float maxDistanceFactor
						, Float maxPathLength = 100.f
						, Bool onlyOneNode = false
						, Uint32 processedNodesLimit = 1000u );

			Vector3 TraverseTowardsLowGradient( const Vector3& position, T limit, Float maxDistance = 100.f );
			Vector3 FindMinimumValue( const Vector3& position, Float minDistance = 5.f, Float maxDistance = 100.f ) const;

			void DrawLayer( rend::IDebugDrawer& dd, const Color& color, const T& value, DrawingRule rule = DrawingRule::Equal ) const;
			void DrawLayerLerp( rend::IDebugDrawer& dd, const Color& startColor, const Color& endColor, const T& minValue, const T& maxValue ) const;

			template<typename COLOR_PICKER, typename H_PICKER >
			void DrawLayerWithHeight( rend::IDebugDrawer& dd, COLOR_PICKER&& colorPicker, H_PICKER&& hPicker ) const;

#if RED_CTSW(RED_RENDER_DEBUG)
			template<typename F>
			void DrawDebugValues( const GridMask& grid, const F& drawer ) const;
			void DrawLastAStarNodes( rend::IDebugDrawer& dd, const red::DynArray< Node >& nodes );
			void PrintDebugMaskFloat( rend::IDebugDrawer& dd ) const;

			red::DynArray< Node > GetLastAstarNodes() const;

			template<typename Q> Float DebugOutput( Q&& foo ) const { return -1.f; }
			Float DebugOutput( Float output ) const { return output; }
#endif
#if RED_CTSW(RED_DEBUG_DRAW_AI_IMGUI)
			void SetDrawLayerDebug( Bool draw ) { m_drawLayer = draw; }
			Bool GetDrawLayerDebug() const { return m_drawLayer; }
#endif
			void BumpDirtyCount( Uint32 count = 1u )
			{
				if ( m_dirtyCount == std::numeric_limits<decltype( m_dirtyCount )>::infinity() ) return;

				m_dirtyCount += count;
			}
		private:
			struct FloodFillNode
			{
				CellID id;
				CellID previousId;

				Float distance;

				T cachedValue;

				Bool operator ==( const FloodFillNode& other ) const { return id == other.id; };
			};
			void AddFloodFillNeigbour( const red::DynArray< CachedCell >& cachedCells, const CellID& id, const FloodFillNode&  previous, Float distance, Float radius, red::Queue< FloodFillNode >& nodesToProcess );

			void FloodFill( const red::DynArray< CachedCell >& cachedCells, const CellID& id, Float radius, red::DynArray < FloodFillNode > & outNodes );

			// todo[andrzej.drabarek]: This should probably be hash map + array (+ freelist)
			typedef red::HashMap< CellID, T > LayerStorage;

			LayerStorage m_layer = { AI::PoolAI_InfluenceMap() };

			Uint32 m_dirtyCount = std::numeric_limits<decltype( m_dirtyCount )>::infinity(); //this can be used while decaching to indicate if a pass is necessary

			mutable red::RWSpinLock m_lock;

#if RED_CTSW(RED_RENDER_DEBUG)
			red::DynArray< Node > m_lastAStarListDebug = { red::Pool_DebugContexts() };
			red::DynArray< Vector3 > m_lastAStarPathDebug = { red::Pool_DebugContexts() };
			GridMask m_lastAstarCellsDebug = { red::Pool_DebugContexts() };

			mutable red::RWSpinLock m_debugLock;
#endif
#if RED_CTSW(RED_DEBUG_DRAW_AI_IMGUI)
			mutable Bool m_drawLayer = false;
#endif

		protected:
			DefaultValue m_defaultValue;
		};
}
} // namespace game    

namespace game
{
	namespace influence
	{
		namespace Helpers
		{
			static void Sort( red::DynArray< CellID > & cells, Bool makeUnique = true, Bool makeFit = true )
			{
				std::sort( cells.Begin(), cells.End(), []( auto& a, auto& b )
				{
					if( a.x < b.x )
					{
						return true;
					}
					else if( a.x == b.x )
					{
						if( a.y < b.y )
						{
							return true;
						}
						else if( a.y == b.y )
						{
							if( a.level < b.level )
							{
								return true;
							}
						}
					}

					return false;
				} );

				auto it = std::unique( cells.Begin(), cells.End() );

				cells.Remove( it, cells.End() );
				cells.Shrink();
			}

			static void Subtract( const red::DynArray< CellID > & A, const red::DynArray< CellID > & B, red::DynArray< CellID > & out )
			{
				//A - B
				out.Reserve( A.Size() );

				auto first1 = A.Begin();
				auto last1 = A.End();

				auto first2 = B.Begin();
				auto last2 = B.End();

				while( first1 != last1 )
				{
					if( first2 == last2 )
					{
						out.PushBack( MakeArraySpan( first1, last1 ) );
						break;
					}

					if( *first1 < *first2 )
					{
						out.PushBack( *first1 );
						++first1;
					}
					else
					{
						if( !( *first2 < *first1 ) )
						{
							++first1;
						}
						++first2;
					}
				}
			}
		}

	}
}

#include "influenceLayer.hpp"