/**
* Copyright (c) 2013 CDProjekt Red, Inc. All Rights Reserved.
*/

#include "build.h"
#include "appPath.h"
#include "../../redFileSystem/include/fileSys.h"
#include "../../redReflection/include/util.h"

namespace app
{
namespace paths
{
red::AbsolutePath MakeEngineDepotPath( const red::AbsolutePath& rootPath )
{
	return rootPath.AddDirPath( "depot" );
}

red::AbsolutePath MakeGameDepotPath( const red::AbsolutePath& rootPath )
{
	return rootPath.AddDirPath( "depot" );
}

red::AbsolutePath MakeEditorDepotPath( const red::AbsolutePath& rootPath )
{
	return rootPath.AddDirPath( "depot" );
}

red::AbsolutePath MakeCachePath( const red::AbsolutePath& rootPath )
{
	return rootPath.AddDirPath( "cache" );
}

red::AbsolutePath MakeConfigPath( const red::AbsolutePath& rootPath )
{
	return rootPath.AddDirPath( "config" );
}

red::String CurrentPlatformDirectory()
{
#if defined( RED_PLATFORM_WEB )
    return "pc"; // Original PC asset formats.
#elif defined( RED_PLATFORM_WINPC )
	if ( red::MultiplayerSetup::ShouldUseWindowsServerData() )
	{
		return "server_windows";
	}
	return "pc";
#elif defined( RED_PLATFORM_LINUX )
	if ( red::MultiplayerSetup::IsServer() )
	{
		return "server_linux";
	}
	return "pc_linux";
#elif defined( RED_PLATFORM_DURANGO )
	return "xbox";
#elif defined( RED_PLATFORM_ORBIS )
	return "ps4";
#else
	RED_FATAL( "Unsupported platform" );
	return "";
#endif
}

red::AbsolutePath DefaultRootPath()
{
	return red::paths::ParentAbsolutePath( GFileManager->GetGameRoot() );
}

red::AbsolutePath DefaultArchivePath()
{
	auto archivePath = DefaultRootPath().AddDirPath( "archive" );
	archivePath.AppendDirPath( CurrentPlatformDirectory() );
	return archivePath;
}

red::AbsolutePath DefaultCookedDepotPath()
{
	auto cookedPath = DefaultRootPath().AddDirPath( "temp/cooked" );
	cookedPath.AppendDirPath( CurrentPlatformDirectory() );
	cookedPath.AppendDirPath( "main" );
	return cookedPath;
}

red::AbsolutePath DefaultCookedCachePath()
{
	auto cookedPath = DefaultRootPath().AddDirPath( "temp/cooked" );
	cookedPath.AppendDirPath( CurrentPlatformDirectory() );
	return cookedPath;
}

#if defined( RED_PLATFORM_WINPC )

red::String DefaultPlatformDirectoryForPlatform( ECookingPlatform platform )
{
	switch ( platform )
	{
	case PLATFORM_PC:
		return "pc";
	case PLATFORM_XboxOne:
		return "xbox";
	case PLATFORM_PS4:
		return "ps4";
	case PLATFORM_WindowsServer:
		return "server_windows";
	case PLATFORM_LinuxServer:
		return "server_linux";

	case PLATFORM_None:
	default:
		RED_FATAL( "Invalid cooking platform to get the directory for" );
		return "";
	};
}

red::AbsolutePath DefaultArchivePathForPlatform( ECookingPlatform platform )
{
	auto archivePath = DefaultRootPath().AddDirPath( "archive" );
	archivePath.AppendDirPath( DefaultPlatformDirectoryForPlatform( platform ) );
	return archivePath;
}

red::AbsolutePath DefaultCookedDepotPathForPlatform( ECookingPlatform platform )
{
	auto cookedPath = DefaultRootPath().AddDirPath( "temp/cooked" );
	cookedPath.AppendDirPath( DefaultPlatformDirectoryForPlatform( platform ) );
	cookedPath.AppendDirPath( "main" );
	return cookedPath;
}

red::AbsolutePath DefaultCookedCachePathForPlatform( ECookingPlatform platform )
{
	auto cookedPath = DefaultRootPath().AddDirPath( "temp/cooked" );
	cookedPath.AppendDirPath( DefaultPlatformDirectoryForPlatform( platform ) );
	return cookedPath;
}

#endif

} // paths

} // app