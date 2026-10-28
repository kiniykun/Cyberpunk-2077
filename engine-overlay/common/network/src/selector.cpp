#include "build.h"
#include "selector.h"

namespace net
{

#if defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_DURANGO ) || defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )

Selector::Selector()
	: m_socket( -1 )
	, m_timeoutSeconds( 0 )
	, m_timeoutMicroseconds( 0 )
{
}

Selector::~Selector()
{
	Close();
}

Status Selector::Init( SocketId socket, Int32 timeoutSeconds, Int32 timeoutMicroseconds )
{
	m_socket = socket;
	m_timeoutSeconds = timeoutSeconds;
	m_timeoutMicroseconds = timeoutMicroseconds;

	return Status( ErrorCode::Ok );
}

void Selector::Close()
{
}

int Selector::Select()
{
	fd_set receiveSet;
	FD_ZERO( &receiveSet );
	FD_SET( m_socket, &receiveSet );

	timeval timeout;
	timeout.tv_sec = m_timeoutSeconds;
	timeout.tv_usec = m_timeoutMicroseconds;

	int selected = select( static_cast< int >( m_socket ) + 1, &receiveSet, nullptr, nullptr, &timeout );
	if ( selected == -1 )
	{
		RED_LOG_ERROR( "Selector: Selector error on read, code = %d", GetLastError() );
		return -1;
	}

	if ( selected == 0 )
	{
		return 0;
	}

	return FD_ISSET( m_socket, &receiveSet ) ? 1 : 0;
}

#elif defined( RED_PLATFORM_ORBIS )

Selector::Selector()
	: m_socket( -1 )
	, m_timeoutSeconds( 0 )
	, m_timeoutMicroseconds( 0 )
	, m_epollId( -1 )
{
	red::Memset( &m_event, 0, sizeof( m_event ) );
	red::Memset( &m_events, 0, sizeof( m_events ) );
}

Selector::~Selector()
{
	Close();
}

Status Selector::Init( SocketId socket, Int32 timeoutSeconds, Int32 timeoutMicroseconds )
{
	m_socket = socket;
	m_timeoutSeconds = timeoutSeconds;
	m_timeoutMicroseconds = timeoutMicroseconds;

	m_epollId = sceNetEpollCreate( "Selector", 0 );
	if ( m_epollId < 0 )
	{
		RED_LOG_ERROR( "Selector: Failed to create epoll selector, code = %d", GetLastError() );
		return Status( ErrorCode::NativeError, GetLastError() );
	}

	m_event.data.u32 = m_socket;
	m_event.events = SCE_NET_EPOLLIN;

	if ( sceNetEpollControl( m_epollId, SCE_NET_EPOLL_CTL_ADD, m_socket, &m_event ) != 0 )
	{
		RED_LOG_ERROR( "Selector: Failed to configure epoll selector for read events, code = %d", GetLastError() );
		return Status( ErrorCode::NativeError, GetLastError() );
	}

	return Status( ErrorCode::Ok );
}

void Selector::Close()
{
	if ( m_epollId != -1 )
	{
		sceNetEpollDestroy( m_epollId );
		m_epollId = -1;
	}
}

int Selector::Select()
{
	int timeout = m_timeoutSeconds * 1000000 + m_timeoutMicroseconds;
	int n = sceNetEpollWait( m_epollId, m_events, MAX_EVENTS, timeout );
	if ( n < 0 )
	{
		RED_LOG_ERROR( "Selector: Failure while waiting for epoll event, code = %d", GetLastError() );
		return -1;
	}
	return n;
}

#endif

}
