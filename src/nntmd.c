/* nntmd – multiplexer daemon for nntm
 * gcc -Wall -O2 -o nntmd nntmd.c
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_CLIENTS 64
#define BUF_SIZE 512
#define DEF_SOCK "/tmp/nntm-stream"

typedef struct
{
      int fd;
      bool is_reader;
      bool active;
} Client;

static Client clients[ MAX_CLIENTS ];
static int srv_fd         = -1;
static bool verbose       = false;
static size_t num_readers = 0;
static size_t num_writers = 0;

/* ───────────────────────── helpers ───────────────────────── */

#define V( fmt, ... )                                                          \
      do                                                                       \
      {                                                                        \
            if ( verbose )                                                     \
                  fprintf( stderr, fmt, ##__VA_ARGS__ );                       \
      } while ( 0 )

static void bump_counts( int delta_reader, int delta_writer )
{
      num_readers += delta_reader;
      num_writers += delta_writer;
      V( "   ↻ readers=%zu writers=%zu\n", num_readers, num_writers );
}

static void cleanup( void )
{
      if ( srv_fd != -1 )
            close( srv_fd );
      unlink( DEF_SOCK );
}

static void die( const char *msg )
{
      perror( msg );
      cleanup();
      _exit( 1 );
}

/* ─────────────────────── client table ────────────────────── */

static int add_client( int fd, bool is_reader )
{
      for ( int i = 0; i < MAX_CLIENTS; ++i )
            if ( !clients[ i ].active )
            {
                  clients[ i ] = (Client){
                      .fd = fd, .is_reader = is_reader, .active = true };
                  bump_counts( is_reader, !is_reader );
                  V( "cli#%d ⇒ registered as %s\n", fd,
                     is_reader ? "READER" : "WRITER" );
                  return i;
            }
      return -1;
}

static void drop_client( int idx )
{
      if ( !clients[ idx ].active )
            return;
      V( "cli#%d ⌁ hang-up\n", clients[ idx ].fd );

      bump_counts( clients[ idx ].is_reader ? -1 : 0,
                   clients[ idx ].is_reader ? 0 : -1 );

      close( clients[ idx ].fd );
      clients[ idx ] = (Client){ 0 };
}

static void broadcast( const char *buf, size_t len )
{
      size_t fanout = 0;
      for ( int i = 0; i < MAX_CLIENTS; ++i )
            if ( clients[ i ].active && clients[ i ].is_reader )
            {
                  if ( write( clients[ i ].fd, buf, len ) != (ssize_t)len )
                        drop_client( i ); /* broken pipe */
                  else
                        ++fanout;
            }

      V( "    → delivered %zu bytes to %zu reader(s)\n", len, fanout );
}

/* ─────────────────────────── main ────────────────────────── */

int main( int argc, char **argv )
{
      const char *sock_path = DEF_SOCK;
      for ( int i = 1; i < argc; ++i )
            if ( !strcmp( argv[ i ], "-v" ) ||
                 !strcmp( argv[ i ], "--verbose" ) )
                  verbose = true;
            else if ( !strcmp( argv[ i ], "-p" ) && i + 1 < argc )
                  sock_path = argv[ ++i ];
            else
            {
                  fprintf( stderr, "usage: %s [-v] [-p <sock>]\n", argv[ 0 ] );
                  return 1;
            }

      signal( SIGINT, (void ( * )( int ))cleanup );
      signal( SIGTERM, (void ( * )( int ))cleanup );
      atexit( cleanup );

      unlink( sock_path );
      srv_fd = socket( AF_UNIX, SOCK_STREAM, 0 );
      if ( srv_fd == -1 )
            die( "socket" );

      struct sockaddr_un sa = { .sun_family = AF_UNIX };
      strncpy( sa.sun_path, sock_path, sizeof( sa.sun_path ) - 1 );
      if ( bind( srv_fd, (struct sockaddr *)&sa, sizeof( sa ) ) )
            die( "bind" );
      if ( listen( srv_fd, 16 ) )
            die( "listen" );

      V( "srv: listening on %s\n", sock_path );

      char buf[ BUF_SIZE ];
      for ( ;; )
      {
            struct pollfd p[ MAX_CLIENTS + 1 ];
            int nf = 0;

            p[ nf++ ] = (struct pollfd){ srv_fd, POLLIN };

            for ( int i = 0; i < MAX_CLIENTS; ++i )
                  if ( clients[ i ].active )
                        p[ nf++ ] = (struct pollfd){ clients[ i ].fd, POLLIN };

            if ( poll( p, nf, -1 ) < 0 )
            {
                  if ( errno == EINTR )
                        continue;
                  die( "poll" );
            }

            /* ─── new connection ─── */
            if ( p[ 0 ].revents & POLLIN )
            {
                  int cfd = accept( srv_fd, NULL, NULL );
                  if ( cfd == -1 )
                  {
                        perror( "accept" );
                        continue;
                  }

                  /* peek first 8 bytes to check for handshake */
                  char hdr[ 8 ] = { 0 };
                  ssize_t n     = recv( cfd, hdr, sizeof( hdr ) - 1, MSG_PEEK );

                  bool is_reader = false;
                  if ( n >= 7 && !memcmp( hdr, "READER\n", 7 ) )
                  {
                        /* consume handshake line */
                        read( cfd, hdr, 7 );
                        write( cfd, "OK\n", 3 );
                        is_reader = true;
                  }
                  else if ( n >= 7 && !memcmp( hdr, "WRITER\n", 7 ) )
                  {
                        read( cfd, hdr, 7 ); /* eat it */
                        write( cfd, "OK\n", 3 );
                        is_reader = false;
                  }

                  if ( add_client( cfd, is_reader ) == -1 )
                  {
                        V( "srv: max clients reached\n" );
                        close( cfd );
                  }
                  /* if it’s NOT a reader, leave the socket in the table as
                     writer; any data waiting in the buffer will be picked up in
                     next loop */
                  continue;
            }

            /* ─── existing sockets ─── */
            int idx = 1;
            for ( int i = 0; i < MAX_CLIENTS; ++i )
            {
                  if ( !clients[ i ].active )
                        continue;

                  if ( p[ idx ].revents & POLLIN )
                  {
                        ssize_t n = read( clients[ i ].fd, buf, sizeof( buf ) );
                        if ( n <= 0 )
                        { /* EOF / error */
                              drop_client( i );
                        }
                        else if ( !clients[ i ].is_reader )
                        { /* writer data */
                              V( "cli#%d → %zd bytes\n", clients[ i ].fd, n );
                              broadcast( buf, (size_t)n );
                        }
                  }
                  ++idx;
            }
      }
}
