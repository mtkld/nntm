///////////// TODO: Do not forget the mutexes...

/*
 * todo‑viewer.c  – ncurses list with date / priority / text columns
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>  // for open()
#include <libgen.h> // for dirname
#include <locale.h>
#include <ncurses.h>
#include <poll.h>
#include <signal.h> // for sig_atomic_t
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h> // for fork(), execl(), _exit()

#include <pthread.h>
#include <sys/stat.h> // for fstat(), S_ISREG, S_ISFIFO, for streaming by pipe functionality
#include <sys/types.h>

#include <sys/socket.h>
#include <sys/un.h>

#define MAX_TODOS 1000
#define MAX_LINE 512
#define MAX_TYPE 32

typedef struct
{
      bool completed;
      char completion_date[ 11 ]; /* YYYY‑MM‑DD               */
      char date[ 11 ];            /* due date / log date      */
      char priority[ 4 ];         /* "(A)" .. "(Z)" or ""     */
      char type[ MAX_TYPE ];      /* @context / @project      */
      char text[ MAX_LINE ];      /* whatever is left         */
} Todo;

static Todo todos[ MAX_TODOS ];
static int todo_count = 0;

static char *types[ MAX_TODOS ];
static int type_count     = 0;
static int selected_type  = 0;
static int selected_index = 0;

static bool show_help    = false;
static int scroll_offset = 0;

static const char *todo_filename = NULL;

static bool sort_descending      = false;
static bool sort_date_descending = false;

static void save_todos_to_file( void );

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

char archive_path[ PATH_MAX ];

static const char *exec_script = NULL;

bool auto_scroll_enabled = true;

// For our streaming by pipe functionality, mutex for thread safety
bool streaming_mode        = false;
pthread_mutex_t todo_mutex = PTHREAD_MUTEX_INITIALIZER;

volatile sig_atomic_t need_redraw = 0;
int wakeup_pipe[ 2 ]; // [0] read, [1] write

static void run_exec_hook( const char *prefix, const char *text )
{
      if ( !exec_script || !text || strlen( text ) == 0 )
            return;

      pid_t pid = fork();
      if ( pid == 0 )
      {
            // In child process
            // Redirect stdout and stderr to /dev/null
            int devnull = open( "/dev/null", O_WRONLY );
            if ( devnull != -1 )
            {
                  dup2( devnull, STDOUT_FILENO );
                  dup2( devnull, STDERR_FILENO );
                  close( devnull );
            }

            char msg[ MAX_LINE * 2 ];
            snprintf( msg, sizeof( msg ), "%s%s", prefix, text );

            execl( exec_script, exec_script, msg, (char *)NULL );
            _exit( 127 ); // only reached if execl fails
      }
      // Parent continues immediately
}

static void remove_oldest_todo( void )
{
      if ( todo_count == 0 )
            return;

      // Shift all todos one step left
      for ( int i = 1; i < todo_count; ++i )
            todos[ i - 1 ] = todos[ i ];

      todo_count--;
}
static void archive_completed_todos( void )
{
      // Derive archive path
      char archive_path[ PATH_MAX ];
      strncpy( archive_path, todo_filename, sizeof( archive_path ) );
      archive_path[ sizeof( archive_path ) - 1 ] = '\0';
      char *dir                                  = dirname( archive_path );

      snprintf( archive_path, sizeof archive_path, "%s/todo.archive.txt", dir );

      FILE *f = fopen( archive_path, "a" );
      if ( !f )
      {
            perror( "archive write" );
            return;
      }

      // Write all completed todos and remove them from the list
      int write_count = 0;
      for ( int i = 0; i < todo_count; )
      {
            Todo *t = &todos[ i ];
            if ( !t->completed )
            {
                  i++;
                  continue;
            }

            fprintf( f, "x %s %s @%s %s\n", t->completion_date, t->date,
                     t->type, t->text );

            // Shift remaining todos left
            for ( int j = i; j < todo_count - 1; ++j )
                  todos[ j ] = todos[ j + 1 ];

            todo_count--;
            write_count++;
      }

      fclose( f );

      if ( !streaming_mode )
            if ( write_count > 0 )
                  save_todos_to_file();
}

static void add_new_todo( void )
{
      if ( streaming_mode )
            return;

      if ( todo_count >= MAX_TODOS )
            return;

      Todo new_todo;
      memset( &new_todo, 0, sizeof( Todo ) );

      // Set today's date
      time_t now    = time( NULL );
      struct tm *tm = localtime( &now );
      strftime( new_todo.date, sizeof new_todo.date, "%Y-%m-%d", tm );

      // Set @type from current context
      strncpy( new_todo.type, types[ selected_type ], MAX_TYPE - 1 );

      // Default to not completed
      new_todo.completed = false;

      // Prompt for text
      echo();
      curs_set( 1 );
      move( LINES - 1, 0 );
      clrtoeol();
      attron( COLOR_PAIR( 2 ) | A_BOLD );
      printw( "New todo: " );
      attroff( COLOR_PAIR( 2 ) | A_BOLD );
      getnstr( new_todo.text, MAX_LINE - 1 );
      noecho();
      curs_set( 0 );

      if ( strlen( new_todo.text ) == 0 )
            return;

      // Insert new todo right after the currently selected item
      int shown = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            if ( strcmp( todos[ i ].type, types[ selected_type ] ) != 0 )
                  continue;

            if ( shown == selected_index )
            {
                  for ( int j = todo_count; j > i + 1; --j )
                        todos[ j ] = todos[ j - 1 ];
                  todos[ i + 1 ] = new_todo;
                  todo_count++;
                  run_exec_hook( "Added: ", new_todo.text );
                  save_todos_to_file();
                  selected_index++;
                  return;
            }
            ++shown;
      }

      // fallback if no match: append at end
      todos[ todo_count++ ] = new_todo;
      save_todos_to_file();
      run_exec_hook( "Added: ", new_todo.text );
}

static void group_todos_by_completed( void )
{
      Todo grouped[ MAX_TODOS ];
      int group_count = 0;
      const char *cat = types[ selected_type ];

      // First: uncompleted
      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];
            if ( ( strcmp( cat, "all" ) == 0 || strcmp( t->type, cat ) == 0 ) &&
                 !t->completed )
                  grouped[ group_count++ ] = *t;
      }

      // Then: completed
      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];
            if ( ( strcmp( cat, "all" ) == 0 || strcmp( t->type, cat ) == 0 ) &&
                 t->completed )
                  grouped[ group_count++ ] = *t;
      }

      // Reinsert grouped section
      int j = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            if ( strcmp( cat, "all" ) == 0 ||
                 strcmp( todos[ i ].type, cat ) == 0 )
                  todos[ i ] = grouped[ j++ ];
      }
}

static int compare_date( const void *a, const void *b )
{
      const Todo *ta = (const Todo *)a;
      const Todo *tb = (const Todo *)b;

      int cmp = strncmp( ta->date, tb->date, 10 );
      return sort_date_descending ? -cmp : cmp;
}
static int compare_priority( const void *a, const void *b )
{
      const Todo *ta = (const Todo *)a;
      const Todo *tb = (const Todo *)b;

      int pa = ( ta->priority[ 0 ] == '(' ) ? ta->priority[ 1 ] : 127;
      int pb = ( tb->priority[ 0 ] == '(' ) ? tb->priority[ 1 ] : 127;

      return sort_descending ? pb - pa : pa - pb;
}

static void sort_todos_by_date( bool descending )
{
      Todo sorted[ MAX_TODOS ];
      int count = 0;

      for ( int i = 0; i < todo_count; ++i )
      {
            if ( strcmp( types[ selected_type ], "all" ) == 0 ||
                 strcmp( todos[ i ].type, types[ selected_type ] ) == 0 )
            {
                  sorted[ count++ ] = todos[ i ];
            }
      }

      sort_date_descending = descending;
      qsort( sorted, count, sizeof( Todo ), compare_date );

      int j = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            if ( strcmp( types[ selected_type ], "all" ) == 0 ||
                 strcmp( todos[ i ].type, types[ selected_type ] ) == 0 )
            {
                  todos[ i ] = sorted[ j++ ];
            }
      }
}

static void sort_todos_by_priority( bool descending )
{
      Todo sorted[ MAX_TODOS ];
      int count = 0;

      // collect matching todos
      for ( int i = 0; i < todo_count; ++i )
      {
            if ( strcmp( types[ selected_type ], "all" ) == 0 ||
                 strcmp( todos[ i ].type, types[ selected_type ] ) == 0 )
            {
                  sorted[ count++ ] = todos[ i ];
            }
      }

      sort_descending = descending;
      qsort( sorted, count, sizeof( Todo ), compare_priority );

      // reinsert sorted section
      int j = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            if ( strcmp( types[ selected_type ], "all" ) == 0 ||
                 strcmp( todos[ i ].type, types[ selected_type ] ) == 0 )
            {
                  todos[ i ] = sorted[ j++ ];
            }
      }
}

static void prompt_priority( void )
{
      int shown = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];
            if ( strcmp( types[ selected_type ], "all" ) != 0 &&
                 strcmp( t->type, types[ selected_type ] ) != 0 )
                  continue;

            if ( shown == selected_index )
            {
                  if ( t->completed )
                  {
                        mvprintw( LINES - 1, 0,
                                  "❌ Cannot set priority on completed "
                                  "item." );
                        refresh();
                        napms( 1000 ); // wait 1 second
                        move( LINES - 1, 0 );
                        clrtoeol();
                        refresh();
                        return;
                  }

                  // Prompt user
                  echo();
                  curs_set( 1 );
                  mvprintw( LINES - 1, 0,
                            "Set priority (a-z, or space to clear): " );
                  int ch = getch();
                  noecho();
                  curs_set( 0 );

                  if ( ch == ' ' || ch == KEY_BACKSPACE || ch == 127 )
                  {
                        t->priority[ 0 ] = '\0'; // clear
                  }
                  else if ( isalpha( ch ) )
                  {
                        ch = toupper( ch );
                        snprintf( t->priority, sizeof t->priority, "(%c)", ch );
                  }

                  if ( !streaming_mode )
                        save_todos_to_file();

                  move( LINES - 1, 0 );
                  clrtoeol();
                  refresh();
                  return;
            }

            ++shown;
      }
}

static void add_type( const char *type )
{
      for ( int i = 0; i < type_count; ++i )
            if ( strcmp( types[ i ], type ) == 0 )
                  return;
      types[ type_count++ ] = strdup( type );
}

static int count_visible_items_for_type( const char *type )
{
      if ( strcmp( type, "all" ) == 0 )
            return todo_count;

      int n = 0;
      for ( int i = 0; i < todo_count; ++i )
            if ( strcmp( todos[ i ].type, type ) == 0 )
                  ++n;
      return n;
}

static void prompt_type( void )
{
      int shown = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];
            if ( strcmp( types[ selected_type ], "all" ) != 0 &&
                 strcmp( t->type, types[ selected_type ] ) != 0 )
                  continue;

            if ( shown == selected_index )
            {
                  // Prompt for new type
                  echo();
                  curs_set( 1 );
                  char input[ MAX_TYPE ] = { 0 };
                  move( LINES - 1, 0 );
                  clrtoeol();
                  attron( COLOR_PAIR( 2 ) | A_BOLD );
                  printw( "Change type to @" );
                  attroff( COLOR_PAIR( 2 ) | A_BOLD );
                  getnstr( input, MAX_TYPE - 1 );
                  noecho();
                  curs_set( 0 );

                  if ( strlen( input ) > 0 )
                  {
                        strncpy( t->type, input, MAX_TYPE - 1 );
                        add_type( input );
                        save_todos_to_file();
                  }

                  move( LINES - 1, 0 );
                  clrtoeol();
                  refresh();
                  return;
            }

            ++shown;
      }
}
/* ─────────────────────────────────────────────── file I/O ── */

void load_todos( const char *filename )
{
      FILE *f = fopen( filename, "r" );
      if ( !f )
      {
            perror( "open" );
            exit( 1 );
      }
      // Clear current todos and types
      // In case we run it again
      todo_count = 0;
      for ( int i = 0; i < type_count; ++i )
      {
            free( types[ i ] );
      }

      type_count = 0;
      // After clearing types and todos, add the virtual type
      types[ type_count++ ] = strdup( "all" );

      char line[ MAX_LINE ];
      while ( fgets( line, sizeof( line ), f ) )
      {
            if ( todo_count >= MAX_TODOS )
                  break;

            // Trim trailing newlines
            line[ strcspn( line, "\r\n" ) ] = '\0';

            Todo *t = &todos[ todo_count ];
            memset( t, 0, sizeof( Todo ) );
            strncpy( t->text, line, MAX_LINE - 1 );
            strcpy( t->date, "2025-05-12" ); // dummy
            strcpy( t->type, "stream" );
            t->completed = false;

            const char *p = line;

            // 1. check if line starts with "x " (completed)
            if ( strncmp( p, "x ", 2 ) == 0 )
            {
                  t->completed = true;
                  p += 2;
                  sscanf( p, "%10s", t->completion_date );
                  p += strlen( t->completion_date );
                  while ( isspace( (unsigned char)*p ) )
                        p++;
            }

            // 2. check for priority first (before date)
            if ( p[ 0 ] == '(' && isalpha( (unsigned char)p[ 1 ] ) &&
                 p[ 2 ] == ')' )
            {
                  strncpy( t->priority, p, 3 );
                  t->priority[ 3 ] = '\0';
                  p += 4;
                  while ( isspace( (unsigned char)*p ) )
                        p++;
            }
            else
            {
                  t->priority[ 0 ] = '\0';
            }

            // 3. extract date
            sscanf( p, "%10s", t->date );
            p += strlen( t->date );
            while ( isspace( (unsigned char)*p ) )
                  p++;

            // 4. if priority wasn't found before, check again after date
            if ( t->priority[ 0 ] == '\0' && p[ 0 ] == '(' &&
                 isalpha( (unsigned char)p[ 1 ] ) && p[ 2 ] == ')' )
            {
                  strncpy( t->priority, p, 3 );
                  t->priority[ 3 ] = '\0';
                  p += 4;
                  while ( isspace( (unsigned char)*p ) )
                        p++;
            }

            // 5. extract @type
            if ( *p == '@' )
            {
                  ++p;
                  sscanf( p, "%31s", t->type );
                  add_type( t->type );
                  p += strlen( t->type );
                  while ( isspace( (unsigned char)*p ) )
                        p++;
            }
            else
            {
                  strcpy( t->type, "all" );
                  add_type( "all" );
            }

            // 6. remaining is the text
            strncpy( t->text, p, MAX_LINE - 1 );
            todo_count++;
      }

      fclose( f );
}

static void save_todos_to_file( void )
{
      pthread_mutex_lock( &todo_mutex );
      FILE *f = fopen( todo_filename, "w" );
      if ( !f )
      {
            perror( "write" );
            pthread_mutex_unlock( &todo_mutex );
            return;
      }

      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];

            if ( t->completed )
            {
                  // Save completed format:
                  // x <completion_date> <original_date> @type text
                  // [pri:X]
                  fprintf( f, "x %s %s @%s %s", t->completion_date, t->date,
                           t->type, t->text );
            }
            else
            {
                  // Save incomplete format:
                  // (X) <date> @type text
                  if ( t->priority[ 0 ] != '\0' )
                        fprintf( f, "%s %s @%s %s", t->priority, t->date,
                                 t->type, t->text );
                  else
                        fprintf( f, "%s @%s %s", t->date, t->type, t->text );
            }

            fputc( '\n', f );
      }

      fclose( f );
      pthread_mutex_unlock( &todo_mutex );
}

/* ───────────────────────────────────────────── logic ── */
static void toggle_completed( int visible_index )
{
      int shown = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];
            if ( strcmp( types[ selected_type ], "all" ) != 0 &&
                 strcmp( t->type, types[ selected_type ] ) != 0 )
                  continue;

            if ( shown == visible_index )
            {
                  t->completed = !t->completed;

                  if ( t->completed )
                  {
                        // Set today's date
                        time_t now        = time( NULL );
                        struct tm *tm_now = localtime( &now );
                        strftime( t->completion_date, sizeof t->completion_date,
                                  "%Y-%m-%d", tm_now );

                        // If priority exists, move it to end of text as
                        // "pri:X"
                        if ( t->priority[ 0 ] == '(' &&
                             t->priority[ 2 ] == ')' )
                        {
                              char pri_tag[ 8 ];
                              snprintf( pri_tag, sizeof pri_tag, " pri:%c",
                                        t->priority[ 1 ] );

                              // Only append if not already there
                              if ( !strstr( t->text, pri_tag ) &&
                                   strlen( t->text ) + strlen( pri_tag ) <
                                       MAX_LINE )
                              {
                                    strcat( t->text, pri_tag );
                              }

                              // Clear priority field
                              t->priority[ 0 ] = '\0';
                        }
                        // 🔽 ADD THIS LINE to trigger exec hook
                        run_exec_hook( "Completed: ", t->text );
                  }
                  else
                  {
                        t->completion_date[ 0 ] = '\0';

                        // On un-complete: detect and extract "pri:X"
                        // from end of text
                        char *pri = strstr( t->text, " pri:" );
                        if ( pri && strlen( pri ) == 6 &&
                             isalpha( (unsigned char)pri[ 5 ] ) )
                        {
                              // Restore priority
                              snprintf( t->priority, sizeof t->priority, "(%c)",
                                        pri[ 5 ] );

                              // Remove it from the end of text
                              *pri = '\0';

                              // Also trim trailing whitespace just in
                              // case
                              size_t len = strlen( t->text );
                              while (
                                  len > 0 &&
                                  isspace( (unsigned char)t->text[ len - 1 ] ) )
                              {
                                    t->text[ len - 1 ] = '\0';
                                    len--;
                              }
                        }
                        // 🔽 ADD THIS LINE to trigger exec hook
                        run_exec_hook( "Uncompleted: ", t->text );
                  }

                  if ( !streaming_mode )
                        save_todos_to_file();
                  return;
            }

            ++shown;
      }
}

/* ───────────────────────────────────────────── UI ── */
/* ───────────────────────────────────────────── left type panel ── */
/* Draws the vertical “types” panel and returns its width. */
#define TYPE_PANEL_W 24 /* change once → layout adapts */

static int draw_type_panel( void )
{
      const int col_w    = TYPE_PANEL_W - 2; // space for text
      const int max_rows = LINES - 2;

      // Draw vertical border lines
      for ( int y = 1; y < LINES; ++y )
            mvprintw( y, 0, " %*s:", col_w, "" );

      // Draw each @type, truncated to fit within col_w
      for ( int i = 0; i < type_count && i < max_rows; ++i )
      {
            bool sel = ( i == selected_type );
            if ( sel )
                  attron( COLOR_PAIR( 2 ) | A_BOLD );

            // Clear area first to avoid leftover characters
            mvhline( i + 1, 1, ' ', col_w );

            // Truncate and print
            mvaddnstr( i + 1, 1, types[ i ], col_w );

            if ( sel )
                  attroff( COLOR_PAIR( 2 ) | A_BOLD );
      }

      return TYPE_PANEL_W;
}

static const char *trimmed( const char *s )
{
      while ( *s && isspace( (unsigned char)*s ) )
            ++s;

      static char buffer[ MAX_LINE ];
      strncpy( buffer, s, sizeof( buffer ) - 1 );
      buffer[ sizeof( buffer ) - 1 ] = '\0';

      size_t len = strlen( buffer );
      while ( len > 0 && isspace( (unsigned char)buffer[ len - 1 ] ) )
            buffer[ --len ] = '\0';

      return buffer;
}

/* ---------------------------------------------------------------------------
 *  draw_ui  – one full screen refresh
 * ------------------------------------------------------------------------- */
/* --------------------------------------------------------------------
 *  draw_ui  – single routine that repaints the whole screen.
 *             – draws the @type side‑bar,
 *             – header / help overlay,
 *             – list of todos.
 * ------------------------------------------------------------------ */
static void draw_ui( void )
{
      erase();

      /* ---------------------------------------------------- side panel */
      const int panel_w = draw_type_panel(); /* left bar          */

      /* --------------------------------------------------- column map  */
      const int TYPE_COL_W = 8;             /* width of "@foo"   */
      const int DATE_COL   = panel_w + 2;   /* YYYY‑MM‑DD        */
      const int PRIO_COL   = DATE_COL + 11; /* "(A)"             */
      const int TYPE_COL   = PRIO_COL + 4;  /* @type (optional)  */

      const bool show_type_col = strcmp( types[ selected_type ], "all" ) == 0;
      int text_col =
          show_type_col
              ? TYPE_COL + TYPE_COL_W + 1 // @type column shown
              : PRIO_COL + 4; // always reserve space after prio (even if blank)
      /* ------------------------------------------------- help overlay  */
      if ( show_help )
      {
            attron( COLOR_PAIR( 2 ) | A_BOLD );
            mvprintw( 0, 0, "HELP — press any key" );
            attroff( COLOR_PAIR( 2 ) | A_BOLD );
            mvprintw( 2, 2, "j/k        move up / down" );
            mvprintw( 3, 2, "h/l        switch context" );
            mvprintw( 4, 2, "SPACE      toggle completed" );
            mvprintw( 5, 2, "?          help" );
            mvprintw( 6, 2, "q          quit" );
            wnoutrefresh( stdscr );
            doupdate();
            return;
      }

      /* ----------------------------------------------------- header    */
      attron( COLOR_PAIR( 2 ) | A_BOLD );
      mvprintw( 0, 0, "   " );
      attron( strcmp( types[ selected_type ], "all" ) == 0
                  ? ( COLOR_PAIR( 9 ) | A_BOLD )
                  : ( COLOR_PAIR( 8 ) | A_BOLD ) );
      printw( "@%s", types[ selected_type ] );
      attroff( COLOR_PAIR( 8 ) | COLOR_PAIR( 9 ) | A_BOLD );
      mvhline( 1, 0, '-', COLS );

      /* ------------------------------------------------ list viewport  */
      int row           = 2;
      int visible_lines = LINES - 2;

      if ( selected_index < scroll_offset )
            scroll_offset = selected_index;
      else if ( selected_index >= scroll_offset + visible_lines )
            scroll_offset = selected_index - visible_lines + 1;

      pthread_mutex_lock( &todo_mutex ); // else causes not all lines to be
                                         // printed on high stress
      int local_idx = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];

            if ( strcmp( types[ selected_type ], "all" ) != 0 &&
                 strcmp( t->type, types[ selected_type ] ) != 0 )
                  continue;

            if ( local_idx++ < scroll_offset )
                  continue;
            if ( row >= LINES )
                  break;

            const bool is_sel = ( local_idx - 1 == selected_index );

            attr_t date_attr, text_attr;
            if ( t->completed )
            {
                  date_attr = is_sel ? ( COLOR_PAIR( 7 ) | A_BOLD )
                                     : ( COLOR_PAIR( 6 ) | A_DIM );
                  text_attr = COLOR_PAIR( 5 ) | ( is_sel ? A_BOLD : A_DIM );
            }
            else
            {
                  date_attr =
                      is_sel ? ( COLOR_PAIR( 4 ) | A_BOLD ) : COLOR_PAIR( 3 );
                  text_attr =
                      is_sel ? ( COLOR_PAIR( 1 ) | A_BOLD ) : COLOR_PAIR( 1 );
            }

            attron( date_attr );
            mvprintw( row, DATE_COL, "%s", t->date );
            attroff( date_attr );

            if ( *t->priority )
            {
                  int prio_color = 5;
                  switch ( t->priority[ 1 ] )
                  {
                  case 'A':
                        prio_color = 11;
                        break;
                  case 'B':
                        prio_color = 12;
                        break;
                  case 'C':
                        prio_color = 13;
                        break;
                  case 'D':
                        prio_color = 14;
                        break;
                  case 'E':
                        prio_color = 15;
                        break;
                  case 'F':
                        prio_color = 16;
                        break;
                  }
                  attron( COLOR_PAIR( prio_color ) | A_BOLD );
                  mvprintw( row, PRIO_COL, "%-4s", t->priority );
                  attroff( COLOR_PAIR( prio_color ) | A_BOLD );
            }
            else
            {
                  //                  mvprintw( row, PRIO_COL, "    " );
                  // mvprintw( row, PRIO_COL, "   " );
            }

            if ( show_type_col )
            {
                  int type_color = strcmp( t->type, "all" ) == 0 ? 9 : 8;
                  mvhline( row, TYPE_COL, ' ', TYPE_COL_W );
                  mvaddch( row, TYPE_COL, '@' | COLOR_PAIR( 10 ) | A_DIM );
                  attron( COLOR_PAIR( type_color ) );
                  mvaddnstr( row, TYPE_COL + 1, t->type, TYPE_COL_W - 1 );
                  attroff( COLOR_PAIR( type_color ) );
            }

            int max_text = COLS - text_col - 1;
            if ( max_text < 0 )
                  max_text = 0;

            mvhline( row, text_col, ' ', max_text );
            attron( text_attr );
            mvaddnstr( row, text_col, trimmed( t->text ), max_text );
            attroff( text_attr );
            ++row;
      }
      pthread_mutex_unlock( &todo_mutex );

      // Fix that it wont go to the very bottom on auto scroll
      if ( auto_scroll_enabled )
      {
            int visible =
                count_visible_items_for_type( types[ selected_type ] );

            count_visible_items_for_type( types[ selected_type ] );
            if ( selected_index >= visible )
                  selected_index = visible - 1;

            if ( visible > visible_lines )
            {
                  scroll_offset = visible - visible_lines;
            }
            else
            {
                  scroll_offset = 0;
            }
      }

      ////

      wnoutrefresh( stdscr );
      doupdate();
}
static inline void safe_draw_ui( void )
{
      need_redraw = 1;
      write( wakeup_pipe[ 1 ], "x", 1 ); // wake up UI thread
}

/* ──────────────────────────────────────────────── funcs, streaming, by pipe ──
 */

bool is_unix_socket( const char *path )
{
      struct stat st;
      if ( stat( path, &st ) == -1 )
      {
            perror( "stat" );
            return false;
      }

      return S_ISSOCK( st.st_mode );
}

void *handle_socket_client( void *arg )
{
      int client_fd = *(int *)arg;
      free( arg );

      char buf[ 512 ];
      char assembly[ 2 * MAX_LINE ];
      size_t asm_len         = 0;
      struct timeval last_ui = { 0 };

      while ( 1 )
      {
            ssize_t n = read( client_fd, buf, sizeof( buf ) );
            if ( n <= 0 )
                  break;

            if ( asm_len + n > sizeof( assembly ) )
                  n = sizeof( assembly ) - asm_len;

            memcpy( assembly + asm_len, buf, n );
            asm_len += (size_t)n;

            size_t start = 0;
            while ( 1 )
            {
                  void *nlp = memchr( assembly + start, '\n', asm_len - start );
                  if ( !nlp )
                        break;

                  size_t len = (char *)nlp - ( assembly + start );
                  if ( len > 0 && len < MAX_LINE )
                  {
                        assembly[ start + len ] = '\0';
                        char *line              = assembly + start;

                        while ( *line && isspace( (unsigned char)*line ) )
                              ++line;

                        if ( *line )
                        {
                              pthread_mutex_lock( &todo_mutex );

                              if ( todo_count >= MAX_TODOS )
                                    remove_oldest_todo();
                              else
                              {
                                    Todo *t = &todos[ todo_count++ ];
                                    memset( t, 0, sizeof( Todo ) );

                                    t->completed  = false;
                                    time_t now    = time( NULL );
                                    struct tm *tm = localtime( &now );
                                    strftime( t->date, sizeof( t->date ),
                                              "%Y-%m-%d", tm );

                                    const char *at = strchr( line, '@' );
                                    if ( at )
                                    {
                                          const char *end = at + 1;
                                          while (
                                              *end &&
                                              !isspace( (unsigned char)*end ) )
                                                ++end;

                                          size_t type_len = end - ( at + 1 );
                                          if ( type_len > 0 &&
                                               type_len < MAX_TYPE )
                                          {
                                                strncpy( t->type, at + 1,
                                                         type_len );
                                                t->type[ type_len ] = '\0';
                                                add_type( t->type );
                                          }
                                          else
                                          {
                                                strncpy( t->type, "all",
                                                         sizeof( t->type ) );
                                          }

                                          size_t offset = at - line;
                                          memmove( line + offset, end,
                                                   strlen( end ) + 1 );
                                          while ( line[ offset ] == ' ' )
                                                offset++;
                                          strncpy( t->text, line + offset,
                                                   MAX_LINE - 1 );
                                    }
                                    else
                                    {
                                          strncpy( t->type, "all",
                                                   sizeof( t->type ) );
                                          strncpy( t->text, line,
                                                   MAX_LINE - 1 );
                                    }

                                    if ( auto_scroll_enabled )
                                    {
                                          selected_index =
                                              count_visible_items_for_type(
                                                  types[ selected_type ] ) -
                                              1;
                                          scroll_offset =
                                              selected_index - ( LINES - 3 );
                                          if ( scroll_offset < 0 )
                                                scroll_offset = 0;
                                    }
                              }

                              pthread_mutex_unlock( &todo_mutex );
                        }
                  }

                  start += len + 1;
            }

            if ( start < asm_len )
            {
                  memmove( assembly, assembly + start, asm_len - start );
                  asm_len -= start;
            }
            else
            {
                  asm_len = 0;
            }

            struct timeval now_tv;
            gettimeofday( &now_tv, NULL );
            long diff_ms = ( now_tv.tv_sec - last_ui.tv_sec ) * 1000 +
                           ( now_tv.tv_usec - last_ui.tv_usec ) / 1000;
            if ( diff_ms > 50 )
            {
                  safe_draw_ui();
                  last_ui = now_tv;
            }
      }

      close( client_fd );
      return NULL;
}
/* ------------------------------------------------------------------ */
/* CONNECT-MODE reader                                                */
static void *socket_reader_thread( void *arg )
/* ------------------------------------------------------------------ */
{
      const char *sock = (const char *)arg;

reconnect:
      /* ① create & connect ------------------------------------------------ */
      int fd = socket( AF_UNIX, SOCK_STREAM, 0 );
      if ( fd == -1 )
      {
            perror( "socket" );
            sleep( 1 );
            goto reconnect;
      }

      struct sockaddr_un sa = { .sun_family = AF_UNIX };
      strncpy( sa.sun_path, sock, sizeof( sa.sun_path ) - 1 );

      if ( connect( fd, (struct sockaddr *)&sa, sizeof sa ) == -1 )
      {
            perror( "connect" );
            close( fd );
            sleep( 1 );
            goto reconnect;
      }

      /* ② announce ourselves as reader ----------------------------------- */
      if ( write( fd, "READER\n", 7 ) != 7 )
      {
            perror( "handshake-write" );
            close( fd );
            sleep( 1 );
            goto reconnect;
      }

      /* daemon answers “OK\n”; ignore contents but read it so it’s gone */
      char ack[ 4 ];
      read( fd, ack, sizeof ack );

      /* ③ run the normal line-by-line reader loop ------------------------ */
      int *p = malloc( sizeof *p );
      *p     = fd;
      handle_socket_client( p ); /* will return when connection dies */

      /* ④ auto-reconnect -------------------------------------------------- */
      free( p ); /* handle_socket_client closed fd for us     */
      sleep( 1 );
      goto reconnect;
      return NULL; // 🔧 Add this line to silence the warning
}

void *pipe_reader_thread( void *arg )
{
      const char *filename = (const char *)arg;
      int fd;

      while ( 1 )
      {
            fd = open( filename, O_RDONLY | O_NONBLOCK );
            if ( fd < 0 )
            {
                  perror( "pipe open" );
                  sleep( 1 );
                  continue;
            }

            struct pollfd pfd = { .fd = fd, .events = POLLIN };

            char buf[ 512 ];
            char assembly[ 2 * MAX_LINE ]; /* room for partial tail */
            size_t asm_len = 0;

            struct timeval last_ui = { 0 };

            while ( 1 )
            {
                  int ret = poll( &pfd, 1, 1000 );
                  if ( ret == -1 )
                  {
                        perror( "poll" );
                        break;
                  }
                  if ( ret == 0 )
                        continue; /* timeout */

                  if ( pfd.revents & POLLIN )
                  {
                        ssize_t n = read( fd, buf, sizeof( buf ) );
                        if ( n <= 0 )
                              break;

                        /* 1️⃣  Append the new chunk to ‘assembly’ */
                        if ( asm_len + n >
                             sizeof( assembly ) ) /* safety: drop overflow */
                              n = sizeof( assembly ) - asm_len;
                        memcpy( assembly + asm_len, buf, n );
                        asm_len += (size_t)n;

                        /* 2️⃣  Extract every complete line in ‘assembly’ */
                        size_t start = 0;
                        for ( ;; )
                        {
                              void *nlp = memchr( assembly + start, '\n',
                                                  asm_len - start );
                              if ( !nlp )
                                    break; /* no full line left */

                              size_t len = (char *)nlp - ( assembly + start );
                              if ( len > 0 && len < MAX_LINE )
                              {
                                    assembly[ start + len ] = '\0';
                                    char *line              = assembly + start;

                                    /* skip blank/whitespace‑only lines */
                                    while ( *line &&
                                            isspace( (unsigned char)*line ) )
                                          ++line;
                                    if ( *line )
                                    {
                                          /* ---------- store the todo
                                           * ---------- */
                                          pthread_mutex_lock( &todo_mutex );

                                          if ( todo_count < MAX_TODOS )
                                          {
                                                Todo *t = &todos[ todo_count ];
                                                memset( t, 0, sizeof( Todo ) );

                                                t->completed = false;
                                                strncpy( t->type, "all",
                                                         sizeof( t->type ) );

                                                time_t now = time( NULL );
                                                struct tm *tm =
                                                    localtime( &now );
                                                strftime( t->date,
                                                          sizeof( t->date ),
                                                          "%Y-%m-%d", tm );

                                                strncpy( t->text, line,
                                                         MAX_LINE - 1 );
                                                todo_count++;

                                                if ( auto_scroll_enabled )
                                                {
                                                      selected_index =
                                                          count_visible_items_for_type(
                                                              types
                                                                  [ selected_type ] ) -
                                                          1;
                                                      scroll_offset =
                                                          selected_index -
                                                          ( LINES - 3 );
                                                      if ( scroll_offset < 0 )
                                                            scroll_offset = 0;
                                                }
                                          }

                                          pthread_mutex_unlock( &todo_mutex );
                                          /* ------------------------------------
                                           */
                                    }
                              }

                              start += len + 1; /* step past '\n' */
                        }

                        /* 3️⃣  Move any tail bytes (after last \n) to front */
                        if ( start < asm_len )
                        {
                              memmove( assembly, assembly + start,
                                       asm_len - start );
                              asm_len -= start;
                        }
                        else
                        {
                              asm_len = 0;
                        }

                        /* 4️⃣  Redraw at most every 50 ms */
                        struct timeval now_tv;
                        gettimeofday( &now_tv, NULL );
                        long diff_ms =
                            ( now_tv.tv_sec - last_ui.tv_sec ) * 1000 +
                            ( now_tv.tv_usec - last_ui.tv_usec ) / 1000;
                        if ( diff_ms > 50 )
                        {
                              safe_draw_ui();
                              last_ui = now_tv;
                        }
                  }
                  else if ( pfd.revents & ( POLLERR | POLLHUP | POLLNVAL ) )
                  {
                        break; /* pipe closed/error */
                  }
            }
            close( fd );
      }
      return NULL;
}

/* ────────────────────────────────────────────────────────── helpers ── */

/* ───────────────────────────────────────────── main loop ── */

static void ui_loop( void )
{

      struct pollfd fds[ 2 ] = { { STDIN_FILENO, POLLIN, 0 },
                                 { wakeup_pipe[ 0 ], POLLIN, 0 } };

      while ( 1 )
      {
            poll( fds, 2, -1 ); // wait for key or redraw signal

            if ( fds[ 1 ].revents & POLLIN )
            {
                  char buf[ 8 ];
                  read( wakeup_pipe[ 0 ], buf, sizeof( buf ) ); // clear wakeup
            }

            if ( need_redraw )
            {
                  draw_ui();
                  need_redraw = 0;
            }

            if ( !( fds[ 0 ].revents & POLLIN ) )
                  continue;

            int ch = getch();
            if ( ch == 'q' )
                  break;

            if ( show_help )
            {
                  show_help = false;
                  safe_draw_ui();
                  continue;
            }

            switch ( ch )
            {
            case ' ':
                  toggle_completed( selected_index );
                  break;
            case '?':
                  show_help = true;
                  break;
            case 's':
                  prompt_priority();
                  break;
            case 'p': // ascending priority
                  sort_todos_by_priority( false );
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;

            case 'P': // descending priority
                  sort_todos_by_priority( true );
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;

            case 'j':
                  if ( selected_index + 1 <
                       count_visible_items_for_type( types[ selected_type ] ) )
                  {
                        ++selected_index;
                  }

                  // If at bottom, not already auto-scrolling, and in stream
                  // mode → enable auto-scroll
                  if ( !auto_scroll_enabled &&
                       selected_index + 1 >= count_visible_items_for_type(
                                                 types[ selected_type ] ) &&
                       streaming_mode )
                  {
                        auto_scroll_enabled = true;
                  }
                  break;

            case 'k':
                  if ( auto_scroll_enabled )
                  {
                        auto_scroll_enabled = false;
                  }
                  if ( selected_index > 0 )
                  {
                        --selected_index;
                  }
                  break;
                  break;
            case 'h':
                  selected_type =
                      ( selected_type - 1 + type_count ) % type_count;
                  selected_index = 0;
                  break;
            case 'l':
                  selected_type  = ( selected_type + 1 ) % type_count;
                  selected_index = 0;
                  break;

            case 'd':
                  sort_todos_by_date( false ); // ascending
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;

            case 'D':
                  sort_todos_by_date( true ); // descending
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;
            case 'g':
                  group_todos_by_completed();
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;

            case 'G':
                  // Restore initial order from file read.
                  load_todos( todo_filename );
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;

            case 'n':
                  add_new_todo();
                  break;
            case '@':
            {
                  echo();
                  curs_set( 1 );
                  char input[ MAX_TYPE ] = { 0 };
                  move( LINES - 1, 0 );
                  clrtoeol();
                  attron( COLOR_PAIR( 2 ) | A_BOLD );
                  printw( "Jump to context @" );
                  attroff( COLOR_PAIR( 2 ) | A_BOLD );
                  getnstr( input, MAX_TYPE - 1 );
                  noecho();
                  curs_set( 0 );

                  if ( strlen( input ) > 0 )
                  {
                        // If not already known, add to types
                        bool found = false;
                        for ( int i = 0; i < type_count; ++i )
                        {
                              if ( strcmp( types[ i ], input ) == 0 )
                              {
                                    selected_type = i;
                                    found         = true;
                                    break;
                              }
                        }
                        if ( !found && type_count < MAX_TODOS )
                        {
                              types[ type_count ] = strdup( input );
                              selected_type       = type_count++;
                        }

                        selected_index = 0;
                        scroll_offset  = 0;
                  }

                  move( LINES - 1, 0 );
                  clrtoeol();
                  break;
            }
            case 'A':
                  archive_completed_todos();
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;

            case 't':
                  prompt_type();
                  break;

            case 'f':
                  auto_scroll_enabled = !auto_scroll_enabled;
                  move( LINES - 1, 0 );
                  clrtoeol();
                  if ( auto_scroll_enabled )
                        mvprintw( LINES - 1, 0, "Auto-scroll: ON" );
                  else
                        mvprintw( LINES - 1, 0, "Auto-scroll: OFF" );
                  refresh();
                  napms( 800 );
                  move( LINES - 1, 0 );
                  clrtoeol();
                  break;
            }

            safe_draw_ui();
      }
}

/* ───────────────────────────────────────────── entry ── */

int main( int argc, char **argv )
{

      for ( int i = 1; i < argc; ++i )
      {
            if ( !todo_filename )
                  todo_filename = argv[ i ];
            else if ( strcmp( argv[ i ], "--exec" ) == 0 && i + 1 < argc )
                  exec_script = argv[ ++i ];
      }

      if ( !todo_filename )
      {
            fprintf( stderr, "Usage: %s <todo-file> [-s] [--exec <script>]\n",
                     argv[ 0 ] );
            return 1;
      }

      selected_type = 0;

      //-- streaming functionality, if activated by file being pipe
      if ( is_unix_socket( todo_filename ) )
      {
            streaming_mode        = true;
            types[ type_count++ ] = strdup( "all" );

            // If file does not exist yet, let the thread create the socket
            if ( !is_unix_socket( todo_filename ) )
                  unlink( todo_filename ); // safety: delete stale file

            pthread_t reader;
            pthread_create( &reader, NULL, socket_reader_thread,
                            (void *)todo_filename );

            pthread_detach( reader );
      }
      else
      {
            load_todos( todo_filename );
      }

      //-- end of streaming functionality

      setlocale( LC_ALL, "" );

      if ( pipe( wakeup_pipe ) == -1 )
      {
            perror( "pipe" );
            exit( 1 );
      }

      initscr();
      curs_set( 0 );
      noecho();
      cbreak();
      keypad( stdscr, TRUE );

      start_color();
      use_default_colors();
      init_pair( 1, 15, -1 );   /* bright white */
      init_pair( 2, 14, -1 );   /* cyan header  */
      init_pair( 3, 220, -1 );  /* yellow date  */
      init_pair( 4, 0, 220 );   /* black on ylw */
      init_pair( 5, 245, -1 );  /* light gray   */
      init_pair( 6, 244, -1 );  /* darker gray  */
      init_pair( 7, 244, 236 ); /* gray on dark */
      init_pair( 8, 14, -1 );   /* cyan         */
      init_pair( 9, 13, -1 );   /* magenta text for 'all' category */
      init_pair( 10, 250, -1 ); /* light gray for '@' prefix */

      init_pair( 11, COLOR_RED, -1 );     // (A)
      init_pair( 12, COLOR_YELLOW, -1 );  // (B)
      init_pair( 13, COLOR_GREEN, -1 );   // (C)
      init_pair( 14, COLOR_CYAN, -1 );    // (D)
      init_pair( 15, COLOR_BLUE, -1 );    // (E)
      init_pair( 16, COLOR_MAGENTA, -1 ); // (F)

      safe_draw_ui();
      ui_loop();

      endwin();
      return 0;
}
