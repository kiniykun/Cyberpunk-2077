#pragma once


#if defined( RED_PLATFORM_WINPC )
	// Decide which implementation is used on Windows
#	define NET_USE_ENET
#	define NET_USE_GALAXY
#   define NET_USE_CLOUD
#	if !defined( RED_CONFIGURATION_FINAL ) // Let's keep at least compiling Steam implementation
//#		define NET_USE_STEAM
#	endif

#elif defined( RED_PLATFORM_DURANGO )
	// Decide which implementation is used on Durango
#	define NET_USE_RAKNET
#	define NET_USE_GALAXY
#   define NET_USE_CLOUD

#elif defined( RED_PLATFORM_ORBIS )
	// Decide which implementation is used on Orbis
#	define NET_USE_RAKNET
#	define NET_USE_GALAXY
#   define NET_USE_CLOUD

#elif defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )
	// Decide which implementation is used on Linux
#	define NET_USE_ENET
#	define NET_USE_GALAXY
#   define NET_USE_CLOUD
#else
#	error Unsupported platform
#endif
