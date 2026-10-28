#pragma once

#ifdef RED_PLATFORM_WINPC

#include "win/recordingToolCoreLoader_WinImpl.h"
namespace srt { namespace platform = win; }

#endif // RED_PLATFORM_WINPC

#ifdef RED_PLATFORM_DURANGO

// Durango can use win implementation.
#include "win/recordingToolCoreLoader_WinImpl.h"
namespace srt { namespace platform = win; }

#endif // RED_PLATFORM_DURANGO

#ifdef RED_PLATFORM_ORBIS

#include "orbis/recordingToolCoreLoader_OrbisImpl.h"
namespace srt { namespace platform = orbis; }

#endif  // RED_PLATFORM_ORBIS

#ifdef RED_PLATFORM_WEB
#include "recording-loader-web.h"
namespace srt { namespace platform = web; }
#endif

namespace srt
{
	// Loader that is just an interface to platform specific dll loaders.
	// Each platform specific loader has to implement all function below, otherwise project will
	// not compile:
	class RecordingToolCoreLoader : private platform::RecordingToolCoreLoaderImpl
	{
		typedef platform::RecordingToolCoreLoaderImpl Super;

	public:
		// Returns true if load was successful.
		Bool TryToLoadDll();

		// Unloads dll.
		void UnloadDll();

		// Returns true if dll is already loaded.
		Bool IsDllLoaded() const;

		// All functions below are just 'wrappers' to recordingToolCore dll functions.
		// They simply should call function from dll with exactly the same parameters and return types.
		// See recordingToolCore for description of those functions.
		recCore::IRecordingToolCore* Call_CreateRecordingToolCore( recCore::TAllocateFunction allocFunc, recCore::TFreeFunction freeFunc );
		void Call_ReleaseRecordingToolCore( recCore::IRecordingToolCore* recordingTool );
		void* Call_ReleaseWithMemento( recCore::IRecordingToolCore* recordingTool );
		Bool Call_RecreateRecordingToolCore( void* memento );
		// ---
	};
}

//////////////////////////////////////////////////////////////////////////
//
// INLINES:
//
//////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////
RED_INLINE Bool srt::RecordingToolCoreLoader::TryToLoadDll()
{
	return Super::TryToLoadDll();
}

//////////////////////////////////////////////////////////////
RED_INLINE void srt::RecordingToolCoreLoader::UnloadDll()
{
	Super::UnloadDll();
}

//////////////////////////////////////////////////////////////
RED_INLINE Bool srt::RecordingToolCoreLoader::IsDllLoaded() const
{
	return Super::IsDllLoaded();
}

//////////////////////////////////////////////////////////////
RED_INLINE recCore::IRecordingToolCore* srt::RecordingToolCoreLoader::Call_CreateRecordingToolCore( recCore::TAllocateFunction allocFunc, recCore::TFreeFunction freeFunc )
{
	return Super::Call_CreateRecordingToolCore( allocFunc, freeFunc );
}

//////////////////////////////////////////////////////////////
RED_INLINE void srt::RecordingToolCoreLoader::Call_ReleaseRecordingToolCore( recCore::IRecordingToolCore* recordingTool )
{
	Super::Call_ReleaseRecordingToolCore( recordingTool );
}

//////////////////////////////////////////////////////////////
RED_INLINE void* srt::RecordingToolCoreLoader::Call_ReleaseWithMemento( recCore::IRecordingToolCore* recordingTool )
{
	return Super::Call_ReleaseWithMemento( recordingTool );
}

//////////////////////////////////////////////////////////////
RED_INLINE Bool srt::RecordingToolCoreLoader::Call_RecreateRecordingToolCore( void* memento )
{
	return Super::Call_RecreateRecordingToolCore( memento );
}
