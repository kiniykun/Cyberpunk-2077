/**
* Copyright (c) 2018 CDProjekt Red, Inc. All Rights Reserved.
*/

#include "build.h"
#include "appPlatform.h"

#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

namespace app
{
	namespace platform
	{

		namespace helper
		{
			// get the BASE DIRECTORY of the application
			// it gets the path to a directory one level above the "bin/"
			// for an installed game it's the root of the whole thing
			static red::AbsolutePath GetBaseDirectory()
			{
#if defined(RED_PLATFORM_WEB)
                return red::AbsolutePath::CreateDirPath("/game/");
#else
				// this is a symlink to the current executble
				const char* procLink = "/proc/self/exe";

				AnsiChar moduleFullPath[ PATH_MAX ];
				// readlink does not null terminate
				memset( moduleFullPath, 0, sizeof( moduleFullPath ) ); 

				Int32 success = readlink( procLink, moduleFullPath, PATH_MAX );
				RED_FATAL_ASSERT( success > 0, "readlink() failed to read %s", procLink );

				// Strip EXE name
				AnsiChar* pEnd = red::StrchrR( moduleFullPath, '/' );
				if ( !pEnd )
					return red::AbsolutePath();
				*pEnd = '\0';

				// Strip the binary subfolder - x86/x64/etc...
				pEnd = red::Strstr( moduleFullPath, "bin/" );
				if ( !pEnd )
					return red::AbsolutePath();
				*pEnd = '\0';

				// returns the directory path above the bin
				return red::AbsolutePath::CreateDirPath( red::String( moduleFullPath ) );
#endif
			}

			// get the USER DIRECTORY for the game
			// this is the place for saves
			static red::AbsolutePath GetUserDirectory()
			{
#if defined(RED_PLATFORM_WEB)
                return red::AbsolutePath::CreateDirPath("/userdata/");
#else
				AnsiChar homeDir[ PATH_MAX ];
				const AnsiChar* homeEnv = getenv( "HOME" );

				if ( homeEnv != nullptr )
				{
					red::Strcpy( homeDir, homeEnv, PATH_MAX );
				}
				else
				{
					struct passwd pwd;
					struct passwd* result;
					AnsiChar buffer[16384];
					Int32 success = getpwuid_r( getuid(), &pwd, buffer, sizeof(buffer), &result );
					if ( success != 0 )
					{
						RED_WARNING( false, "getpwuid_r() failed to get user directories" );
						return red::AbsolutePath();
					}
					red::Strcpy( homeDir, pwd.pw_dir, PATH_MAX );
				}
				// Set user data path
				return red::AbsolutePath::CreateDirPath( red::String( homeDir ) );
#endif
			}

		} // helper

		void StartEnableHDR()
		{
			// not supported
		}

		Bool FinishEnableHDR()
		{
			// not supported
			return false;
		}

		void PlatformGetRootDirectory( red::AbsolutePath& outPath )
		{
			outPath = helper::GetBaseDirectory();
		}

		Bool PlatformIsPathOnSsdDrive( const red::AbsolutePath &path )
		{
			// #TODO: Implement this
			return false;
		}

		Bool PlatformPreInit( const InitializationSettings& config )
		{
			return true;
		}

		Bool PlatformPostConfigInit( const InitializationSettings& config )
		{
			return true;
		}

		Bool PlatformPostInit( const InitializationSettings& config )
		{
			return true;
		}

		void PlatformShutdown()
		{

		}

		Bool PlatformPumpMessages()
		{
			return true;
		}

		void PlatformGetUserDirectory( red::AbsolutePath& outpath )
		{
			outpath = helper::GetUserDirectory();
		}

		void PlatformGetSaveDirectory( red::AbsolutePath& outpath, const red::StringView gameName )
		{
			outpath = helper::GetUserDirectory();
			outpath.AppendDirPath( "cdprojektred/savegames" );
			outpath.AppendDirPath( gameName );
		}

		void PlatformGetScreenshotsDirectory( red::AbsolutePath& outpath, const red::StringView gameName )
		{
			outpath = helper::GetUserDirectory();
			outpath.AppendDirPath( "cdprojektred/screenshots" );
			outpath.AppendDirPath( gameName );
		}

	} // prv

} // app
