/* rpc.c - see rpc.h */
#include <3ds.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>

#include "rpc.h"
#include "mapv.h"
#include "render.h"

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000
#define RPC_PORT       5151
#define LOG_BUF_SIZE   4096

#define SCAN_ROOT "sdmc:/3ds" /* keep in sync with main.c */

static u32 *soc_buffer = NULL;
static int listen_sock = -1;

static u32 injected_keys = 0;

static char log_buf[LOG_BUF_SIZE];
static size_t log_len = 0;


void
rpc_logf( const char *fmt, ... )
{
	va_list ap;
	int n;

	va_start( ap, fmt );
	vprintf( fmt, ap );
	va_end( ap );

	if (log_len >= LOG_BUF_SIZE - 1)
		return;

	va_start( ap, fmt );
	n = vsnprintf( log_buf + log_len, LOG_BUF_SIZE - log_len, fmt, ap );
	va_end( ap );

	if (n > 0)
		log_len += (size_t)n < (LOG_BUF_SIZE - log_len) ? (size_t)n : (LOG_BUF_SIZE - log_len - 1);
}


void
rpc_init( void )
{
	struct sockaddr_in server;

	soc_buffer = (u32 *)memalign( SOC_ALIGN, SOC_BUFFERSIZE );
	if (soc_buffer == NULL)
		return;

	if (socInit( soc_buffer, SOC_BUFFERSIZE ) != 0)
		return;

	listen_sock = socket( AF_INET, SOCK_STREAM, IPPROTO_IP );
	if (listen_sock < 0)
		return;

	memset( &server, 0, sizeof(server) );
	server.sin_family = AF_INET;
	server.sin_port = htons( RPC_PORT );
	server.sin_addr.s_addr = gethostid( );

	if (bind( listen_sock, (struct sockaddr *)&server, sizeof(server) ) < 0) {
		close( listen_sock );
		listen_sock = -1;
		return;
	}

	fcntl( listen_sock, F_SETFL, fcntl( listen_sock, F_GETFL, 0 ) | O_NONBLOCK );
	listen( listen_sock, 1 );

	rpc_logf( "rpc: listening on %s:%d\n", inet_ntoa( server.sin_addr ), RPC_PORT );
}


void
rpc_fini( void )
{
	if (listen_sock >= 0) {
		close( listen_sock );
		listen_sock = -1;
	}
	socExit( );
	if (soc_buffer != NULL) {
		free( soc_buffer );
		soc_buffer = NULL;
	}
}


u32
rpc_take_injected_keys( void )
{
	u32 k = injected_keys;
	injected_keys = 0;
	return k;
}


/* Reads one '\n'-terminated command line (dropping the newline and any
 * trailing '\r'). Returns line length, or -1 on error/timeout.
 *
 * devkitARM's SOC service doesn't support SO_RCVTIMEO, so the timeout
 * (bounding how long a stuck/slow client can stall the render loop) is
 * done by hand with select() before each byte. */
static int
recv_line( int csock, char *buf, int bufsize )
{
	int total = 0;
	int n;

	while (total < bufsize - 1) {
		fd_set rfds;
		struct timeval tv;

		FD_ZERO( &rfds );
		FD_SET( csock, &rfds );
		tv.tv_sec = 3;
		tv.tv_usec = 0;

		if (select( csock + 1, &rfds, NULL, NULL, &tv ) <= 0)
			return -1; /* timeout or error */

		n = recv( csock, buf + total, 1, 0 );
		if (n <= 0)
			return -1;
		if (buf[total] == '\n')
			break;
		++total;
	}
	buf[total] = '\0';
	if (total > 0 && buf[total - 1] == '\r')
		buf[total - 1] = '\0';

	return total;
}


static u32
key_from_name( const char *name )
{
	if (!strcmp( name, "A" )) return KEY_A;
	if (!strcmp( name, "B" )) return KEY_B;
	if (!strcmp( name, "X" )) return KEY_X;
	if (!strcmp( name, "Y" )) return KEY_Y;
	if (!strcmp( name, "L" )) return KEY_L;
	if (!strcmp( name, "R" )) return KEY_R;
	if (!strcmp( name, "START" )) return KEY_START;
	if (!strcmp( name, "SELECT" )) return KEY_SELECT;
	if (!strcmp( name, "UP" )) return KEY_DUP;
	if (!strcmp( name, "DOWN" )) return KEY_DDOWN;
	if (!strcmp( name, "LEFT" )) return KEY_DLEFT;
	if (!strcmp( name, "RIGHT" )) return KEY_DRIGHT;

	return 0;
}


static void
handle_command( int csock, char *line )
{
	char *args = strchr( line, ' ' );

	if (args != NULL) {
		*args = '\0';
		++args;
	}

	if (!strcmp( line, "PING" )) {
		send( csock, "PONG\n", 5, 0 );
	}
	else if (!strcmp( line, "SCAN" )) {
		char reply[128];
		int len;

		mapv_scan_and_build( SCAN_ROOT );
		render_frame( ); /* so an immediate SHOT reflects the new scene */

		len = snprintf( reply, sizeof(reply), "OK vertices=%u\n", mapv_vertex_count( ) );
		send( csock, reply, len, 0 );
	}
	else if (!strcmp( line, "SHOT" )) {
		static unsigned char rgb[400 * 240 * 3];
		char header[32];
		int hlen;

		render_capture_rgb( rgb );
		hlen = snprintf( header, sizeof(header), "PPM %d\n", 15 + (int)sizeof(rgb) );
		send( csock, header, hlen, 0 );
		send( csock, "P6\n400 240\n255\n", 15, 0 );
		send( csock, rgb, sizeof(rgb), 0 );
	}
	else if (!strcmp( line, "LOG" )) {
		char header[32];
		int hlen;

		hlen = snprintf( header, sizeof(header), "LOG %d\n", (int)log_len );
		send( csock, header, hlen, 0 );
		if (log_len > 0)
			send( csock, log_buf, log_len, 0 );
	}
	else if (!strcmp( line, "KEY" ) && args != NULL) {
		u32 k = key_from_name( args );

		if (k != 0) {
			injected_keys |= k;
			send( csock, "OK\n", 3, 0 );
		}
		else
			send( csock, "ERR unknown key\n", 16, 0 );
	}
	else {
		send( csock, "ERR unknown command\n", 21, 0 );
	}
}


void
rpc_poll( void )
{
	struct sockaddr_in client;
	socklen_t clientlen = sizeof(client);
	int csock;
	char line[256];

	if (listen_sock < 0)
		return;

	csock = accept( listen_sock, (struct sockaddr *)&client, &clientlen );
	if (csock < 0)
		return; /* EAGAIN: nobody waiting, normal case */

	/* Accepted sockets don't reliably inherit the listener's O_NONBLOCK;
	 * make sure it's blocking so send() below can't return a short
	 * write. select() in recv_line() is what bounds our wait time. */
	fcntl( csock, F_SETFL, fcntl( csock, F_GETFL, 0 ) & ~O_NONBLOCK );

	if (recv_line( csock, line, sizeof(line) ) >= 0)
		handle_command( csock, line );

	close( csock );
}

/* end rpc.c */
