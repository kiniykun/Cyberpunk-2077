#pragma once

#if defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_DURANGO )
	#include <winsock2.h>
	#include <ws2tcpip.h>
#elif defined( RED_PLATFORM_ORBIS )
	// System Includes
	#include <net.h>
	#define closesocket close

	#pragma comment( lib, "libSceNet_stub_weak.a" )
	#pragma comment( lib, "libScePosix_stub_weak.a" )

#elif defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )
	// System Includes
#	include <sys/socket.h>
#	include <arpa/inet.h>
#	include <endian.h>    // __BYTE_ORDER __LITTLE_ENDIAN
#	include <byteswap.h>  // bswap_64()

#	ifndef __LITTLE_ENDIAN
#		error Big endian machines are not supported!
#	endif
#endif

namespace net
{

#if defined( RED_PLATFORM_WINPC ) || defined ( RED_PLATFORM_DURANGO )

	typedef SOCKET SocketId;
	typedef sockaddr_in	SockaddrIpv4;
	typedef sockaddr Sockaddr;
	typedef ADDRESS_FAMILY AddressFamily;
	typedef int	ProtocolFamily;
	typedef socklen_t SockaddrLen;
	typedef int	SocketErrorCode;

	#define RED_INADDR_ANY INADDR_ANY

	#define RED_AF_INET AF_INET
	#define RED_AF_INET6 AF_INET6

	#define RED_SOCK_DGRAM SOCK_DGRAM
	#define RED_IPPROTO_UDP IPPROTO_UDP

	#define RED_INADDR_BROADCAST INADDR_BROADCAST

	RED_INLINE SocketId Socket( ProtocolFamily family, Int32 type, Int32 protocol )
	{
		return socket( family, type, protocol );
	}

	RED_INLINE Bool SetBroadcast( SocketId socketDescriptor )
	{
		DWORD broadcast = 1;
		return setsockopt( socketDescriptor, SOL_SOCKET, SO_BROADCAST, reinterpret_cast< const char * >( &broadcast ), sizeof( broadcast ) ) != SOCKET_ERROR;
	}
	
	RED_INLINE Bool SetNonBlocking( SocketId socketDescriptor )
	{
		DWORD on = 1;
		return ioctlsocket( socketDescriptor, FIONBIO, &on ) != SOCKET_ERROR;
	}

	RED_INLINE Bool SetPortReused( SocketId socketDescriptor )
	{
		DWORD on = 1;
		return setsockopt( socketDescriptor, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast< char * >( &on ), sizeof(on)) != SOCKET_ERROR;
	}

	RED_INLINE Bool Close( SocketId socketDescriptor )
	{
		return closesocket( socketDescriptor ) == 0;
	}

	RED_INLINE Bool Bind( SocketId socketDescriptor, const Sockaddr* address, SockaddrLen size )
	{
		return bind( socketDescriptor, address, size ) != SOCKET_ERROR;
	}

	RED_INLINE Int32 SendTo( SocketId socketDescriptor, const void* buffer, Uint32 bufferSize, const Sockaddr* address, SockaddrLen addressSize )
	{
		return sendto( socketDescriptor, static_cast< const char* >( buffer ), bufferSize, 0, address, addressSize );
	}

	RED_INLINE Int32 RecvFrom( SocketId socketDescriptor, void* buffer, Uint32 bufferSize, Sockaddr* address, SockaddrLen* addressSize )
	{
		return recvfrom( socketDescriptor, static_cast< char* >( buffer ), bufferSize, 0, address, addressSize );
	}

	RED_INLINE SocketErrorCode GetLastError()
	{
		return WSAGetLastError();
	}

	RED_INLINE red::Int32 INetPtoN( AddressFamily family, const red::AnsiChar* source, void* destination ) { return InetPtonA( family, source, destination ); }
	RED_INLINE red::Int32 INetPtoN( AddressFamily family, const red::UniChar* source, void* destination ) { return InetPtonW( family, source, destination ); }

	RED_INLINE red::Bool INetNtoP( AddressFamily family, const void* source, red::AnsiChar* destination, red::Uint32 destinationSize ) { return ( InetNtopA( family, const_cast< PVOID >( source ), destination, destinationSize ) ) ? true : false; }
	RED_INLINE red::Bool INetNtoP( AddressFamily family, const void* source, red::UniChar* destination, red::Uint32 destinationSize ) { return ( InetNtopW( family, const_cast< PVOID >( source ), destination, destinationSize ) ) ? true : false; }

	RED_INLINE Int8		NetworkToHost( const Int8 data )			{ return data; }
	RED_INLINE Int32	NetworkToHost( const Int32 data )			{ return ntohl( data ); }
	RED_INLINE Int16	NetworkToHost( const Int16 data )			{ return ntohs( data ); }
	RED_INLINE Int64	NetworkToHost( const Int64 data )
	{
#ifdef RED_ENDIAN_LITTLE
		return
			(
			( ( data & 0x00000000000000FFULL ) << 56 ) |
			( ( data & 0x000000000000FF00ULL ) << 40 ) |
			( ( data & 0x0000000000FF0000ULL ) << 24 ) |
			( ( data & 0x00000000FF000000ULL ) <<  8 ) |
			( ( data & 0x000000FF00000000ULL ) >>  8 ) |
			( ( data & 0x0000FF0000000000ULL ) >> 24 ) |
			( ( data & 0x00FF000000000000ULL ) >> 40 ) |
			( ( data & 0xFF00000000000000ULL ) >> 56 )
			);
#else
		return data;
#endif
	}

	RED_INLINE Int8		HostToNetwork( const Int8 data )			{ return data; }
	RED_INLINE Int16	HostToNetwork( const Int16 data )			{ return htons( data ); }
	RED_INLINE Int32	HostToNetwork( const Int32 data )			{ return htonl( data ); }
	RED_INLINE Int64	HostToNetwork( const Int64 data )			{ return NetworkToHost( data ); }

	RED_INLINE Uint8	NetworkToHost( const Uint8 data )			{ return data; }
	RED_INLINE Uint32	NetworkToHost( const Uint32 data )			{ return ntohl( data ); }
	RED_INLINE Uint16	NetworkToHost( const Uint16 data )			{ return ntohs( data ); }
	RED_INLINE Uint64	NetworkToHost( const Uint64 data )			{ return NetworkToHost( static_cast< Int64 >( data ) ); }

	RED_INLINE Uint8	HostToNetwork( const Uint8 data )			{ return data; }
	RED_INLINE Uint16	HostToNetwork( const Uint16 data )			{ return htons( data ); }
	RED_INLINE Uint32	HostToNetwork( const Uint32 data )			{ return htonl( data ); }
	RED_INLINE Uint64	HostToNetwork( const Uint64 data )			{ return NetworkToHost( data ); }

	RED_INLINE UniChar	NetworkToHost( const UniChar data )			{ return ntohs( data ); }
	RED_INLINE UniChar	HostToNetwork( const UniChar data )			{ return htons( data ); }

#elif defined( RED_PLATFORM_ORBIS )

	typedef int	SocketErrorCode;
	typedef SceNetId SocketId;
	typedef SceNetSockaddrIn SockaddrIpv4;
	typedef SceNetSockaddr Sockaddr;
	typedef SceNetSaFamily_t AddressFamily;
	typedef int	ProtocolFamily;
	typedef SceNetSocklen_t SockaddrLen;

	#define RED_INADDR_ANY SCE_NET_INADDR_ANY

	#define RED_AF_INET SCE_NET_AF_INET
	#define RED_AF_INET6 (SceNetSaFamily_t)-1

	#define RED_SOCK_DGRAM SCE_NET_SOCK_DGRAM
	#define RED_IPPROTO_UDP SCE_NET_IPPROTO_UDP

	#define RED_NET_MAX_ADDRSTRLEN SCE_NET_INET_ADDRSTRLEN

	#define RED_INADDR_BROADCAST SCE_NET_INADDR_BROADCAST

	RED_INLINE SocketId Socket( ProtocolFamily family, Int32 type, Int32 protocol )
	{
		return sceNetSocket( nullptr, family, type, protocol );
	}

	RED_INLINE Bool SetBroadcast( SocketId socketDescriptor )
	{
		int on = 1;
		return sceNetSetsockopt( socketDescriptor, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &on, sizeof( on ) ) == 0;
	}

	RED_INLINE Bool SetNonBlocking( SocketId socketDescriptor )
	{
		int on = 1;
		return sceNetSetsockopt( socketDescriptor, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof( int ) ) == 0;
	}

	RED_INLINE Bool SetPortReused( SocketId socketDescriptor )
	{
		int on = 1;
		return sceNetSetsockopt( socketDescriptor, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &on, sizeof(on)) == 0;
	}

	RED_INLINE Bool Close( SocketId socketDescriptor )
	{
		return sceNetSocketClose( socketDescriptor ) == 0;
	}

	RED_INLINE Bool Bind( SocketId socketDescriptor, const Sockaddr* address, SockaddrLen size )
	{
		return sceNetBind( socketDescriptor, address, size ) == 0;
	}

	RED_INLINE Int32 SendTo( SocketId socketDescriptor, const void* buffer, Uint32 bufferSize, const Sockaddr* address, SockaddrLen addressSize )
	{
		return sceNetSendto( socketDescriptor, buffer, bufferSize, 0, address, addressSize );
	}

	RED_INLINE Int32 RecvFrom( SocketId socketDescriptor, void* buffer, Uint32 bufferSize, Sockaddr* address, SockaddrLen* addressSize )
	{
		return sceNetRecvfrom( socketDescriptor, buffer, bufferSize, 0, address, addressSize );
	}

	RED_INLINE red::Int32 INetPtoN( AddressFamily family, const red::AnsiChar* source, void* destination ) { return sceNetInetPton( family, source, destination ); }
	RED_INLINE red::Int32 INetPtoN( AddressFamily family, const red::UniChar* source, void* destination )
	{
		red::AnsiChar convertedIp[ RED_NET_MAX_ADDRSTRLEN ];
		red::WideCharToStdChar( convertedIp, source, RED_NET_MAX_ADDRSTRLEN );

		return sceNetInetPton( family, convertedIp, destination );
	}

	RED_INLINE red::Bool INetNtoP( AddressFamily family, const void* source, red::AnsiChar* destination, red::Uint32 destinationSize )
	{
		return ( sceNetInetNtop( family, source, destination, destinationSize ) )? true : false;
	}

	RED_INLINE SocketErrorCode GetLastError()
	{
		return sce_net_errno;
	}

	RED_INLINE Int8		NetworkToHost( const Int8 data )			{ return data; }
	RED_INLINE Int32	NetworkToHost( const Int32 data )			{ return sceNetNtohl( data ); }
	RED_INLINE Int16	NetworkToHost( const Int16 data )			{ return sceNetNtohs( data ); }
	RED_INLINE Int64	NetworkToHost( const Int64 data )			{ return sceNetNtohll( data ); }

	RED_INLINE Int8		HostToNetwork( const Int8 data )			{ return data; }
	RED_INLINE Int16	HostToNetwork( const Int16 data )			{ return sceNetHtons( data ); }
	RED_INLINE Int32	HostToNetwork( const Int32 data )			{ return sceNetHtonl( data ); }
	RED_INLINE Int64	HostToNetwork( const Int64 data )			{ return sceNetHtonll( data ); }

	RED_INLINE Uint8	NetworkToHost( const Uint8 data )			{ return data; }
	RED_INLINE Uint32	NetworkToHost( const Uint32 data )			{ return sceNetNtohl( data ); }
	RED_INLINE Uint16	NetworkToHost( const Uint16 data )			{ return sceNetNtohs( data ); }
	RED_INLINE Uint64	NetworkToHost( const Uint64 data )			{ return sceNetNtohll( data ); }

	RED_INLINE Uint8	HostToNetwork( const Uint8 data )			{ return data; }
	RED_INLINE Uint16	HostToNetwork( const Uint16 data )			{ return sceNetHtons( data ); }
	RED_INLINE Uint32	HostToNetwork( const Uint32 data )			{ return sceNetHtonl( data ); }
	RED_INLINE Uint64	HostToNetwork( const Uint64 data )			{ return sceNetHtonll( data ); }

	RED_INLINE UniChar	NetworkToHost( const UniChar data )			{ return sceNetNtohs( data ); }
	RED_INLINE UniChar	HostToNetwork( const UniChar data )			{ return sceNetHtons( data ); }

#elif defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )

	typedef int32_t		SocketErrorCode;
	typedef int32_t		SocketId;
	typedef sockaddr_in SockaddrIpv4;
	typedef sockaddr	Sockaddr;
	typedef sa_family_t AddressFamily;
	typedef int32_t		ProtocolFamily;
	typedef socklen_t	SockaddrLen;

	#define RED_INADDR_ANY			INADDR_ANY

	#define RED_AF_INET				AF_INET
	#define RED_AF_INET6			AF_INET6

	#define RED_SOCK_DGRAM			SOCK_DGRAM
	#define RED_IPPROTO_UDP			IPPROTO_UDP

	#define RED_NET_MAX_ADDRSTRLEN	INET6_ADDRSTRLEN

	#define RED_INADDR_BROADCAST	INADDR_BROADCAST

	const int c_SocketError = -1;

	RED_INLINE SocketId Socket( ProtocolFamily family, Int32 type, Int32 protocol )
	{
		return socket( family, type, protocol );
	}

	RED_INLINE Bool SetBroadcast( SocketId socketDescriptor )
	{
		uint32_t on = 1;
		return setsockopt( socketDescriptor, SOL_SOCKET, SO_BROADCAST, &on, sizeof( on ) ) != c_SocketError;
	}
	
	RED_INLINE Bool SetNonBlocking( SocketId socketDescriptor )
	{
		int flags = fcntl( socketDescriptor, F_GETFL, 0 );
		if ( flags < 0 )
			return false;
	
		flags = ( flags | O_NONBLOCK );
		int result = fcntl( socketDescriptor, F_SETFL, flags );
		return result > c_SocketError;
	}
	
	RED_INLINE Bool SetPortReused( SocketId socketDescriptor )
	{
		uint32_t on = 1;
		return setsockopt( socketDescriptor, SOL_SOCKET, SO_REUSEADDR, &on, sizeof( on ) ) != c_SocketError;
	}
	
	RED_INLINE Bool Close( SocketId socketDescriptor )
	{
		return close( socketDescriptor ) != c_SocketError;
	}
	
	RED_INLINE Bool Bind( SocketId socketDescriptor, const Sockaddr* address, SockaddrLen size )
	{
		return bind( socketDescriptor, address, size ) != c_SocketError;
	}
	
	RED_INLINE Int32 SendTo( SocketId socketDescriptor, const void* buffer, Uint32 bufferSize, const Sockaddr* address, SockaddrLen addressSize )
	{
		return sendto( socketDescriptor, static_cast< const char* >( buffer ), bufferSize, 0, address, addressSize );
	}
	
	RED_INLINE Int32 RecvFrom( SocketId socketDescriptor, void* buffer, Uint32 bufferSize, Sockaddr* address, SockaddrLen* addressSize )
	{
		return recvfrom( socketDescriptor, static_cast< char* >( buffer ), bufferSize, 0, address, addressSize );
	}
	
	RED_INLINE red::Int32 INetPtoN( AddressFamily family, const red::AnsiChar* source, void* destination ) { return inet_pton( family, source, destination ); }
	RED_INLINE red::Int32 INetPtoN( AddressFamily family, const red::UniChar* source, void* destination )
	{
		red::AnsiChar convertedIp[RED_NET_MAX_ADDRSTRLEN];
		red::WideCharToStdChar( convertedIp, source, RED_NET_MAX_ADDRSTRLEN );
	
		return inet_pton( family, convertedIp, destination );
	}
	
	RED_INLINE red::Bool INetNtoP( AddressFamily family, const void* source, red::AnsiChar* destination, red::Uint32 destinationSize )
	{
		return inet_ntop( family,  source, destination, destinationSize ) != nullptr;
	}
	
	RED_INLINE SocketErrorCode GetLastError()
	{
		return errno;
	}
	
	RED_INLINE Int8		NetworkToHost( const Int8 data )    { return data; }
	RED_INLINE Int16	NetworkToHost( const Int16 data )   { return bswap_16( data ); }
	RED_INLINE Int32	NetworkToHost( const Int32 data )   { return bswap_32( data ); }
	
	RED_INLINE Int64	NetworkToHost( const Int64 data )   { return bswap_64( data ); }
		
	RED_INLINE Int8		HostToNetwork( const Int8 data )    { return data; }
	RED_INLINE Int16	HostToNetwork( const Int16 data )   { return bswap_16( data ); }
	RED_INLINE Int32	HostToNetwork( const Int32 data )   { return bswap_32( data ); }
	RED_INLINE Int64	HostToNetwork( const Int64 data )   { return bswap_64( data ); }
	
	RED_INLINE Uint8	NetworkToHost( const Uint8 data )   { return data; }
	RED_INLINE Uint16	NetworkToHost( const Uint16 data )  { return bswap_16( data ); }
	RED_INLINE Uint32	NetworkToHost( const Uint32 data )  { return bswap_32( data ); }
	RED_INLINE Uint64	NetworkToHost( const Uint64 data )  { return bswap_64( data ); }
	
	RED_INLINE Uint8	HostToNetwork( const Uint8 data )   { return data; }
	RED_INLINE Uint16	HostToNetwork( const Uint16 data )  { return bswap_16( data ); }
	RED_INLINE Uint32	HostToNetwork( const Uint32 data )  { return bswap_32( data ); }
	RED_INLINE Uint64	HostToNetwork( const Uint64 data )  { return bswap_64( data ); }
	
	RED_INLINE UniChar	NetworkToHost( const UniChar data ) { return bswap_16( data ); }
	RED_INLINE UniChar	HostToNetwork( const UniChar data ) { return bswap_16( data ); }

#else
#	error Unsupported platform!
#endif

}
