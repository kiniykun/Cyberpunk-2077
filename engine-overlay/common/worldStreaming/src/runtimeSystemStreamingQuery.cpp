/**
* Copyright (c) 2020 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include <cstdio>

#include "../../spacePartitioning/include/subdivisionTreeCollectors.h"
#include "../../renderDraw/include/fragmentBuilder.h"
#include "../../spline/include/splineUtils.h"
#include "../../renderData/include/drawer.h"
#include "../../world/include/streamingQueryRoadData.h"
#include "../../world/include/renderDebugContext.h"
#include "runtimeSystemWorldStreaming.h"
#include "streamingQueryDataResource.h"
#include "runtimeSystemStreamingQuery.h"

RTTI_BEGIN_TYPE_IN_NAMESPACE(RuntimeSystemStreamingQuery, world);
	RTTI_PARENT_TYPE(IRuntimeSystem);
RTTI_END_TYPE();

namespace world
{

	RuntimeSystemStreamingQuery::RuntimeSystemStreamingQuery()
	{
	}

	RuntimeSystemStreamingQuery::~RuntimeSystemStreamingQuery()
	{
	}

	SystemID RuntimeSystemStreamingQuery::GetSystemID() const
	{
		return SYSTEM_ID;
	}

	void RuntimeSystemStreamingQuery::Init(const THandle<StreamingQueryDataResource>& dataResource)
	{
		m_dataResource = dataResource;
//		m_roadDataQuadTree.SetPool(PoolStreamingQuery());

		if (m_dataResource)
		{
			for (const auto& roadData : m_dataResource->GetRoadDatas())
			{
				const Box& localBounds = roadData.GetLocalBoundingBox();
				const Box worldBounds = roadData.GetTransform().TransformBox(localBounds);
				m_roadDataQuadTree.Insert(worldBounds, &roadData);
			}
		}
	}

	void RuntimeSystemStreamingQuery::OnStreamingWorldLoaded(world::RuntimeScene& scene, job::Counter& counter)
	{
		auto streamingSystem = scene.GetSystem< world::RuntimeSystemWorldStreaming >();
		RED_FATAL_ASSERT(streamingSystem);

		// CP2077 web: the asserts here are compiled out in FINAL. When the session's .streamingworld
		// failed to load (RuntimeSystemWorldStreaming keeps m_streamingWorld null by design), or it has
		// no streaming query data, IssueLoadingRequest() returns nullptr and nullptr->OnLoaded() reads a
		// garbage weak-this pointer from low memory -> "operation does not support unaligned accesses".
		if (!streamingSystem)
		{
			std::fprintf(stderr, "CP2077 streaming query guard: no RuntimeSystemWorldStreaming; skipping road data\n");
			return;
		}

		auto streamingWorld = streamingSystem->GetStreamingWorld();
		RED_FATAL_ASSERT(streamingWorld);
		if (!streamingWorld)
		{
			std::fprintf(stderr, "CP2077 streaming query guard: no streaming world loaded (missing .streamingworld?); skipping road data\n");
			return;
		}

		if (!streamingWorld->m_streamingQueryDataResource.IsValid())
		{
			std::fprintf(stderr, "CP2077 streaming query guard: streaming world has no streamingquerydata path; skipping road data\n");
			return;
		}

		auto token = streamingWorld->m_streamingQueryDataResource.IssueLoadingRequest();
		if (!token)
		{
			std::fprintf(stderr, "CP2077 streaming query guard: loading request returned no token; skipping road data\n");
			return;
		}
		counter += token->OnLoaded([this](const THandle< CResource > res) {
			Init(Cast< StreamingQueryDataResource >(res));
		});
	}
	
	void RuntimeSystemStreamingQuery::OnPostWorldDetached(RuntimeScene& scene)
	{
		m_roadDataQuadTree.Clear();
		m_dataResource = nullptr;
	}

	namespace helper
	{
		struct QueryCollector
		{
			red::DynArray< const StreamingQueryRoadData* >& m_collected;

			QueryCollector(red::DynArray< const StreamingQueryRoadData* >& collected)
				: m_collected(collected)
			{
			}

			Bool operator()(const Box&, const StreamingQueryRoadData* data)
			{
				m_collected.PushBack(data);
				return true;
			}
		};
	}

	void RuntimeSystemStreamingQuery::QueryNN(const Vector3& queryPoint, Uint32 nnMax, red::DynArray< const StreamingQueryRoadData* >& outResults) const
	{
		using Collector = helper::QueryCollector;
		Collector collector{ outResults };

		m_roadDataQuadTree.QueryNN(queryPoint, collector, nnMax);
	}

	void RuntimeSystemStreamingQuery::QueryConnections(const StreamingQueryRoadData& roadData, red::DynArray< const StreamingQueryRoadData* >& outResults) const
	{
		if (m_dataResource)
		{
			m_dataResource->GetConnectedRoads(roadData, outResults);
		}
	}

	static rend::DebugFilter DF_StreamingQuery_RoadData("RuntimeSystem/StreamingQuery/RoadData");

	void RuntimeSystemStreamingQuery::OnRenderDebug(const RenderDebugContext& context)
	{
		if (!context.debugFilterMask.Test(DF_StreamingQuery_RoadData))
		{
			return;
		}

		if (m_dataResource)
		{
			red::DynArray<const StreamingQueryRoadData*> connectedRoads{ red::PoolDebug() };
			for (const auto& roadData : m_dataResource->GetRoadDatas())
			{		
				roadData.RenderDebugSpline(context, Color::WHITE());;

				m_dataResource->GetConnectedRoads(roadData, connectedRoads);
				for (const StreamingQueryRoadData* connectedRoad : connectedRoads)
				{
					roadData.RenderRoadConnection(context, *connectedRoad, Color::LIGHT_BLUE());
				}
			}
		}
	}
}
