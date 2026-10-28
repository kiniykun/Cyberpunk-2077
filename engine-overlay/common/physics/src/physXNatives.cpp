#include "build.h"
#include "physXNatives.h"
#include "physXErrorHandler.h"
#include "physXVisualDebugger.h"
#include "physicsMaterialLibrary.h"
#include "physXStreamIO.h"
#include "physicsCollection.h"
#include "physicsCollisionFilterResource.h"

#include "physicsCollider.h"
#include "physXCpuDispatcher.h"
#include "physicsSystemResourceValidator.h"
#include "physicsIDLookup.h"
#include "physicsClothResourceInfo.h"
#include "physicsClothDebug.h"
#include "physicsNestedScene.h"

#include "../../redReflection/include/scriptStackFrame.h"
#include "../../redReflection/include/gatheredResource.h"
#include "../../redFileSystem/include/fileSys.h"
#include "../../resource/include/jsonResource.h"
#include "../../redJobs2/include/jobBuilder.h"

#ifdef RED_PLATFORM_WINPC
#include "../../redCore/include/processRunner.h"
#endif

#ifndef NO_EDITOR
#include "../../../common/redReflection/include/util.h"
#endif

#ifdef PHYSICS_DEBUG
#pragma optimize("", off)
#endif

namespace Config
{
	TConfigVar< String > cvPhysicsCollisionFilteringResource( "Physics", "CollisionFilteringResource", "engine\\physics\\collision_groups.json", 0 );
}

namespace physics
{
	res::TStaticResource<JsonResource> g_filterResource( RED_CONST_RESOURCEPATH( "engine\\physics\\collision_groups.json" ), GRF_Startup );

	namespace prv
	{
		Transform ScaleTransform( const Transform& base, const Vector3& scale )
		{
			auto m = Matrix::IDENTITY();
			m.SetScale( scale );
            
			return { m.TransformPoint( base.GetPosition() ), base.GetOrientation() };
		}

		void CreatePxTriangleMeshDesc( physx::PxTriangleMeshDesc& out, const THandle<ColliderMesh>& in, const PhysXErrorPtr& error )
		{
			out.setToDefault();
            
			out.points.count = in->GetNumPoints();
			out.points.stride = sizeof( physx::PxVec3 );
			out.points.data = in->GetPoints();
			out.triangles.count = in->GetNumIndices() / 3;
			out.triangles.data = in->GetIndices();
			out.triangles.stride = 3 * sizeof( Uint16 );
			out.flags = physx::PxMeshFlag::e16_BIT_INDICES;
#ifdef RED_LOGGING_ENABLED
			out.userData = error.Get();
#else
			RED_TOUCH( error );
#endif
		}
        
		void CreatePxConvexMeshDesc( physx::PxConvexMeshDesc& out, Uint32 numPoints, const void* data, const red::UniquePtr<PhysXError>& error, const Uint32 vertexLimit = 64, Bool enablePlaneShifting = false )
		{
			out.setToDefault();
            
			out.points.count = numPoints;
			out.points.stride = sizeof( physx::PxVec3 );
			out.points.data = data;
			out.flags = physx::PxConvexFlag::eCOMPUTE_CONVEX | physx::PxConvexFlag::eCHECK_ZERO_AREA_TRIANGLES | physx::PxConvexFlag::eSHIFT_VERTICES;
			if( enablePlaneShifting )
			{
				out.flags |= physx::PxConvexFlag::ePLANE_SHIFTING;
			}
			out.vertexLimit = vertexLimit;
            
#ifdef RED_LOGGING_ENABLED
			out.userData = error.Get();
#else
			RED_TOUCH( error );
#endif
		}
	}
    
	struct Natives
	{
		RED_USE_MEMORY_POOL( PoolPhysicsInternal );
        
		PhysXAllocator m_allocator;
		PhysXErrorHandler m_errorHandler;
		PhysXAssertHandler m_assertHandler;

		PhysxUniquePtr< physx::PxFoundation > m_foundation;
		red::UniquePtr< PhysXVisualDebugger > m_pvd;
		PhysxUniquePtr< physx::PxPhysics > m_physics;
		PhysxUniquePtr< physx::PxCooking > m_cooking;
		PhysxUniquePtr< physx::PxSerializationRegistry > m_registry;
		red::UniquePtr<PhysXCpuDispatcher> m_cpuDispatcher;
		red::UniquePtr< PhysicsMaterialLibrary > m_materialLibrary;
		THandle< CollisionFilterResource > m_filterResource;
		PhysxUniquePtr<physx::PxMaterial> m_ragdollMaterial;

#if PHYSICS_USE_NV_CLOTH
		NvClothAllocator m_clothAllocator;
		nv::cloth::Factory* m_clothFactory = nullptr;
        red::LightMutex m_fabricMutex;
#endif
		red::AbsolutePath m_fileDumpPath;
	};

	static red::UniquePtr< Natives > physics_natives;

	red::AbsolutePath GetPVDFileDumpDirectory()
	{
#if defined( RED_PLATFORM_WINPC )
		return GFileManager->GetEngineRoot().AddDirPath("PVDDump");
#elif defined( RED_PLATFORM_ORBIS )
		return red::AbsolutePath::CreateDirPath( "/data/" );
#elif defined( RED_PLATFORM_DURANGO )
		return red::AbsolutePath::CreateDirPath( "D:\\PVDDump\\" );
#elif defined( RED_PLATFORM_WEB ) || defined( RED_PLATFORM_LINUX )
		return red::AbsolutePath::CreateDirPath( "/tmp/" );
#else
		#error "Please define temp directory"
		return red::AbsolutePath();
#endif
	}

	Bool CreateNatives()
	{
		physics_natives = red::CreateUniquePtr< Natives >();
        
		physics_natives->m_foundation = MakePhysxUniquePtr( PxCreateFoundation( PX_FOUNDATION_VERSION, physics_natives->m_allocator, physics_natives->m_errorHandler ) );
		if( !physics_natives->m_foundation )
		{
			RED_LOG_ERROR( "Failed to create PxFoundation" );
			return false;
		}
        
		physics_natives->m_pvd = red::CreateUniquePtr< PhysXVisualDebugger >();
		if( !physics_natives->m_pvd )
		{
			RED_LOG_ERROR( "Failed to create the PVD" );
			return false;
		}
        
		physx::PxTolerancesScale scale;
		physics_natives->m_physics = MakePhysxUniquePtr( PxCreatePhysics( PX_PHYSICS_VERSION, PxGetFoundation(), scale, false, physics_natives->m_pvd->GetNative() ) );
		if( !physics_natives->m_physics )
		{
			RED_LOG_ERROR( "Failed to create PxPhysics" );
			return false;
		}

		physx::PxSetAssertHandler( physics_natives->m_assertHandler );
        
		PxInitExtensions( *physics_natives->m_physics, physics_natives->m_pvd->GetNative() );
        
		physx::PxTolerancesScale scaleCooking;
		scaleCooking.length = 0.1f;
        
		physx::PxCookingParams params( scaleCooking );
		params.meshWeldTolerance = 0.001f;
		params.meshPreprocessParams = physx::PxMeshPreprocessingFlags( physx::PxMeshPreprocessingFlag::eWELD_VERTICES );
		params.midphaseDesc = physx::PxMeshMidPhase::eBVH34;
		params.midphaseDesc.mBVH34Desc.numTrisPerLeaf = 15;
		params.suppressTriangleMeshRemapTable = true;
        
		physics_natives->m_cooking = MakePhysxUniquePtr( PxCreateCooking( PX_PHYSICS_VERSION, PxGetFoundation(), params ) );
		if( !physics_natives->m_cooking )
		{
			RED_LOG_ERROR( "Failed to create PxCooking" );
			return false;
		}
        
		physics_natives->m_registry = MakePhysxUniquePtr( physx::PxSerialization::createSerializationRegistry( PxGetPhysics() ) );

		physics_natives->m_materialLibrary = red::CreateUniquePtr< PhysicsMaterialLibrary >();
		if( !physics_natives->m_materialLibrary )
		{
			RED_LOG_ERROR( "Failed to create material library" );
			return false;
		}
        
		//@todo [william.mcvicar 01.05.2018] The goal is to be passed the counter, when that happens remove this one and pass directly to initialize
		job::Builder builder{ job::Priority::Latent };
		physics_natives->m_materialLibrary->Initialize( builder );
		auto counter = builder.ExtractWaitCounter();
		job::FlushCounter( counter );
        
		physics_natives->m_fileDumpPath = GetPVDFileDumpDirectory();
		auto json_resource = Cast<JsonResource>( g_filterResource.Get() );
		RED_FATAL_ASSERT( json_resource, "Error: Failed to load the collision filtering resource" );
		physics_natives->m_filterResource = json_resource->GetRoot<CollisionFilterResource>();
		RED_FATAL_ASSERT( physics_natives->m_filterResource, "Error: Collision filtering resource must exist" );
		physics_natives->m_filterResource->Initialize();
        
		physics_natives->m_cpuDispatcher = red::CreateUniquePtr<PhysXCpuDispatcher>();
		RED_FATAL_ASSERT( physics_natives->m_cpuDispatcher, "Error: Failed to allocate cpu dispatcher" );

#if PHYSICS_USE_NV_CLOTH
		nv::cloth::InitializeNvCloth( &physics_natives->m_clothAllocator, &physics_natives->m_errorHandler, nullptr, physics_natives->m_cpuDispatcher.Get() );
		physics_natives->m_clothFactory = NvClothCreateFactoryCPU();
#endif
        
		CreateProxyInternals();
		CreateNestedSceneData();
        
		physics_natives->m_ragdollMaterial = MakePhysxUniquePtr(physics_natives->m_physics->createMaterial( 1.f, 1.f, 0.25f ));
		physics_natives->m_ragdollMaterial->setFrictionCombineMode( physx::PxCombineMode::eAVERAGE );
		physics_natives->m_ragdollMaterial->setRestitutionCombineMode( physx::PxCombineMode::eAVERAGE );

		return true;
	}
    
	void CreateNativesAsync( const job::RunContext& context, red::Atomic< Uint32 >& keepRunning )
	{
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		job::Builder builder{ context };
		builder.DispatchJob( "Physics/NativesCreation/Init", [&keepRunning]( const job::RunContext& context )
		{
			if ( !keepRunning.GetValue() )
			{
				return;
			}

			physics_natives = red::CreateUniquePtr< Natives >();
        
			physics_natives->m_foundation = MakePhysxUniquePtr( PxCreateFoundation( PX_FOUNDATION_VERSION, physics_natives->m_allocator, physics_natives->m_errorHandler ) );
			if( !physics_natives->m_foundation )
			{
				RED_LOG_ERROR( "Failed to create PxFoundation" );
				keepRunning.SetValue( 0 );
				return;
			}
        
			physics_natives->m_pvd = red::CreateUniquePtr< PhysXVisualDebugger >();
			if( !physics_natives->m_pvd )
			{
				RED_LOG_ERROR( "Failed to create the PVD" );
				keepRunning.SetValue( 0 );
				return;
			}
        
			physx::PxTolerancesScale scale;
			physics_natives->m_physics = MakePhysxUniquePtr( PxCreatePhysics( PX_PHYSICS_VERSION, PxGetFoundation(), scale, false, physics_natives->m_pvd->GetNative() ) );
			if( !physics_natives->m_physics )
			{
				RED_LOG_ERROR( "Failed to create PxPhysics" );
				keepRunning.SetValue( 0 );
				return;
			}
        
			physx::PxSetAssertHandler( physics_natives->m_assertHandler );

			PxInitExtensions( *physics_natives->m_physics, physics_natives->m_pvd->GetNative() );
        
			physx::PxTolerancesScale scaleCooking;
			scaleCooking.length = 0.1f;
        
			physx::PxCookingParams params( scaleCooking );
			params.meshWeldTolerance = 0.001f;
			params.meshPreprocessParams = physx::PxMeshPreprocessingFlags( physx::PxMeshPreprocessingFlag::eWELD_VERTICES );
			params.midphaseDesc = physx::PxMeshMidPhase::eBVH34;
			params.suppressTriangleMeshRemapTable = true;
        
			physics_natives->m_cooking = MakePhysxUniquePtr( PxCreateCooking( PX_PHYSICS_VERSION, PxGetFoundation(), params ) );
			if( !physics_natives->m_cooking )
			{
				RED_LOG_ERROR( "Failed to create PxCooking" );
				keepRunning.SetValue( 0 );
				return;
			}

			physics_natives->m_registry = MakePhysxUniquePtr( physx::PxSerialization::createSerializationRegistry( PxGetPhysics() ) );

			physics_natives->m_materialLibrary = red::CreateUniquePtr< PhysicsMaterialLibrary >();
		} );

		builder.DispatchJob( "Physics/NativesCreation/MaterialLibInit", [&keepRunning]( const job::RunContext& context )
		{
			if ( !keepRunning.GetValue() )
			{
				return;
			}

			job::Builder builder{ context };
			physics_natives->m_materialLibrary->Initialize( builder );
		} );

		builder.DispatchJob( "Physics/NativesCreation/Finishing", [&keepRunning]( const job::RunContext& context )
		{
			if ( !keepRunning.GetValue() )
			{
				return;
			}

			physics_natives->m_fileDumpPath = GetPVDFileDumpDirectory();

#if PHYSICS_USE_NV_CLOTH
			nv::cloth::InitializeNvCloth( &physics_natives->m_clothAllocator, &physics_natives->m_errorHandler, nullptr, nullptr );
			physics_natives->m_clothFactory = NvClothCreateFactoryCPU();
#endif
			auto json_resource = Cast<JsonResource>( g_filterResource.Get() );
			RED_FATAL_ASSERT( json_resource, "Error: Failed to load the collision filtering resource" );
			physics_natives->m_filterResource = json_resource->GetRoot<CollisionFilterResource>();
			RED_FATAL_ASSERT( physics_natives->m_filterResource, "Error: Collision filtering resource must exist" );
			physics_natives->m_filterResource->Initialize();
        
			physics_natives->m_cpuDispatcher = red::CreateUniquePtr<PhysXCpuDispatcher>();
			RED_FATAL_ASSERT( physics_natives->m_cpuDispatcher, "Error: Failed to allocate cpu dispatcher" );
        
			CreateProxyInternals();
			CreateNestedSceneData();

			physics_natives->m_ragdollMaterial = MakePhysxUniquePtr(physics_natives->m_physics->createMaterial( 1.f, 1.f, 0.25f ));
			physics_natives->m_ragdollMaterial->setFrictionCombineMode( physx::PxCombineMode::eAVERAGE );
			physics_natives->m_ragdollMaterial->setRestitutionCombineMode( physx::PxCombineMode::eAVERAGE );

			cloth::debug::Create();
		} );
	}

	void DestroyNatives()
	{
		if(physics_natives)
		{
			cloth::debug::Destroy();

			DestroyNestedSceneData();
			DestroyProxyInternals();
#if PHYSICS_USE_NV_CLOTH
			NvClothDestroyFactory(physics_natives->m_clothFactory);
#endif
			PxCloseExtensions();
			physics_natives = nullptr;
		}
	}
    
	Bool AreNativesCreated()
	{
		return physics_natives != nullptr;
	}

	void EditorOnly_LoadFilterResources()
	{
		physics_natives = red::CreateUniquePtr< Natives >();
        
		auto token = GResourceLoader->IssueLoadingRequest(res::ResourcePath::Build(Config::cvPhysicsCollisionFilteringResource.Get()));
		token->OnLoaded([](THandle<CResource> res)
		{
			auto jsonRes = Cast<JsonResource>(res);
			RED_FATAL_ASSERT(jsonRes, "Error: Failed to load the collision filtering resource");
			physics_natives->m_filterResource = jsonRes->GetRoot<CollisionFilterResource>();
			RED_FATAL_ASSERT(physics_natives->m_filterResource, "Error: Collision filtering resource must exist");
			physics_natives->m_filterResource->Initialize();
		});
	}
    
    Bool PxSerializationDumperOnly_CreateNatives()
    {
        physics_natives = red::CreateUniquePtr< Natives >();
        
		physics_natives->m_foundation = MakePhysxUniquePtr( PxCreateFoundation( PX_FOUNDATION_VERSION, physics_natives->m_allocator, physics_natives->m_errorHandler ) );
		if( !physics_natives->m_foundation )
		{
			RED_LOG_ERROR( "Failed to create PxFoundation" );
			return false;
		}
        
		physics_natives->m_pvd = red::CreateUniquePtr< PhysXVisualDebugger >();
		if( !physics_natives->m_pvd )
		{
			RED_LOG_ERROR( "Failed to create the PVD" );
			return false;
		}
        
		physx::PxTolerancesScale scale;
		physics_natives->m_physics = MakePhysxUniquePtr( PxCreatePhysics( PX_PHYSICS_VERSION, PxGetFoundation(), scale, false, physics_natives->m_pvd->GetNative() ) );
		if( !physics_natives->m_physics )
		{
			RED_LOG_ERROR( "Failed to create PxPhysics" );
			return false;
		}
        
		PxInitExtensions( *physics_natives->m_physics, physics_natives->m_pvd->GetNative() );
        
		physics_natives->m_registry = MakePhysxUniquePtr( physx::PxSerialization::createSerializationRegistry( PxGetPhysics() ) );

        return true;
    }
    
	void PvdClientConnect( IScriptable*, CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( String, text, String::EMPTY() );
		FINISH_PARAMETERS;
		if( text.Empty() )
			text = String( "127.0.0.1" );
		physics_natives->m_pvd->ClientConnect( text );
		RETURN_VOID();
	}
    
	void PvdFileDumpConnect( IScriptable*, CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( String, text, String::EMPTY() );
		FINISH_PARAMETERS;
		Utf16String path = Utf16String( (text + ".pxd2").AsChar() );
		physics_natives->m_pvd->FileDumpConnect( red::AbsolutePath::CreateFilePath( path ) );
		RETURN_VOID();
	}
    
	void GetQueryMask( IScriptable*, CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( red::DynArray< CName >, names, red::PoolScript() );
		FINISH_PARAMETERS;
        
		RETURN_STRUCT( QueryFilter, physics::QueryFilter::Make( names ) );
		RETURN_VOID();
	}
    
	void GetQueryMaskAll( IScriptable*, CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		RETURN_STRUCT( QueryFilter, QueryFilter::ALL() );
		RETURN_VOID();
	}
    
	void GetQueryMaskNONE( IScriptable*, CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		RETURN_STRUCT( QueryFilter, QueryFilter::ZERO() );
		RETURN_VOID();
	}
    
	void RegisterPhysicsScriptFunctions()
	{
		RTTI_DEFINE_NATIVE_GLOBAL_FUNCTION( "PvdClientConnect", PvdClientConnect );
		RTTI_DEFINE_NATIVE_GLOBAL_FUNCTION( "PvdFileDumpConnect", PvdFileDumpConnect );
		RTTI_DEFINE_NATIVE_GLOBAL_FUNCTION( "GetQueryMask", GetQueryMask );
		RTTI_DEFINE_NATIVE_GLOBAL_FUNCTION( "GetQueryMaskAll", GetQueryMaskAll );
		RTTI_DEFINE_NATIVE_GLOBAL_FUNCTION( "GetQueryMaskNONE", GetQueryMaskNONE );
	}
    
	physx::PxCooking& GetPxCooking()
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_cooking, "Error: Cooking has not yet been created" );
		return *physics_natives->m_cooking;
	}
    
	physx::PxMaterial& GetPxDefaultMaterial()
	{
		RED_FATAL_ASSERT( physics_natives->m_materialLibrary, "Error: material library has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_materialLibrary->GetDefaultMaterial().m_nativeMaterial, "Error: default material has not yet been created" );
		return *physics_natives->m_materialLibrary->GetDefaultMaterial().m_nativeMaterial;
	}
    
	physx::PxMaterial* GetPxDefaultMaterialPtr()
	{
		RED_FATAL_ASSERT( physics_natives->m_materialLibrary, "Error: material library has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_materialLibrary->GetDefaultMaterial().m_nativeMaterial, "Error: default material has not yet been created" );
		return physics_natives->m_materialLibrary->GetDefaultMaterial().m_nativeMaterial;
	}

	physx::PxCollection* GetMaterialCollection()
	{
		RED_FATAL_ASSERT( physics_natives->m_materialLibrary, "Error: material library has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_materialLibrary->GetCollection(), "Error: Material collection has not yet been created" );
		return physics_natives->m_materialLibrary->GetCollection();
	}

	physx::PxSerializationRegistry& GetPxSerializationRegistry()
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_registry, "Error: PxSerializationRegistry has not yet been created" );
		return *physics_natives->m_registry;
	}
    
	Bool ConnectVisualDebugger( const String& ip_address )
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_pvd, "Error: PhysX Visual Debugger has not yet been created" );
		return physics_natives->m_pvd->ClientConnect( ip_address );
	}
    
	Bool FileDumpVisualDebugger()
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_pvd, "Error: PhysX Visual Debugger has not yet been created" );
        
		red::DateTime time;
		red::Clock::GetInstance().GetLocalTime( time );
		String date = String::Printf( "%04d-%02d-%02d_%02d-%02d-%02d-%03d",
                                     time.GetYear(), time.GetMonth() + 1, time.GetDay() + 1,
                                     time.GetHour(), time.GetMinute(), time.GetSecond(), time.GetMilliSeconds() );
        
		auto logFilename = String::Printf( "pvd_dump_%hs.pxd2", date.AsChar() );
        
		GFileManager->CreatePath( physics_natives->m_fileDumpPath );
		auto fileNameString = physics_natives->m_fileDumpPath.AddFilePath( logFilename );
        
		return physics_natives->m_pvd->FileDumpConnect( fileNameString );
	}
    
	Bool ReconnectVisualDebugger( const String& ip_address )
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_pvd, "Error: PhysX Visual Debugger has not yet been created" );
		return physics_natives->m_pvd->Reconnect( ip_address );
	}
    
	void DisconnectVisualDebugger()
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_pvd, "Error: PhysX Visual Debugger has not yet been created" );
		physics_natives->m_pvd->Disconnect();
	}
    
	Bool IsConnectedToVisualDebugger()
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_pvd, "Error: PhysX Visual Debugger has not yet been created" );
		return physics_natives->m_pvd->IsConnected();
	}
    
	Bool LaunchVisualDebugger( const String& ip_address )
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_pvd, "Error: PhysX Visual Debugger has not yet been created" );
#ifdef RED_PLATFORM_WINPC
		if( !ReconnectVisualDebugger( ip_address ) )
		{
			const auto path = GFileManager->GetEngineRoot().AddDirPath( "tools\\PhysxVisualDebugger" ).AddFilePath( "PVD3.exe" );
			CProcessRunner pvdProcess;
			pvdProcess.Run( path, Utf16String(), red::utils::GetCurrentWorkingDirectory() );
		}
#endif
		return true;
	}
    
	PhysicsMaterialLibrary& GetMaterialLibrary()
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_materialLibrary, "Error: Material library has not yet been created" );
		return *physics_natives->m_materialLibrary;
	}
    
	const CollisionFilterResource& GetFilterResource()
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_filterResource, "Error: Filter resource not yet created" );
		return *physics_natives->m_filterResource;
	}
    
	nv::cloth::Factory* GetNvClothFactory()
	{
#if PHYSICS_USE_NV_CLOTH
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_clothFactory, "Error: Cloth factory has not yet been created" );
		return physics_natives->m_clothFactory;
#else
		return nullptr;
#endif
	}
    
	// #tbd: PxDefaultMemoryOutputStream has no C++ rtti, so causes linking errors with UBSan vptr option
#ifndef NO_EDITOR
	
	physx::PxBinaryConverter* CreateBinaryConverter(ECookingPlatform platform, Bool verbose)
	{
		physx::PxBinaryConverter* result = nullptr;

		result = physx::PxSerialization::createBinaryConverter();
		result->setReportMode(verbose ? physx::PxConverterReportMode::eVERBOSE : physx::PxConverterReportMode::eNORMAL);

		auto dst_metadata_path = GFileManager->GetEngineRoot().AddDirPath("physics");
		switch (platform)
		{
		case PLATFORM_None:
		case PLATFORM_PC:
		case PLATFORM_WindowsServer:
			dst_metadata_path.AppendFilePath("win64.metaData");
			break;

		case PLATFORM_PS4:
			dst_metadata_path.AppendFilePath("ps4.metaData");
			break;

		case PLATFORM_XboxOne:
			dst_metadata_path.AppendFilePath("xboxone.metaData");
			break;

		case PLATFORM_LinuxServer:
			dst_metadata_path.AppendFilePath("linux64.metaData");
			break;
		}

		physx::PxDefaultMemoryOutputStream tmp;
		physx::PxSerialization::dumpBinaryMetaData(tmp, GetPxSerializationRegistry());
		physx::PxDefaultMemoryInputData srcMetaData(tmp.getData(), tmp.getSize());

		physx::PxDefaultFileInputData dstMetaData(dst_metadata_path.AsChar());
		if (!result->setMetaData(srcMetaData, dstMetaData))
		{
			PHYSICS_LOG_ERROR("Failed to set binary conversion meta data");
			return nullptr;
		}

		return result;
	}
    
    void DestroyBinaryConverter(physx::PxBinaryConverter** converter)
    {
        physx::PxBinaryConverter* pxConverter = *converter;
        *converter = nullptr;
        
        if( pxConverter )
            pxConverter->release();
    }
    
	Bool ConvertBinaryData( DataBuffer& buffer, physx::PxBinaryConverter* converter )
	{
		if( converter )
		{
			physx::PxDefaultMemoryInputData src( (physx::PxU8*)buffer.Data(), buffer.Size() );
			physx::PxDefaultMemoryOutputStream dst;
            
			if( !converter->convert( src, buffer.Size(), dst ) )
				return false;
            
			buffer = DataBuffer::Copy( dst.getData(), dst.getSize(), PX_SERIAL_FILE_ALIGN, PoolPhysics() );
		}
        
		return true;
	}
    
	Bool ConvertBinaryData( const serialization::BufferHandleRO& in, DataBuffer& outData, physx::PxBinaryConverter* converter )
	{
		if( converter )
		{
			physx::PxDefaultMemoryInputData src( (physx::PxU8*)in.Data(), in.GetSize() );
			physx::PxDefaultMemoryOutputStream dst;
            
			if( !converter->convert( src, in.GetSize(), dst ) )
				return false;
            
			outData = DataBuffer::Copy( dst.getData(), dst.getSize(), PX_SERIAL_FILE_ALIGN, PoolPhysics() );
		}

		return true;
	}
	
	Bool SerializeAndConvertPxCollection( DataBuffer& buffer, physx::PxBinaryConverter& converter, physx::PxCollection& collection, physx::PxCollection* external )
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_registry, "Error: Serialization Registry not yet created" );

		physx::PxSerialization::complete(collection, *physics_natives->m_registry, external);

#if 0 //removed for performance, can redefine this when debugging issues as a sanity check
		if( !physx::PxSerialization::isSerializable( collection, *physics_natives->m_registry, external ) )
		{
			RED_FATAL("Error: Collection is not serializable");
			return false;
		}
#endif

		physx::PxDefaultMemoryOutputStream stream;
		if( !physx::PxSerialization::serializeCollectionToBinary( stream, collection, *physics_natives->m_registry, external ) )
			return false;

		physx::PxDefaultMemoryInputData src( stream.getData(), stream.getSize() );
		physx::PxDefaultMemoryOutputStream dst;

		if( !converter.convert( src, src.getLength(), dst ) )
			return false;

		buffer = DataBuffer::Copy( dst.getData(), dst.getSize(), PX_SERIAL_FILE_ALIGN, PoolPhysics() );
		return true;
	}

	Bool SerializePxCollectionToDataBuffer( DataBuffer& buffer, physx::PxCollection& collection, physx::PxCollection* external )
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_registry, "Error: Serialization Registry not yet created" );

		physx::PxSerialization::complete(collection, *physics_natives->m_registry, external);

#if 0 //removed for performance, can redefine this when debugging issues as a sanity check
		if( !physx::PxSerialization::isSerializable( collection, *physics_natives->m_registry, external ) )
		{
			RED_FATAL("Error: Collection is not serializable");
			return false;
		}
#endif

		physx::PxDefaultMemoryOutputStream stream;
		if( !physx::PxSerialization::serializeCollectionToBinary( stream, collection, *physics_natives->m_registry, external ) )
			return false;

		buffer = DataBuffer::Copy( stream.getData(), stream.getSize(), PX_SERIAL_FILE_ALIGN, PoolPhysics() );
		return true;
	}

	Bool SerializePxCollectionToDeferredDataBuffer( serialization::DeferredDataBuffer& buffer, physx::PxCollection& collection, physx::PxCollection* external )
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_registry, "Error: Serialization Registry not yet created" );

		physx::PxSerialization::complete(collection, *physics_natives->m_registry, external);

#if 0
		if( !physx::PxSerialization::isSerializable( collection, *physics_natives->m_registry, external ) )
		{
			RED_FATAL("Error: Collection is not serializable");
			return false;
		}
#endif

		physx::PxDefaultMemoryOutputStream stream;
		if( !physx::PxSerialization::serializeCollectionToBinary( stream, collection, *physics_natives->m_registry, external ) )
			return false;

		buffer.Editor_SetContentDirect( stream.getData(), stream.getSize(), PX_SERIAL_FILE_ALIGN );
		return true;
	}

	Bool SerializePxCollectionToXml( IFile& file, physx::PxCollection& collection, physx::PxCollection* external )
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_registry, "Error: Serialization Registry not yet created" );

		physx::PxSerialization::complete(collection, *physics_natives->m_registry, external);

		if( !physx::PxSerialization::isSerializable( collection, *physics_natives->m_registry, external ) )
			return false;

		FileWriter stream( file );

		if( !physx::PxSerialization::serializeCollectionToXml( stream, collection, *physics_natives->m_registry, nullptr, external ) )
			return false;
        
		return true;
	}
    
    void DumpPxCollectionToBinary(IFile& file, physx::PxCollection* collection, physx::PxCollection* external)
    {
        FileWriter writer(file);
        physx::PxSerialization::serializeCollectionToBinaryDeterministic( writer, *collection, *physics_natives->m_registry, external );
    }
    
	Uint64 CalculatePxSerializationId( const res::ResourcePath& path, Uint64 shapeIdx )
	{
		const auto parentPath = red::paths::ExtractParentPath( path.ToStringView() );
		const auto fileStem = red::paths::ExtractFileStem( path.ToStringView() );
		const auto extension = red::paths::ExtractExtension( path.ToStringView() );
        
		auto tmp = String::Printf( "%hs%hs_%llx.%hs", parentPath.AsChar(), fileStem.AsChar(), shapeIdx, extension.AsChar() );
		return red::CalculateHash64WithLength( tmp.AsChar(), tmp.Length() );
	}
    
    physx::PxActor* CreateRigidStatic( const Transform& xform, const CName& name )
    {
        if( auto actor = PxGetPhysics().createRigidStatic(ToPxTransform(xform)) )
        {
            actor->setName(name.AsChar());
            return actor;
        }
        
        return nullptr;
    }
    
	void ReleaseActor( physx::PxActor*& actor )
	{
		if ( actor )
		{
			actor->release();
			actor = nullptr;
		}
	}
    
	Bool TryAttachPxShapeToActor( physx::PxShape* shape, physx::PxActor* actor, physics::FilterData& filter, Uint32 id )
    {
        physics::ApplyFilterDataToShape(shape,filter);
        physics::ConstructUserData(shape, id );
        
        return TryAttachPxShapeToActor(shape, actor);
    }
    
    Bool TryAttachPxShapeToActor(physx::PxShape* shape, physx::PxActor* actor)
    {
        if( auto* rigidActor = actor->is<physx::PxRigidActor>() )
        {
            const auto previous_count = rigidActor->getNbShapes();
            rigidActor->attachShape(*shape);
            
            if( shape->isExclusive() )
                shape->release();
            
            return rigidActor->getNbShapes() > previous_count;
        }
        
        return false;
    }

	math::Box GetWorldBounds(physx::PxActor* actor)
	{
		if( auto* pxActor = actor->is<physx::PxRigidActor>() )
		{
			return ToBox(actor->getWorldBounds(1.f));
		}

		return { math::Box::EResetState::RESET_STATE };
	}

	ShapeType GetShapeType( const physx::PxShape* shape )
	{
		const physx::PxGeometryType::Enum pxType = shape->getGeometryType();

		switch ( pxType )
		{
			case physx::PxGeometryType::eBOX:
				return ShapeType::Box;
			case physx::PxGeometryType::eCAPSULE:
				return ShapeType::Capsule;
			case physx::PxGeometryType::eSPHERE:
				return ShapeType::Sphere;
			case physx::PxGeometryType::eCONVEXMESH:
				return ShapeType::ConvexMesh;
			case physx::PxGeometryType::eTRIANGLEMESH:
				return ShapeType::TriangleMesh;
			case physx::PxGeometryType::eHEIGHTFIELD:
				return ShapeType::Heightfield;
			default:
				return ShapeType::Invalid;
		}
	}

	Int32 FindShapeCount( Int32 type, Int32 i, physx::PxActor* actor )
    {
        if( auto* pxActor = actor->is<physx::PxRigidActor>() )
        {
            red::DynArray<physx::PxShape*> shapes(pxActor->getNbShapes(), PoolPhysics());
            pxActor->getShapes(shapes.TypedData(),pxActor->getNbShapes());
            
            return i + std::accumulate(shapes.Begin(),shapes.End(),0,[type](Int32 j, const physx::PxShape* shape){
				return static_cast<Int32>(shape->getGeometryType()) == type;
            });
        }
        
        return 0;
    }

	Int32 GetNbShapes(physx::PxActor* actor)
	{
		if(auto* pxActor = actor->is<physx::PxRigidActor>())
		{
			return pxActor->getNbShapes();
		}

		return 0;
	}

	Uint32 GetNbObjects(physx::PxCollection& collection)
	{
		return collection.getNbObjects();
	}
#endif
    
    physx::PxCollection* CreateEmptyPxCollection()
    {
        return PxCreateCollection();
    }

	physx::PxCollection* CreatePxCollectionFromBinary( void* data )
	{
		return physx::PxSerialization::createCollectionFromBinary(data, GetPxSerializationRegistry(), GetMaterialCollection());
	}
    
    void DestroyPxCollection(physx::PxCollection** collection)
    {
        (*collection)->release();
        *collection = nullptr;
    }
    
    void DestroyShape(physx::PxShape** shape)
    {
        (*shape)->release();
        *shape = nullptr;
    }
    
    void AddToPxCollection(physx::PxCollection& collection, physx::PxBase& base)
    {
        collection.add(base);
    }
    
    void AddTriangleMeshToPxCollection(physx::PxCollection& collection, physx::PxTriangleMesh& trimesh)
    {
        collection.add(trimesh);
    }
    
    void AddActorToPxCollection(physx::PxCollection& collection, physx::PxActor& actor)
    {
        collection.add(actor);
    }
    
    void AddActorsToPxCollection(physx::PxCollection& collection, red::ArraySpan<physx::PxActor*> actors)
    {
        for( auto* actor : actors )
		{
			collection.add(*actor);
		}
    }

	void AddAggregateToPxCollection( physx::PxCollection& collection, physx::PxAggregate& aggregate )
	{
		collection.add( aggregate );
	}

    void DestroyActors(red::ArraySpan<physx::PxActor*> actors)
    {
        for( auto* actor : actors )
            actor->release();
    }
    
	physx::PxTriangleMesh* CreatePxTriangleMesh( const THandle<ColliderMesh>& mesh, const red::UniquePtr<PhysXError>& error )
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
        
		physx::PxTriangleMeshDesc desc;
		prv::CreatePxTriangleMeshDesc( desc, mesh, error );
        
		if( !mesh->m_faceMaterialIndices.Empty() )
		{
			desc.materialIndices.data = mesh->GetFaceMaterialIndices();
			desc.materialIndices.stride = sizeof( Uint16 );
		}
        
		MemoryWriteBuffer buf;
		if( !GetPxCooking().cookTriangleMesh( desc, buf ) )
		{
			return nullptr;
		}
        
		MemoryReadBuffer mrb( buf.GetData(), buf.GetSize() );
		return PxGetPhysics().createTriangleMesh( mrb );
	}
    
	physx::PxConvexMesh* CreatePxConvexMesh( const THandle<ColliderConvex>& convex, const red::UniquePtr<PhysXError>& error, const Uint32 vertexLimit, Bool enablePlaneShifting )
	{
		return CreatePxConvexMesh( convex->GetNumPoints(), convex->m_vertices.TypedData(), error, vertexLimit, enablePlaneShifting );
	}
    
	physx::PxConvexMesh* CreatePxConvexMesh(red::BlobSpan span)
	{
		physx::PxDefaultMemoryInputData inputStream(span.Pointer<physx::PxU8>(), span.Size());
		return PxGetPhysics().createConvexMesh(inputStream);
	}

	physx::PxTriangleMesh* CreatePxTriangleMesh(red::BlobSpan span)
	{
		physx::PxDefaultMemoryInputData inputStream(span.Pointer<physx::PxU8>(), span.Size());
		return PxGetPhysics().createTriangleMesh(inputStream);
	}

	physx::PxHeightField* CreatePxHeightfield(red::BlobSpan span)
	{
		physx::PxDefaultMemoryInputData inputStream(span.Pointer<physx::PxU8>(), span.Size());
		return PxGetPhysics().createHeightField(inputStream);
	}

	physx::PxConvexMesh* CreatePxConvexMesh( Uint32 numPoints, const void* data, const red::UniquePtr<PhysXError>& error, const Uint32 vertexLimit, Bool enablePlaneShifting )
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_cooking, "Error: Physics Cooking has not yet been created" );
        
		physx::PxConvexMeshDesc convexDesc;
		prv::CreatePxConvexMeshDesc( convexDesc, numPoints, data, error, vertexLimit, enablePlaneShifting );
        
		MemoryWriteBuffer buf;
		physx::PxConvexMeshCookingResult::Enum condition;
        
		if( !GetPxCooking().cookConvexMesh( convexDesc, buf, &condition ) )
		{
			if( condition & physx::PxConvexMeshCookingResult::Enum::eZERO_AREA_TEST_FAILED )
				RED_LOG_ERROR( "Algorithm couldn't find 4 initial vertices without a small triangle." );
            
			if( condition & physx::PxConvexMeshCookingResult::Enum::eFAILURE )
				RED_LOG_ERROR( "Convex cooking algorithm failed because something failed internally and it couldn't recover." );
            
			return nullptr;
		}
        
		if( condition & physx::PxConvexMeshCookingResult::Enum::ePOLYGONS_LIMIT_REACHED )
			RED_LOG_ERROR( "The algorithm has reached the 255 polygons limit. The produced hull does not contain all input vertices. Try to simplify the input vertices" );
        
		MemoryReadBuffer mrb( buf.GetData(), buf.GetSize() );
		return PxGetPhysics().createConvexMesh( mrb );
	}
    
	physx::PxShape* CreatePxTriangleMeshShape( const THandle<ColliderMesh>& mesh, const math::Vector3& scale, const red::UniquePtr<PhysXError>& error )
	{
		if( auto* meshData = CreatePxTriangleMesh( mesh, error ) )
			return CreatePxTriangleMeshShape( mesh, meshData, scale );
        
		return nullptr;
	}
    
	physx::PxShape* CreatePxTriangleMeshShape( const THandle<ColliderMesh>& mesh, physx::PxTriangleMesh* data, const math::Vector3& scale )
	{
		red::DynArray<physx::PxMaterial*> materials{ PoolPhysics() };
		auto& material_library = GetMaterialLibrary();
		if( !mesh->m_faceMaterials.Empty() )
		{
			for( auto& mat : mesh->m_faceMaterials )
			{
				materials.PushBack( material_library.FindMaterial( mat )->m_nativeMaterial );
			}
		}
		else
		{
			materials.PushBack( material_library.FindMaterial( mesh->m_material )->m_nativeMaterial );
		}
        
		physx::PxTriangleMeshGeometry geom;
		geom.triangleMesh = data;
		geom.scale.scale = ToPxVec3( scale );
        
		if( auto* shape = PxGetPhysics().createShape( geom, materials.TypedData(), (physx::PxU16)materials.Size(), true ) )
		{
			auto xform = prv::ScaleTransform( mesh->m_localToBody, scale );
			shape->setLocalPose( ToPxTransform( xform ) );
			physics::ConstructUserData( shape, mesh->GetID().CalcHash() );
			return shape;
		}
        
		return nullptr;
	}
    
	physx::PxShape* CreatePxConvexMeshShape( const THandle<ColliderConvex>& convex, const math::Vector3& scale )
	{
		if( auto* convexData = CreatePxConvexMesh( convex, nullptr ) )
			return CreatePxConvexMeshShape( convex, convexData, scale );
        
		return nullptr;
	}

	bool CookPxTriangleMeshToDataBuffer(const THandle<ColliderMesh>& mesh, DataBuffer& out, const PhysXErrorPtr& error)
	{
		physx::PxTriangleMeshDesc desc;
		desc.points.count = mesh->m_points.Size();
		desc.points.stride = sizeof( decltype(mesh->m_points)::ElementType );
		desc.points.data = mesh->m_points.TypedData();
		desc.triangles.count = mesh->m_indices.Size() / 3;
		desc.triangles.stride = 3 * sizeof( decltype(mesh->m_indices)::ElementType );
		desc.triangles.data = mesh->m_indices.TypedData();

		if ( mesh->m_faceMaterials.Size() != 0 )
		{
			desc.materialIndices.data = mesh->m_faceMaterialIndices.TypedData();
			desc.materialIndices.stride = sizeof( decltype(mesh->m_faceMaterialIndices)::ElementType );
		}

		desc.flags = physx::PxMeshFlag::e16_BIT_INDICES;

#ifdef RED_LOGGING_ENABLED
		desc.userData = error.Get();
#else
		RED_TOUCH( error );
#endif

		physx::PxDefaultMemoryOutputStream outputStream;
		physx::PxTriangleMeshCookingResult::Enum result;
		if( GetPxCooking().cookTriangleMesh(desc, outputStream, &result) )
		{
			out = DataBuffer::Copy( outputStream.getData(), outputStream.getSize(), 16, PoolPhysicsCollision());
			return true;
		}

		return false;
	}

	bool CookPxConvexMeshToDataBuffer(const THandle<ColliderConvex>& convex, DataBuffer& out, const PhysXErrorPtr& error)
	{
		physx::PxConvexMeshDesc desc;
		desc.points.count = convex->m_vertices.Size();
		desc.points.stride = sizeof( Vector3 );
		desc.points.data = convex->m_vertices.TypedData();

		desc.flags = physx::PxConvexFlag::eCOMPUTE_CONVEX | physx::PxConvexFlag::eCHECK_ZERO_AREA_TRIANGLES | physx::PxConvexFlag::eSHIFT_VERTICES;
		desc.flags |= convex->RequiresPlaneShifting() ? physx::PxConvexFlag::ePLANE_SHIFTING : (physx::PxConvexFlag::Enum)0;

#ifdef RED_LOGGING_ENABLED
		desc.userData = error.Get();
#else
		RED_TOUCH( error );
#endif

		physx::PxDefaultMemoryOutputStream outputStream;
		physx::PxConvexMeshCookingResult::Enum result;
		if( GetPxCooking().cookConvexMesh(desc, outputStream, &result) )
		{
			out = DataBuffer::Copy( outputStream.getData(), outputStream.getSize(), 16, PoolPhysicsCollision());
			return true;
		}

		return false;
	}

	bool CookPxHeightfieldToDataBuffer(const THandle<DeferredCollection>& collection, DataBuffer& out, const red::UniquePtr<PhysXError>& error)
	{
		if( auto* c = collection->GetSerializedCollection() )
		{
			for( Uint32 i = 0; i < c->getNbObjects(); ++i )
			{
				auto& base = c->getObject(i);
		        if( base.getConcreteType() == physx::PxConcreteType::eHEIGHTFIELD )
		        {
					auto* hf = base.is<physx::PxHeightField>();

					const Uint32 rows = hf->getNbRows();
					const Uint32 cols = hf->getNbColumns();
					const Uint32 size = rows*cols*sizeof(physx::PxHeightFieldSample);

					auto buffer = red::CreateUniqueBuffer<red::PoolBackend>( size, 16);
					auto bytes_written = hf->saveCells(buffer.Data(), buffer.GetSize());
					if( bytes_written != size )
					{
						return false;
					}

					physx::PxHeightFieldDesc desc;
					desc.nbColumns = hf->getNbColumns();
					desc.nbRows = hf->getNbRows();
					desc.convexEdgeThreshold = hf->getConvexEdgeThreshold();
					desc.flags = hf->getFlags();
					desc.format = hf->getFormat();
					desc.thickness = hf->getThickness();
					desc.samples.stride = hf->getSampleStride();
					desc.samples.data = buffer.Data();

					physx::PxDefaultMemoryOutputStream outputStream;
					if( GetPxCooking().cookHeightField(desc, outputStream) )
					{
						out = DataBuffer::Copy( outputStream.getData(), outputStream.getSize(), 16, PoolPhysicsCollision());
						return true;
					}
				}
			}
		}
		
		return false;
	}
    
	physx::PxShape* CreatePxConvexMeshShape( const THandle<ColliderConvex>& convex, physx::PxConvexMesh* data, const math::Vector3& scale )
	{
		physx::PxConvexMeshGeometry geom;
		geom.convexMesh = data;
		geom.scale.scale = ToPxVec3( scale );
        geom.meshFlags = physx::PxConvexMeshGeometryFlag::eTIGHT_BOUNDS;
        
		const auto& materialRes = *GetMaterialLibrary().FindMaterial( convex->m_material );
		auto* pxMaterial = materialRes.m_nativeMaterial;
		if( auto* shape = PxGetPhysics().createShape( geom, *pxMaterial, true ) )
		{
			auto xform = prv::ScaleTransform( convex->m_localToBody, scale );
			shape->setLocalPose( ToPxTransform( xform ) );
			physics::ConstructUserData( shape, convex->GetID().CalcHash() );
			return shape;
		}
        
		return nullptr;
	}
    
	void ApplyFilterDataToShape( physx::PxShape* shape, const FilterData& filterData )
	{
		physx::PxFilterData simFilter, queryFilter;
		filterData.BuildPxQueryFilter( queryFilter );
		filterData.BuildPxSimulationData( simFilter );
		shape->setSimulationFilterData( simFilter );
		shape->setQueryFilterData( queryFilter );
    }

    Transform GetShapeLocalPose( physx::PxShape* shape )
    {
        return ToTransform( shape->getLocalPose() );
    }

    void SetLocalPoseToShape( physx::PxShape* shape, const Transform& localPose )
    {
        shape->setLocalPose( ToPxTransform( localPose ) );
    }

	void DumpPhysXBinaryMetaData()
	{
#if defined( RED_PLATFORM_WINPC )
		const char* path = "..\\..\\engine\\physics\\win64.metaData";

#elif defined( RED_PLATFORM_DURANGO )
		const char* path = "d:\\xboxone.metaData";

#elif defined( RED_PLATFORM_ORBIS )
		const char* path = "/data/ps4.metaData";

#elif defined( RED_PLATFORM_WEB )
        const char* path = "/userdata/physx-wasm64.metaData";
#elif defined( RED_PLATFORM_LINUX )
		const char* path = "../linux64.metaData";
#else
#	error Unsupported platform
#endif
        
		physx::PxDefaultFileOutputStream file( path );
		physx::PxSerialization::dumpBinaryMetaData( file, GetPxSerializationRegistry() );
	}

	void DumpMaterialCollection( red::AbsolutePath path, bool xml )
	{
		auto* collection = GetMaterialCollection();
		auto file_ptr = GFileManager->CreateFileWriter( path, FOF_AbsolutePath );
		FileWriter file( *file_ptr );

		if( xml )
		{
			physx::PxSerialization::serializeCollectionToXml( file, *collection, GetPxSerializationRegistry() );
		}
		else
		{
			physx::PxSerialization::serializeCollectionToBinaryDeterministic( file, *collection, GetPxSerializationRegistry() );
		}
	}

	Bool ValidateColliderMesh( const THandle<ColliderMesh>& mesh, const red::UniquePtr<PhysXError>& error )
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );

		const auto num_vertices = mesh->GetNumPoints();

		if( num_vertices < 3 )
		{
			error->ReportDataError( SystemResourceValidator::FailReason_TriangleMeshNotEnoughTriangles, "" );
			return false;
		}

		if( num_vertices > 0xffff )
		{
			error->ReportDataError( SystemResourceValidator::FailReason_TriangleMeshHasTooManyVertices, "" );
			return false;
		}

		if( (!mesh->GetIndices()) )
		{
			error->ReportDataError( SystemResourceValidator::FailReason_TriangleMeshHasNullData, "" );
			return false;
		}

		physx::PxTriangleMeshDesc desc;
		prv::CreatePxTriangleMeshDesc( desc, mesh, error );

		MemoryWriteBuffer buf;
		physx::PxTriangleMeshCookingResult::Enum result;
		if( !GetPxCooking().cookTriangleMesh( desc, buf, &result ) )
		{
			if( result & physx::PxTriangleMeshCookingResult::eLARGE_TRIANGLE )
			{
				error->ReportDataError( SystemResourceValidator::FailReason_TriangleMeshHasLargeTriangles, "" );
				return false;
			}

			if( result & physx::PxTriangleMeshCookingResult::eFAILURE )
			{
				return false;
			}
		}

		return true;
	}

	Bool ValidateColliderConvex( const THandle<ColliderConvex>& convex, const red::UniquePtr<PhysXError>& error )
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Physics Natives has not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_cooking, "Error: Physics Cooking has not yet been created" );
        
		const auto numPoints = convex->GetNumPoints();
        
		if( numPoints < 3 )
		{
			error->ReportDataError( SystemResourceValidator::FailReason_ConvexHullNotEnoughTris, "" );
			return false;
		}
        
		if( numPoints > 0xffff )
		{
			error->ReportDataError( SystemResourceValidator::FailReason_ConvexHullTooManyInputPoints, "" );
			return false;
		}
        
		if( convex->m_vertices.Size() == 0 )
		{
			error->ReportDataError( SystemResourceValidator::FailReason_ConvexHullInputDataIsNull, "" );
			return false;
		}

		physx::PxConvexMeshDesc convexDesc;
		prv::CreatePxConvexMeshDesc( convexDesc, convex->GetNumPoints(), convex->m_vertices.TypedData(), error );

		MemoryWriteBuffer buf;
		physx::PxConvexMeshCookingResult::Enum condition;

		if( !GetPxCooking().cookConvexMesh( convexDesc, buf, &condition ) )
		{
			if( condition & physx::PxConvexMeshCookingResult::Enum::eZERO_AREA_TEST_FAILED )
			{
				error->ReportDataError( SystemResourceValidator::FailReason_ConvexHullHasSmallTriangles, "" );
				return false;
			}
		}

		if( condition & physx::PxConvexMeshCookingResult::Enum::ePOLYGONS_LIMIT_REACHED )
		{
			error->ReportDataError( SystemResourceValidator::FailReason_ConvexHullHasTooManyPolygons, "" );
			return false;
		}
        
		return true;
	}

	Vector3 CalcPrimitiveExtents_Sphere( Vector3 rotatedScale, Float radius )
	{
		const auto safeRadius = std::max< Float >( k_physicsAbsoluteMinSize, radius );
		return Vector3{ safeRadius * std::max( { rotatedScale.X, rotatedScale.Y, rotatedScale.Z } ), 1.f, 1.f };
	}

	Vector3 CalcPrimitiveExtents_Capsule( Vector3 rotatedScale, Float radius, Float height )
	{
		const auto safeRadius = std::max< Float >( k_physicsAbsoluteMinSize, radius );
		Float radiusScale = std::max( rotatedScale.X, rotatedScale.Y );
		return Vector3{ safeRadius * radiusScale, height * 0.5f * rotatedScale.Z, 1.f };
	}

	Vector3 CalcPrimitiveExtents_Box( Vector3 rotatedScale, math::Vector3& halfExtents )
	{
		const auto X = std::max< Float >( k_physicsAbsoluteMinSize, halfExtents.X * rotatedScale.X );
		const auto Y = std::max< Float >( k_physicsAbsoluteMinSize, halfExtents.Y * rotatedScale.Y );
		const auto Z = std::max< Float >( k_physicsAbsoluteMinSize, halfExtents.Z * rotatedScale.Z );

		return { X, Y, Z };
	}

	void ComputePrimitiveDimensions(math::Vector3& dims, math::Transform& localToBody, math::Vector3 scale, Uint8 type)
	{
		Matrix outLocalToBody = localToBody.ToMatrix();
		Vector3 rotatedScale = scale;
		physics::ICollider::AdjustScaleAndPose( rotatedScale, outLocalToBody);

		if( static_cast<ShapeType>(type) == ShapeType::Sphere )
		{
			dims = CalcPrimitiveExtents_Sphere( rotatedScale, dims.X);
		}
		else if( static_cast<ShapeType>(type) == ShapeType::Capsule )
		{
			dims = CalcPrimitiveExtents_Capsule( rotatedScale, dims.X, dims.Y );

		}
		else if( static_cast<ShapeType>(type) == ShapeType::Box )
		{
			dims = CalcPrimitiveExtents_Box( rotatedScale, dims );
		}

		localToBody = outLocalToBody.ToXform();
	}
    
	PhysXCpuDispatcher& GetCPUDispatcher()
	{
		RED_FATAL_ASSERT( physics_natives, "Error: Natives have not yet been created" );
		RED_FATAL_ASSERT( physics_natives->m_registry, "Error: CPU Dispatcher not yet created" );
		return *physics_natives->m_cpuDispatcher;
	}
    
	Uint32 GetSerializedFileAlignment()
	{
		return PX_SERIAL_FILE_ALIGN;
	}

	Uint8 GetPxShapeFlags(const THandle<ICollider>& collider)
	{
		physx::PxShapeFlags shapeFlags = physx::PxShapeFlag::eVISUALIZATION | physx::PxShapeFlag::eSCENE_QUERY_SHAPE;

		if(!collider->m_isQueryShapeOnly)
			shapeFlags |= physx::PxShapeFlag::eSIMULATION_SHAPE;

		return static_cast<Uint8>(shapeFlags);
	}

	Uint8 GetDefaultPxShapeFlags()
	{
		physx::PxShapeFlags shapeFlags = physx::PxShapeFlag::eVISUALIZATION | physx::PxShapeFlag::eSCENE_QUERY_SHAPE | physx::PxShapeFlag::eSIMULATION_SHAPE;
		return static_cast<Uint8>(shapeFlags);
	}

	void ExtractHeightfieldGeometry( const physx::PxHeightFieldGeometry &geom, const Transform& shapeLocalPose, red::DynArray<Vector3> &outVertices, red::DynArray<Uint32> &outIndices )
	{
		const Uint32 nbRows = geom.heightField->getNbRows();
		const Uint32 nbCols = geom.heightField->getNbColumns();
		const Uint32 nbVerts = nbRows * nbCols;
        
		auto data = red::CreateUniqueBuffer<red::PoolBackend>( nbVerts * sizeof( physx::PxHeightFieldSample ), 16 );
		geom.heightField->saveCells( data.Get(), data.GetSize() );
        
		physx::PxHeightFieldSample* samples = static_cast<physx::PxHeightFieldSample*>(data.Get());
		outVertices.Resize( nbVerts );
        
		for( Uint32 i = 0; i < nbRows; i++ )
		{
			for( Uint32 j = 0; j < nbCols; j++ )
			{
				Vector3 vtx{ Float( i ) * geom.rowScale, Float( samples[ j + i * nbCols ].height ) * geom.heightScale, Float( j ) * geom.columnScale };
				outVertices[ i*nbCols + j ] = shapeLocalPose.TransformPoint( vtx );
			}
		}
        
		const Uint32 quadsPerRow = nbRows - 1;
		const Uint32 quadsPerCol = nbCols - 1;
		outIndices.Reserve( quadsPerRow*quadsPerCol * 6 );
        
		for( Uint32 i = 0; i < quadsPerRow; i++ )
		{
			for( Uint32 j = 0; j < quadsPerCol; j++ )
			{
				auto corner0 = (j + i * nbCols);
				auto corner1 = corner0 + 1;
				auto corner2 = corner0 + nbCols;
				auto corner3 = corner2 + 1;
                
				Bool corner0_isNotHole = (samples[ corner0 ].materialIndex0 != physx::PxHeightFieldMaterial::eHOLE) && (samples[ corner0 ].materialIndex1 != physx::PxHeightFieldMaterial::eHOLE);
				Bool corner1_isNotHole = (samples[ corner1 ].materialIndex0 != physx::PxHeightFieldMaterial::eHOLE) && (samples[ corner1 ].materialIndex1 != physx::PxHeightFieldMaterial::eHOLE);
				Bool corner2_isNotHole = (samples[ corner2 ].materialIndex0 != physx::PxHeightFieldMaterial::eHOLE) && (samples[ corner2 ].materialIndex1 != physx::PxHeightFieldMaterial::eHOLE);
				Bool corner3_isNotHole = (samples[ corner3 ].materialIndex0 != physx::PxHeightFieldMaterial::eHOLE) && (samples[ corner3 ].materialIndex1 != physx::PxHeightFieldMaterial::eHOLE);
                
				if( corner0_isNotHole && corner1_isNotHole && corner2_isNotHole && corner3_isNotHole )
				{
					outIndices.PushBack( corner0 );
					outIndices.PushBack( corner2 );
					outIndices.PushBack( corner3 );
                    
					outIndices.PushBack( corner0 );
					outIndices.PushBack( corner3 );
					outIndices.PushBack( corner1 );
                    
					RED_FATAL_ASSERT( corner0 < nbVerts, "Error: Invalid idx count" );
					RED_FATAL_ASSERT( corner1 < nbVerts, "Error: Invalid idx count" );
					RED_FATAL_ASSERT( corner2 < nbVerts, "Error: Invalid idx count" );
					RED_FATAL_ASSERT( corner3 < nbVerts, "Error: Invalid idx count" );
				}
			}
		}
	}

#ifndef NO_EDITOR
	void ExtractTerrainCollisionInfo(const THandle<DeferredCollection>& collection, red::DynArray<CName>& materials, WorldTransform& actorTransform, Float& rowScale, Float& colScale, Float& heightScale)
	{
        if( auto* px_collection = collection->GetSerializedCollection() )
        {
            for( Uint32 i = 0; i < px_collection->getNbObjects(); ++i )
            {
				if ( auto* px_actor = px_collection->getObject( i ).is<physx::PxRigidStatic>() )
				{
					actorTransform = ToWorldTransform(px_actor->getGlobalPose());
				}
				else if ( auto* px_shape = px_collection->getObject( i ).is<physx::PxShape>() )
				{
					physx::PxHeightFieldGeometry geom;
					px_shape->getHeightFieldGeometry( geom );
					rowScale = geom.rowScale;
					colScale = geom.columnScale;
					heightScale = geom.heightScale;

					for( Uint32 i = 0; i < px_shape->getNbMaterials(); ++i)
					{
						physx::PxMaterial* current_material = nullptr;
						px_shape->getMaterials(&current_material, 1, i);
						auto name = GetUserData(current_material);
						materials.PushBack(name);
					}
				}
			}
		}
	}

    Bool ExtractGeometryFromTerrainDataForNavMesh(const THandle<DeferredCollection>& collection, red::DynArray< Vector3 >& outVertices, red::DynArray< Uint32 >& outIndices)
    {
        RED_FATAL_ASSERT(collection->m_buffer.GetSize() > 0, "Error: Deferred Data Buffer not loaded");
        RED_FATAL_ASSERT(outVertices.Empty(), "Error: vertices must be empty");
        RED_FATAL_ASSERT(outIndices.Empty(), "Error: indices must be empty");

        THandle<physics::SystemResource> result = nullptr;
		physx::PxTransform xform;
		physx::PxHeightFieldGeometry geom;

        if( auto* px_collection = collection->GetSerializedCollection() )
        {
            for( Uint32 i = 0; i < px_collection->getNbObjects(); ++i )
            {
				if ( auto* px_actor = px_collection->getObject( i ).is<physx::PxRigidStatic>() )
				{
					xform = px_actor->getGlobalPose();
				}
				else if ( auto* px_shape = px_collection->getObject( i ).is<physx::PxShape>() )
				{
					px_shape->getHeightFieldGeometry( geom );
				}				
            }

			ExtractHeightfieldGeometry( geom, ToTransform( xform ), outVertices, outIndices );
			return true;
        }

        return false;
    }

	THandle<DeferredCollection> CreateTerrainCollisionData(red::ArraySpan<const Float> heightData, red::ArraySpan<const Uint16> controlData,
	                                                       red::ArraySpan<const Uint8> materialData, red::ArraySpan<const CName> materialNames,
	                                                       Uint32 dimensions, math::Vector2 topLeft, Float texelSize)
    {
        constexpr Float PX_MIN_HEIGHTFIELD_Y_SCALE_ = (0.0001f / physx::PxReal(0xFFFF));

        RED_FATAL_ASSERT(physics_natives, "Error: Natives have not yet been created");
		RED_FATAL_ASSERT(physics_natives->m_registry, "Error: Serialization registry not yet created");
        RED_FATAL_ASSERT(physics_natives->m_cooking, "Error: Cooking not yet created");
        RED_FATAL_ASSERT(heightData.Size() == dimensions*dimensions, "Error: height data doesn't match up with dimensions");
        RED_FATAL_ASSERT(heightData.Size() == materialData.Size(), "Error: height data doesn't match up with material data");

        auto result = CreateHandle<DeferredCollection>();
        FilterData filter;
        filter.ApplyPreset(RED_NAME_CONSTEXPR("Terrain"));

        Float minHeight =  std::numeric_limits<Float>::max();
        Float maxHeight = -std::numeric_limits<Float>::max();

        for(auto flt : heightData)
        {
            minHeight = std::min(minHeight,flt);
            maxHeight = std::max(maxHeight,flt);
        }

        Float deltaHeight = maxHeight - minHeight;
        Float quantization = Float(0x7fff);

        Float heightScale = std::max(deltaHeight / quantization, PX_MIN_HEIGHTFIELD_Y_SCALE_ );
        texelSize = std::max( texelSize, PX_MIN_HEIGHTFIELD_XZ_SCALE );

        Uint8 maxMaterialIndex = !materialNames.Empty() ? (materialNames.Size() - 1u) : 0u;

        const Float divisor = (deltaHeight!=0.0f) ? deltaHeight : 1.0f;
        red::DynArray<physx::PxHeightFieldSample> samples{ heightData.Size(), red::PoolBackend() };

        for( Uint32 row = 0; row < dimensions; ++row )
        {
            for( Uint32 col = 0; col < dimensions; ++col )
            {
                const Uint32 i0 = row * dimensions + col;
                auto& sample = samples[i0];

                const Uint8 materialIndex = std::min( materialData[i0], maxMaterialIndex );
                sample.materialIndex0 = materialIndex;
                sample.materialIndex1 = materialIndex;

                if( !controlData.Empty() )
                {
                    const Uint32 nextCol = Min( col + 1, dimensions - 1 );
                    const Uint32 nextRow = Min( row + 1, dimensions - 1 );

                    const Uint32 i1 = row * dimensions + nextCol;
                    const Uint32 i2 = nextRow * dimensions + col;
                    const Uint32 i3 = nextRow * dimensions + nextCol;

                    // this have to match the code from terrainMeshBuilder.cpp
                    // there is a hole when at least one sample is marked as a hole
                    const Uint32 i0Hole = !(controlData[i0] & 0x8000);
                    const Uint32 i1Hole = !(controlData[i1] & 0x8000);
                    const Uint32 i2Hole = !(controlData[i2] & 0x8000);
                    const Uint32 i3Hole = !(controlData[i3] & 0x8000);

                    if( i0Hole | i1Hole | i2Hole | i3Hole )
                    {
                        sample.materialIndex0 = physx::PxHeightFieldMaterial::eHOLE;
                        sample.materialIndex1 = physx::PxHeightFieldMaterial::eHOLE;
                    }
                }

                sample.height = Uint16( quantization * ((heightData[i0] - minHeight) / divisor) );
                sample.setTessFlag();
            }
        }

        RED_FATAL_ASSERT( materialNames.Size() <= 16u, "Too many physical materials" );
        red::StaticArray< physx::PxMaterial*, 16u > materials;
        for ( Uint32 i = 0; i < materialNames.Size(); ++i )
        {
            THandle<physics::MaterialResource> material = physics_natives->m_materialLibrary->FindMaterial( materialNames[i] );
            RED_FATAL_ASSERT( material != nullptr, "Unable to find physical material '%hs' (for terrain layer %u), it was not found in the library.\r\n"
                                                   "Maybe there is a wrong material name in *.terrainsetup file?", materialNames[i].AsChar(), i );
            materials.PushBack( material->m_nativeMaterial );
        }

        physx::PxHeightFieldDesc desc;
        desc.format = physx::PxHeightFieldFormat::eS16_TM;
        desc.flags = physx::PxHeightFieldFlag::eNO_BOUNDARY_EDGES;
        desc.nbRows = dimensions;
        desc.nbColumns = dimensions;
        desc.samples.data = samples.TypedData();
        desc.samples.stride = sizeof(physx::PxHeightFieldSample);

        physx::PxHeightFieldGeometry hf_geom;
        hf_geom.heightScale = (deltaHeight!=0.0f) ? heightScale : 1.0f;
        hf_geom.heightFieldFlags = physx::PxMeshGeometryFlags();
        hf_geom.rowScale = texelSize;
        hf_geom.columnScale = texelSize;
        hf_geom.heightField = physics_natives->m_cooking->createHeightField(desc, PxGetPhysics().getPhysicsInsertionCallback());

        RED_FATAL_ASSERT(hf_geom.isValid(), "Error: Invalid hf_geom");
        Vector4 pos = { topLeft.X, topLeft.Y, minHeight, 1.0f };
        Quaternion q(0.5f, 0.5f, 0.5f, 0.5f);

		auto* actor = PxGetPhysics().createRigidStatic(ToPxTransform(Transform{pos, q}));

        physx::PxShape* shape;
        if ( materials.Empty() )
            shape = physx::PxRigidActorExt::createExclusiveShape(*actor, hf_geom, *physics_natives->m_materialLibrary->GetDefaultMaterial().m_nativeMaterial);
        else
            shape = physx::PxRigidActorExt::createExclusiveShape( *actor, hf_geom, materials.TypedData(), static_cast<physx::PxU16>(materials.Size()) );

		physics::ConstructUserData( shape, SerializableID::GenerateRuntimeId().CalcHash() );
        ApplyFilterDataToShape(shape, filter);

		auto* external = GetMaterialCollection();
        auto collection = PxCreateCollection();
        collection->add(*actor);
		
		physx::PxSerialization::complete(*collection, *physics_natives->m_registry, external);

		physx::PxDefaultMemoryOutputStream stream;
		if( physx::PxSerialization::serializeCollectionToBinaryDeterministic(stream, *collection, *physics_natives->m_registry, external ) )
		{
			result->m_buffer.Editor_SetContentDirect(stream.getData(), stream.getSize(), PX_SERIAL_FILE_ALIGN);
		}

        physx::PxCollectionExt::releaseObjects(*collection);
        collection->release();

        return result;
    }
#endif
}

namespace physics
{
    namespace cloth
    {
    #if PHYSICS_USE_NV_CLOTH
        struct SerializeFabricContext
        {
            red::ArraySpan<math::Vector4> defaultPose;
            red::ArraySpan<Float> maxDistance;
			red::ArraySpan<Float> maxDistanceExt;
            red::ArraySpan<Float> backstopDistance;
			red::ArraySpan<Float> backstopRadius;
            red::ArraySpan<Uint32> selfCollision;
            red::ArraySpan<nv::cloth::PhaseConfig> phaseConfigs;
            RuntimeInfo info;
        };

        DataBuffer SerializeFabricToDataBuffer(nv::cloth::Fabric* fabric, SerializeFabricContext ctx)
        {
            RED_FATAL_ASSERT(fabric, "Error: Fabric is a nullptr");

            const Uint32 header_size = sizeof(FabricHeader);
            const Uint32 runtimeInfo_size = sizeof(RuntimeInfo);
            const Uint32 phaseConfig_size = fabric->getNumPhases()*sizeof(nv::cloth::PhaseConfig);
            const Uint32 motionConstraints_size = ctx.maxDistance.SizeInBytes();
			const Uint32 motionConstraintsExt_size = ctx.maxDistanceExt.SizeInBytes();
            const Uint32 separationConstraints_size = ctx.backstopDistance.SizeInBytes();
			const Uint32 separationRadii_size = ctx.backstopRadius.SizeInBytes();
            const Uint32 selfCollisionIndices_size = ctx.selfCollision.SizeInBytes();
            const Uint32 sets_size = fabric->getNumSets()*sizeof(uint32_t);
            const Uint32 restvalues_size = fabric->getNumRestvalues()*sizeof(float);
            const Uint32 stiffnessValues_size = fabric->getNumStiffnessValues()*sizeof(float);
            const Uint32 indices_size = fabric->getNumIndices()*sizeof(uint32_t);
            const Uint32 tethers_size = fabric->getNumTethers()*sizeof(uint32_t);
            const Uint32 anchors_size = fabric->getNumTethers()*sizeof(float);
            const Uint32 triangles_size = fabric->getNumTriangles()*sizeof(uint32_t) * 3;

			auto padding = static_cast<Uint32>(red::memory::AlignAddress(header_size + runtimeInfo_size, 16)) - (header_size + runtimeInfo_size);

            const Uint32 dataSize = header_size + runtimeInfo_size + padding + ctx.defaultPose.SizeInBytes() + ctx.phaseConfigs.SizeInBytes() + phaseConfig_size + motionConstraints_size + 
            motionConstraints_size + separationConstraints_size + separationRadii_size + selfCollisionIndices_size + sets_size + restvalues_size + stiffnessValues_size + indices_size + 
			tethers_size + anchors_size + triangles_size;

            DataBuffer buffer = DataBuffer(dataSize, 16, physics::PoolPhysicsCloth());
            auto span = buffer.GetSpan();

            RuntimeInfo* runtimeInfo_begin = span.Pointer<RuntimeInfo>(sizeof(FabricHeader));
            RuntimeInfo* runtimeInfo_end = span.Pointer<RuntimeInfo>(sizeof(FabricHeader) + sizeof(RuntimeInfo));

            math::Vector4* particles_begin = (math::Vector4*)red::memory::AlignAddress(runtimeInfo_end, 16);
            math::Vector4* particles_end =  particles_begin + ctx.defaultPose.Size();

			RED_FATAL_ASSERT(red::memory::IsAligned(particles_begin, 16), "Error: Vector4 data not aligned to 16 bytes for SIMD");

            nv::cloth::PhaseConfig* phaseConfig_begin = (nv::cloth::PhaseConfig*)particles_end;
            nv::cloth::PhaseConfig* phaseConfig_end = phaseConfig_begin + ctx.phaseConfigs.Size();

            Float* motionConstraints_begin = (Float*)phaseConfig_end;
            Float* motionConstraints_end = motionConstraints_begin + ctx.maxDistance.Size();

			Float* motionConstraintsExt_begin = (Float*)motionConstraints_end;
            Float* motionConstraintsExt_end = motionConstraintsExt_begin + ctx.maxDistanceExt.Size();

            Float* separationConstraints_begin = (Float*)motionConstraintsExt_end;
            Float* separationConstraints_end = separationConstraints_begin + ctx.backstopDistance.Size();

			Float* separationRadii_begin = (Float*)separationConstraints_end;
            Float* separationRadii_end = separationRadii_begin + ctx.backstopRadius.Size();

            Uint32* selfCollisionIndices_begin = (Uint32*)separationRadii_end;
            Uint32* selfCollisionIndices_end = selfCollisionIndices_begin + ctx.selfCollision.Size();

            Uint32* phaseIndices_begin = (Uint32*)selfCollisionIndices_end;
            Uint32* phaseIndices_end = phaseIndices_begin + fabric->getNumPhases();

            Uint32* sets_begin = phaseIndices_end;
            Uint32* sets_end = sets_begin + fabric->getNumSets();

            Float* restvalues_begin = (Float*)sets_end;
            Float* restvalues_end = restvalues_begin + fabric->getNumRestvalues();

            Float* stiffnessvalues_begin = restvalues_end;
            Float* stiffnessvalues_end = stiffnessvalues_begin + fabric->getNumStiffnessValues();

            Uint32* indices_begin = (Uint32*)stiffnessvalues_end;
            Uint32* indices_end = indices_begin + fabric->getNumIndices();

            Uint32* anchors_begin = indices_end;
            Uint32* anchors_end = anchors_begin + fabric->getNumTethers();

            Float* tethers_begin = (Float*)anchors_end;
            Float* tethers_end = tethers_begin + fabric->getNumTethers();

            Uint32* triangles_begin = (Uint32*)tethers_end;
            Uint32* triangles_end = triangles_begin + fabric->getNumTriangles()*3;

            const Uint32* u32_data = span.Pointer<Uint32>();
            static_assert(std::is_standard_layout<RuntimeInfo>::value, "Error: NonStandardLayout");

            auto* header                        = span.Pointer<FabricHeader>();
            header->magic                       = CLOTH_MAGIC;
            header->version                     = CURRENT_VERSION;
            header->runtimeInfoOffset           = sizeof(FabricHeader);
            header->numParticles                = fabric->getNumParticles();
            header->particlesOffset             = red::ByteDistance<Uint32>(u32_data, particles_begin);
            header->numPhaseConfigs             = ctx.phaseConfigs.Size();
            header->phaseConfigOffset           = red::ByteDistance<Uint32>(u32_data, phaseConfig_begin);
            header->motionConstraintsOffset     = red::ByteDistance<Uint32>(u32_data, motionConstraints_begin);
			header->motionConstraintsExtOffset  = red::ByteDistance<Uint32>(u32_data, motionConstraintsExt_begin);
            header->separationConstraintsOffset = red::ByteDistance<Uint32>(u32_data, separationConstraints_begin);
			header->separationRadiiOffset		= red::ByteDistance<Uint32>(u32_data, separationRadii_begin);
            header->numSelfCollisionIndices     = ctx.selfCollision.Size();
            header->selfCollisionIndicesOffset  = red::ByteDistance<Uint32>(u32_data, selfCollisionIndices_begin);
            header->numPhaseIndices             = fabric->getNumPhases();
            header->phaseIndiciesOffset         = red::ByteDistance<Uint32>(u32_data, phaseIndices_begin);
            header->numSets                     = fabric->getNumSets();
            header->setsOffset                  = red::ByteDistance<Uint32>(u32_data, sets_begin);
            header->numRestValues               = fabric->getNumRestvalues();
            header->restValuesOffset            = red::ByteDistance<Uint32>(u32_data, restvalues_begin);
            header->numStiffnessValues          = fabric->getNumStiffnessValues();
            header->stiffnessValuesOffset       = red::ByteDistance<Uint32>(u32_data, stiffnessvalues_begin);
            header->numIndices                  = fabric->getNumIndices();
            header->indicesOffset               = red::ByteDistance<Uint32>(u32_data, indices_begin);
            header->numAnchors                  = fabric->getNumTethers();
            header->anchorsOffset               = red::ByteDistance<Uint32>(u32_data, anchors_begin);
            header->numTetherLengths            = fabric->getNumTethers();
            header->tetherLengthsOffset         = red::ByteDistance<Uint32>(u32_data, tethers_begin);
            header->numTriangles                = fabric->getNumTriangles();
            header->trianglesOffset             = red::ByteDistance<Uint32>(u32_data, triangles_begin);

            red::Memcpy(runtimeInfo_begin, &ctx.info, sizeof(RuntimeInfo));
            red::Memcpy(particles_begin, ctx.defaultPose.Data(), ctx.defaultPose.SizeInBytes());
            red::Memcpy(phaseConfig_begin, ctx.phaseConfigs.Data(), ctx.phaseConfigs.SizeInBytes());
            red::Memcpy(motionConstraints_begin, ctx.maxDistance.Data(), ctx.maxDistance.SizeInBytes());
			red::Memcpy(motionConstraintsExt_begin, ctx.maxDistanceExt.Data(), ctx.maxDistanceExt.SizeInBytes());
            red::Memcpy(separationConstraints_begin, ctx.backstopDistance.Data(), ctx.backstopDistance.SizeInBytes());
			red::Memcpy(separationRadii_begin, ctx.backstopRadius.Data(), ctx.backstopRadius.SizeInBytes());
            red::Memcpy(selfCollisionIndices_begin, ctx.selfCollision.Data(), ctx.selfCollision.SizeInBytes());

            fabric->getFactory().extractFabricData(*fabric,
                                                    nv::cloth::Range<uint32_t>(phaseIndices_begin, phaseIndices_end),
                                                    nv::cloth::Range<uint32_t>(sets_begin, sets_end),
                                                    nv::cloth::Range<float>(restvalues_begin, restvalues_end),
                                                    nv::cloth::Range<float>(stiffnessvalues_begin, stiffnessvalues_end),
                                                    nv::cloth::Range<uint32_t>(indices_begin, indices_end),
                                                    nv::cloth::Range<uint32_t>(anchors_begin, anchors_end),
                                                    nv::cloth::Range<float>(tethers_begin, tethers_end),
                                                    nv::cloth::Range<uint32_t>(triangles_begin, triangles_end) );

            return buffer;
        }

        nv::cloth::Fabric* CreateFabricFromView(red::BlobView view)
        {
            const auto* header = view.Pointer<FabricHeader>();

            const Uint32* phases_begin = view.Pointer<Uint32>(header->phaseIndiciesOffset);
            const Uint32* phases_end = phases_begin + header->numPhaseIndices;

            const Uint32* sets_begin = view.Pointer<Uint32>(header->setsOffset);
            const Uint32* sets_end = sets_begin + header->numSets;

            const Float* restvalues_begin = view.Pointer<Float>(header->restValuesOffset);
            const Float* restvalues_end = restvalues_begin + header->numRestValues;

            const Float* stiffnessvalues_begin = view.Pointer<Float>(header->stiffnessValuesOffset);
            const Float* stiffnessvalues_end = stiffnessvalues_begin + header->numStiffnessValues;

            const Uint32* indices_begin = view.Pointer<Uint32>(header->indicesOffset);
            const Uint32* indices_end = indices_begin + header->numIndices;

            const Uint32* anchors_begin = view.Pointer<Uint32>(header->anchorsOffset);
            const Uint32* anchors_end = anchors_begin + header->numAnchors;

            const Float* tethers_begin = view.Pointer<Float>(header->tetherLengthsOffset);
            const Float* tethers_end = tethers_begin + header->numTetherLengths;

            const Uint32* triangles_begin = view.Pointer<Uint32>(header->trianglesOffset);
            const Uint32* triangles_end = triangles_begin + header->numTriangles*3;

            RED_SCOPE_LOCK(physics_natives->m_fabricMutex);
            auto* fabric = physics::GetNvClothFactory()->createFabric(header->numParticles, 
                                                                      nv::cloth::Range<const uint32_t>(phases_begin, phases_end),
                                                                      nv::cloth::Range<const uint32_t>(sets_begin, sets_end),
                                                                      nv::cloth::Range<const Float>(restvalues_begin, restvalues_end),
                                                                      nv::cloth::Range<const Float>(stiffnessvalues_begin, stiffnessvalues_end),
                                                                      nv::cloth::Range<const uint32_t>(indices_begin, indices_end),
                                                                      nv::cloth::Range<const uint32_t>(anchors_begin, anchors_end),
                                                                      nv::cloth::Range<const Float>(tethers_begin, tethers_end),
                                                                      nv::cloth::Range<const uint32_t>(triangles_begin, triangles_end));

            return fabric;
        }

        nv::cloth::Cloth* CreateClothFromFabricAndView(red::BlobView view, nv::cloth::Fabric& fabric)
        {
            const auto* header = view.Pointer<FabricHeader>();

            auto* phaseTypes_begin = view.Pointer<nv::cloth::PhaseConfig>(header->phaseConfigOffset);
            auto phaseConfig = nv::cloth::Range<const nv::cloth::PhaseConfig>( phaseTypes_begin, phaseTypes_begin + header->numPhaseConfigs );

            const auto* particles_begin = view.Pointer<physx::PxVec4>(header->particlesOffset);
            auto particles = nv::cloth::Range<const physx::PxVec4>(particles_begin, particles_begin + header->numParticles);

            static_assert( sizeof( Vector4 ) == sizeof( physx::PxVec4 ), "Error Size type mismatch" );

            auto* cloth = physics::GetNvClothFactory()->createCloth( particles, fabric );
            cloth->setPhaseConfig( phaseConfig );

            return cloth;
        }

		void DestroyFabric(nv::cloth::Fabric*& fabric)
		{
			RED_SCOPE_LOCK(physics_natives->m_fabricMutex);
			fabric->decRefCount();
			fabric = nullptr;
		}

        Bool NeedsVersionUpdate(red::BlobView clothData)
        {
            return CheckVersion(clothData) == false;
        }

        Bool Update(DataBuffer& clothData)
        {
            return UpdateVersion(clothData);
        }

        DataBuffer CreateClothResource(CreateClothResourceParams params)
        {
            const auto _assign = [](auto& p, const auto& in )
            {
                p.mStiffness = in.mStiffness;
                p.mStiffnessMultiplier = in.mStiffnessMultiplier;
                p.mCompressionLimit = in.mCompressionLimit;
                p.mStretchLimit = in.mStretchLimit;
            };
            
            red::DynArray<Float> invMasses( params.points.Size(), red::PoolBackend() );
			std::transform(begin(params.points), end(params.points), begin(invMasses), [](const auto& p){ return p.W; });

            nv::cloth::ClothMeshDesc meshDesc;
            meshDesc.setToDefault();

            meshDesc.points.data = params.points.Data();
            meshDesc.points.stride = sizeof(decltype(params.points)::value_type);
            meshDesc.points.count = params.points.Count();

            meshDesc.triangles.data = params.indices.Data();
            meshDesc.triangles.stride = 3 * sizeof(decltype(params.indices)::value_type);
            meshDesc.triangles.count = params.indices.Size() / 3;

            meshDesc.invMasses.data = invMasses.Data();
            meshDesc.invMasses.stride = sizeof(decltype(invMasses)::value_type);
            meshDesc.invMasses.count = invMasses.Size();

            meshDesc.flags = nv::cloth::MeshFlag::e16_BIT_INDICES;
            RED_FATAL_ASSERT( meshDesc.isValid(), "Invalid desc" );

			if( params.quadifyMesh )
			{
				nv::cloth::ClothMeshQuadifier* quadifier = NvClothCreateMeshQuadifier();
				quadifier->quadify(meshDesc);
				delete quadifier;
			}	

            nv::cloth::Vector<int32_t>::Type phaseTypes;
			nv::cloth::Fabric* fabric = nullptr;
			{
				RED_SCOPE_LOCK( physics_natives->m_fabricMutex );
				fabric = NvClothCookFabricFromMesh( physics::GetNvClothFactory(), meshDesc, ToPxVec3( params.state.m_runtimeInfo.m_gravity ), &phaseTypes, params.useGeodesicTethers );
			}

            red::DynArray<nv::cloth::PhaseConfig> phases( fabric->getNumPhases(), PoolPhysics() );
            for ( Uint32 i : phases.Indices() )
            {
                phases[i].mPhaseIndex = i;
                switch(phaseTypes[i])
                {
                    case nv::cloth::ClothFabricPhaseType::eVERTICAL:   _assign(phases[i], params.state.m_verticalPhaseData);   break;
                    case nv::cloth::ClothFabricPhaseType::eHORIZONTAL: _assign(phases[i], params.state.m_horizontalPhaseData); break;
                    case nv::cloth::ClothFabricPhaseType::eBENDING:    _assign(phases[i], params.state.m_bendPhaseData);       break;
                    case nv::cloth::ClothFabricPhaseType::eSHEARING:   _assign(phases[i], params.state.m_shearPhaseData);      break;
                }
            }

            SerializeFabricContext ctx =
            {
                params.points,
                params.maxDistance,
				params.maxDistanceExt,
                params.backstopDistance,
				params.backstopRadius,
                params.selfCollision,
                {phases},
                params.state.m_runtimeInfo
            };

            return SerializeFabricToDataBuffer(fabric, ctx );
        }

        red::ArraySpan<const Float> ExtractMotionConstraints(red::BlobView view)
        {
            auto* header = view.Pointer<const FabricHeader>();
            auto* begin = view.Pointer<Float>(header->motionConstraintsOffset);
            auto* end = begin + header->numParticles;
            return red::MakeArraySpan(begin, end);
        }

		red::ArraySpan<const Float> ExtractMotionConstraintsExt(red::BlobView view)
        {
            auto* header = view.Pointer<const FabricHeader>();
            auto* begin = view.Pointer<Float>(header->motionConstraintsExtOffset);
            auto* end = begin + header->numParticles;
            return red::MakeArraySpan(begin, end);
        }
        
        red::ArraySpan<const Float> ExtractSeparationConstraints(red::BlobView view)
        {
            auto* header = view.Pointer<const FabricHeader>();
            auto* begin = view.Pointer<Float>(header->separationConstraintsOffset);
            auto* end = begin + header->numParticles;
            return red::MakeArraySpan(begin, end);
        }

		red::ArraySpan<const Float> ExtractSeparationRadii(red::BlobView view)
		{
			auto* header = view.Pointer<const FabricHeader>();
            auto* begin = view.Pointer<Float>(header->separationRadiiOffset);
            auto* end = begin + header->numParticles;
            return red::MakeArraySpan(begin, end);
		}

        red::ArraySpan<const Vector4> ExtractRestPositions(red::BlobView view)
        {
            auto* header = view.Pointer<const FabricHeader>();
            auto* begin = view.Pointer<Vector4>(header->particlesOffset);
            auto* end = begin + header->numParticles;
            return red::MakeArraySpan(begin, end);
        }

        red::ArraySpan<const Uint32> ExtractSelfCollisionIndices(red::BlobView view)
        {
            auto* header = view.Pointer<const FabricHeader>();
            auto* begin = view.Pointer<Uint32>(header->selfCollisionIndicesOffset);
            return red::MakeArraySpan(begin, header->numSelfCollisionIndices);
        }

	    RuntimeInfo ExtractDefaultRuntimeInfo(red::BlobView view)
        {
            RuntimeInfo result;
            auto* header = view.Pointer<const FabricHeader>();
            result = view.As<const RuntimeInfo>(header->runtimeInfoOffset);
            return result;
        }

		red::ArraySpan<const Uint32> ExtractTriangles(red::BlobView view)
		{
			auto* header = view.Pointer<const FabricHeader>();
            const auto* begin = view.Pointer<const Uint32>(header->trianglesOffset);
            const auto* end = begin + header->numTriangles*3;
            return red::MakeArraySpan(begin, end);
		}

		Bool Validate_NoDuplicateParticles(const red::memory::Pool& pool, red::BlobView view)
		{
			if( view.Empty() )
				return false;

			auto restPositions = ExtractRestPositions(view);

			nv::cloth::Fabric* fabric = CreateFabricFromView(view);
			if( !fabric )
				return false;

			nv::cloth::Cloth* cloth = CreateClothFromFabricAndView(view, *fabric);
			if( !cloth )
				return false;

			struct vtx
			{
				Vector4 v;

				bool operator<(const vtx& _v) const
				{
					return v < _v.v;
				}

				bool operator==(const vtx& _v) const
				{
					return v == _v.v;
				}
			};

			red::Map<vtx, Uint32> vtxMap(pool);
			for(Uint32 i = 0; i < restPositions.Size(); ++i)
			{
				vtx v = {restPositions[i]};
				if( vtxMap.KeyExist(v) )
					return false;

				vtxMap.Insert(v, i);
			}

			return true;
		}

		Bool Validate_NoZeroRestValues(const red::memory::Pool& pool, red::BlobView view)
		{
			nv::cloth::Fabric* fabric = CreateFabricFromView(view);
			if(fabric == nullptr)
				return false;

			const uint32_t nbRestValues = fabric->getNumRestvalues();
			red::DynArray<Float> restValues(nbRestValues, pool);

			auto& factory = fabric->getFactory();
			factory.extractFabricData(*fabric, {}, {}, MakeRange<Float>(restValues), {}, {}, {}, {}, {});

			return std::find_if(begin(restValues), end(restValues), [](Float f){ return f == 0.f; }) == end(restValues);
		}

    #else
        Bool NeedsVersionUpdate(red::BlobView clothData)
        {
            return false;
        }

        DataBuffer CreateClothResource(CreateClothResourceParams)
        {
            return DataBuffer(0);
        }
        
        nv::cloth::Fabric* CreateFabricFromView(red::BlobView)
        {
            return nullptr;
        }
        
        nv::cloth::Cloth* CreateClothFromFabricAndView(red::BlobView,nv::cloth::Fabric&)
        {
            return nullptr;
        }
        
        red::ArraySpan<const Float> ExtractMotionConstraints(red::BlobView)
        {
            return {};
        }

        red::ArraySpan<const Float> ExtractSeparationConstraints(red::BlobView)
        {
            return {};
        }

        red::ArraySpan<const Uint32> ExtractSelfCollisionIndices(red::BlobView view)
        {
            return {};
        }

        red::ArraySpan<const Vector4> ExtractRestPositions(red::BlobView)
        {
            return {};
        }

        red::ArraySpan<const ExportedCapsule> ExtractCollisionCapsules(red::BlobView view)
        {
            return {};
        }

		red::ArraySpan<const Uint32> ExtractTriangles(red::BlobView view)
		{
			return {};
		}

	    RuntimeInfo ExtractDefaultRuntimeInfo(red::BlobView)
        {
            return RuntimeInfo();
        }

		Bool Validate_NoDuplicateParticles(red::BlobView view)
		{
			return true;
		}

		Bool Validate_NoZeroLengthTethers(red::BlobView view)
		{
			return true;
		}
    #endif
    }
}

namespace physics
{
physx::PxMaterial* ragdoll::GetMaterial()
{
	return physics_natives->m_ragdollMaterial.Get();
}
}

#ifdef PHYSICS_DEBUG
#pragma optimize("", on)
#endif
