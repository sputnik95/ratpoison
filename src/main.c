/* Ratpoison.
 * Copyright (C) 2000, 2001 Shawn Betts
 *
 * This file is part of ratpoison.
 *
 * ratpoison is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * ratpoison is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307 USA
 */

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xproto.h>
#include <X11/cursorfont.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>

#include "ratpoison.h"

static void init_screen (screen_info *s, int screen_num);

int alarm_signalled = 0;
int kill_signalled = 0;
int hup_signalled = 0;
int rat_x;
int rat_y;
int rat_visible = 1;		/* rat is visible by default */

Atom wm_state;
Atom wm_change_state;
Atom wm_protocols;
Atom wm_delete;
Atom wm_take_focus;
Atom wm_colormaps;

Atom rp_command;
Atom rp_command_request;
Atom rp_command_result;

int rp_current_screen;
screen_info *screens;
int num_screens;
Display *dpy;

struct rp_defaults defaults;

int ignore_badwindow = 0;

char **myargv;

struct rp_key prefix_key;

struct modifier_info rp_modifier_info;

/* rudeness levels */
int rp_honour_transient_raise = 1;
int rp_honour_normal_raise = 1;
int rp_honour_transient_map = 1;
int rp_honour_normal_map = 1;

char *rp_error_msg = NULL;

/* Command line options */
static struct option ratpoison_longopts[] = 
  { {"help", 	no_argument, 		0, 	'h'},
    {"version", no_argument, 		0, 	'v'},
    {"command", required_argument, 	0, 	'c'},
    {0, 	0, 			0, 	0} };

static char ratpoison_opts[] = "hvc:";

void
fatal (const char *msg)
{
  fprintf (stderr, "ratpoison: %s", msg);
  abort ();
}

void *
xmalloc (size_t size)
{
  register void *value = malloc (size);
  if (value == 0)
    fatal ("Virtual memory exhausted");
  return value;
}

void *
xrealloc (void *ptr, size_t size)
{
  register void *value = realloc (ptr, size);
  if (value == 0)
    fatal ("Virtual memory exhausted");
  PRINT_DEBUG("realloc: %d\n", size);
  return value;
}

char *
xstrdup (char *s)
{
  char *value;
  value = strdup (s);
  if (value == 0)
    fatal ("Virtual memory exhausted");
  return value;
}

/* Return a new string based on fmt. */
char *
xvsprintf (char *fmt, va_list ap)
{
  int size, nchars;
  char *buffer;

  /* A resonable starting value. */
  size = strlen (fmt) + 1;
  buffer = (char *)xmalloc (size);

  nchars = vsnprintf (buffer, size, fmt, ap);

  /* From the GNU Libc manual: In versions of the GNU C library prior
     to 2.1 the return value is the number of characters stored, not
     including the terminating null; unless there was not enough space
     in S to store the result in which case `-1' is returned. */
  if (nchars == -1)
    {
      do
	{
	  size *= 2;
	  buffer = (char *)xrealloc (buffer, size);
	} while (vsnprintf (buffer, size, fmt, ap) == -1);
    }
  else if (nchars >= size)
    {
      buffer = (char *)xrealloc (buffer, nchars + 1);
      vsnprintf (buffer, nchars + 1, fmt, ap);
    }

  return buffer;
}

/* Return a new string based on fmt. */
char *
xsprintf (char *fmt, ...)
{
  char *buffer;
  va_list ap;

  va_start (ap, fmt);
  buffer = xvsprintf (fmt, ap);
  va_end (ap);

  return buffer;
}

void
sighandler (int signum)
{
  kill_signalled++;
}

void
hup_handler (int signum)
{
  hup_signalled++;
}

void
alrm_handler (int signum)
{
  alarm_signalled++;
}

int
handler (Display *d, XErrorEvent *e)
{
  char error_msg[100];

  if (e->request_code == X_ChangeWindowAttributes && e->error_code == BadAccess) {
    fprintf(stderr, "ratpoison: There can be only ONE.\n");
    exit(EXIT_FAILURE);
  }  

  if (ignore_badwindow && e->error_code == BadWindow) return 0;

  XGetErrorText (d, e->error_code, error_msg + 7, sizeof (error_msg) - 7);
  fprintf (stderr, "ratpoison: ERROR: %s!\n", error_msg);

  /* If there is already an error to report, replace it with this new
     one. */
  if (rp_error_msg) 
    free (rp_error_msg);
  rp_error_msg = xstrdup (error_msg);

  return 0;
}

void
set_sig_handler (int sig, void (*action)(int))
{
  /* use sigaction because SVR4 systems do not replace the signal
    handler by default which is a tip of the hat to some god-aweful
    ancient code.  So use the POSIX sigaction call instead. */
  struct sigaction act;
       
  /* check setting for sig */
  if (sigaction (sig, NULL, &act)) 
    {
      PRINT_ERROR ("Error %d fetching SIGALRM handler\n", errno );
    } 
  else 
    {
      /* if the existing action is to ignore then leave it intact
	 otherwise add our handler */
      if (act.sa_handler != SIG_IGN) 
	{
	  act.sa_handler = action;
	  sigemptyset(&act.sa_mask);
	  act.sa_flags = 0;
	  if (sigaction (sig, &act, NULL)) 
	    {
	      PRINT_ERROR ("Error %d setting SIGALRM handler\n", errno );
	    }
	}
    }
}

void
print_version ()
{
  printf ("%s %s\n", PACKAGE, VERSION);
  printf ("Copyright (C) 2000, 2001 Shawn Betts\n\n");

  exit (EXIT_SUCCESS);
}  

void
print_help ()
{
  printf ("Help for %s %s\n\n", PACKAGE, VERSION);
  printf ("-h, --help            Display this help screen\n");
  printf ("-v, --version         Display the version\n");
  printf ("-c, --command         Send ratpoison a colon-command\n\n");

  printf ("Report bugs to ratpoison-devel@lists.sourceforge.net\n\n");

  exit (EXIT_SUCCESS);
}

void
read_rc_file (FILE *file)
{
  size_t n = 256;
  char *partial;
  char *line;
  int linesize = n;

  partial = (char*)xmalloc(n);
  line = (char*)xmalloc(linesize);

  *line = '\0';
  while (fgets (partial, n, file) != NULL)
    {
      if ((strlen (line) + strlen (partial)) >= linesize)
	{
	  linesize *= 2;
	  line = (char*) xrealloc (line, linesize);
	}

      strcat (line, partial);

      if (feof(file) || (*(line + strlen(line) - 1) == '\n'))
	{
	  /* FIXME: this is a hack, command() should properly parse
	     the command and args (ie strip whitespace, etc) 

	     We should not care if there is a newline (or vertical
	     tabs or linefeeds for that matter) at the end of the
	     command (or anywhere between tokens). */
	  if (*(line + strlen(line) - 1) == '\n')
	    *(line + strlen(line) - 1) = '\0';

	  PRINT_DEBUG ("rcfile line: %s\n", line);

	  /* do it */
	  if (*line != '#')
	    {
	      char *result;
	      result = command (0, line);

	      /* Gobble the result. */
	      if (result)
		free (result);
	    }

	  *line = '\0';
	}
    }


  free (line);
  free (partial);
}

static void
read_startup_files ()
{
  char *homedir;
  FILE *fileptr;

  /* first check $HOME/.ratpoisonrc and if that does not exist then try
     /etc/ratpoisonrc */

  homedir = getenv ("HOME");
  if (!homedir)
    {
      PRINT_ERROR ("ratpoison: $HOME not set!?\n");
    }
  else
    {
      char *filename = (char*)xmalloc (strlen (homedir) + strlen ("/.ratpoisonrc") + 1);
      sprintf (filename, "%s/.ratpoisonrc", homedir);
      
      if ((fileptr = fopen (filename, "r")) == NULL)
	{
	  /* we probably don't need to report this, its not an error */
	  PRINT_DEBUG ("ratpoison: could not open %s\n", filename);

	  if ((fileptr = fopen ("/etc/ratpoisonrc", "r")) == NULL)
	    {
	      /* neither is this */
	      PRINT_DEBUG ("ratpoison: could not open /etc/ratpoisonrc\n");
	    }
	}

      if (fileptr)
	{
	  read_rc_file (fileptr);
	  fclose (fileptr);
	}

      free (filename);
    }
}

/* Odd that we spend so much code on making sure the silly welcome
   message is correct. Oh well... */
static void
show_welcome_message ()
{
  rp_action *help_action;
  char *prefix, *help;
  
  prefix = keysym_to_string (prefix_key.sym, prefix_key.state);

  /* Find the help key binding. */
  help_action = find_keybinding_by_action ("help");
  if (help_action)
    help = keysym_to_string (help_action->key, help_action->state);
  else
    help = NULL;


  if (help)
    {
      /* A little kludge to use ? instead of `question' for the help
	 key. */
      if (!strcmp (help, "question"))
	marked_message_printf (0, 0, MESSAGE_WELCOME, prefix, "?");
      else
	marked_message_printf (0, 0, MESSAGE_WELCOME, prefix, help);

      free (help);
    }
  else
    {
      marked_message_printf (0, 0, MESSAGE_WELCOME, prefix, ":help");
    }
  
  free (prefix);
}

static void
init_defaults ()
{
  defaults.win_gravity     = NorthWestGravity;
  defaults.trans_gravity   = CenterGravity;
  defaults.maxsize_gravity = CenterGravity;

  defaults.input_window_size   = 200;
  defaults.window_border_width = 1;
  defaults.bar_x_padding       = 0;
  defaults.bar_y_padding       = 0;
  defaults.bar_location        = NorthEastGravity;
  defaults.bar_timeout 	       = 5;

  defaults.frame_indicator_timeout = 1;

  defaults.padding_left   = 0;
  defaults.padding_right  = 0;
  defaults.padding_top 	  = 0;
  defaults.padding_bottom = 0;

  defaults.font = XLoadQueryFont (dpy, "9x15bold");
  if (defaults.font == NULL)
    {
      fprintf (stderr, "ratpoison: Cannot load font %s.\n", "9x15bold");
      exit (EXIT_FAILURE);
    }

  defaults.wait_for_key_cursor = 1;

  defaults.window_fmt = xstrdup ("%n%s%t");

  defaults.win_name = 0;
  defaults.startup_message = 1;
}

int
main (int argc, char *argv[])
{
  int i;
  int c;
  char **command = NULL;
  int cmd_count = 0;

  myargv = argv;

  /* Parse the arguments */
  while (1)
    {
      int option_index = 0;

      c = getopt_long (argc, argv, ratpoison_opts, ratpoison_longopts, &option_index);
      if (c == -1) break;

      switch (c)
	{
	case 'h':
	  print_help ();
	  break;
	case 'v':
	  print_version ();
	  break;
	case 'c':
	  if (!command)
	    {
	      command = xmalloc (sizeof(char *));
	      cmd_count = 0;
	    }
	  else
	    {
	      command = xrealloc (command, sizeof (char *) * (cmd_count + 1));
	    }

	  command[cmd_count] = xstrdup (optarg);
	  cmd_count++;
	  break;
	default:
	  exit (EXIT_FAILURE);
	}
    }

  if (!(dpy = XOpenDisplay (NULL)))
    {
      fprintf (stderr, "Can't open display\n");
      return EXIT_FAILURE;
    }

  /* Set ratpoison specific Atoms. */
  rp_command = XInternAtom (dpy, "RP_COMMAND", False);
  rp_command_request = XInternAtom (dpy, "RP_COMMAND_REQUEST", False);
  rp_command_result = XInternAtom (dpy, "RP_COMMAND_RESULT", False);

  if (cmd_count > 0)
    {
      int i;

      for (i=0; i<cmd_count; i++)
	{
	  send_command (command[i]);
	  free (command[i]);
	}

      free (command);
      XCloseDisplay (dpy);
      return EXIT_SUCCESS;
    }

  /* Set our Atoms */
  wm_state = XInternAtom(dpy, "WM_STATE", False);
  wm_change_state = XInternAtom(dpy, "WM_CHANGE_STATE", False);
  wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
  wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  wm_take_focus = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
  wm_colormaps = XInternAtom(dpy, "WM_COLORMAP_WINDOWS", False);

  /* Setup signal handlers. */
  XSetErrorHandler(handler);  
  set_sig_handler (SIGALRM, alrm_handler);
  set_sig_handler (SIGTERM, sighandler);
  set_sig_handler (SIGINT, sighandler);
  set_sig_handler (SIGHUP, hup_handler);

  /* Setup ratpoison's internal structures */
  init_defaults();
  init_numbers ();

  /* Initialize the screens */
  num_screens = ScreenCount (dpy);
  screens = (screen_info *)xmalloc (sizeof (screen_info) * num_screens);
  PRINT_DEBUG ("%d screens.\n", num_screens);

  for (i=0; i<num_screens; i++)
    {
      init_screen (&screens[i], i);
    }

  init_window_list ();
  init_frame_lists ();
  update_modifier_map ();
  initialize_default_keybindings ();

  /* Scan for windows */
  rp_current_screen = 0;
  for (i=0; i<num_screens; i++)
    {
      scanwins (&screens[i]);
    }

  read_startup_files ();

  /* Indicate to the user that ratpoison has booted. */
  if (defaults.startup_message)
    show_welcome_message();

  /* If no window has focus, give the key_window focus. */
  if (current_window() == NULL)
    XSetInputFocus (dpy, current_screen()->key_window, 
		    RevertToPointerRoot, CurrentTime);

  listen_for_events ();

  return EXIT_SUCCESS;
}

static void
init_rat_cursor (screen_info *s)
{
  s->rat = XCreateFontCursor( dpy, XC_icon );
}

static void
init_screen (screen_info *s, int screen_num)
{
  XGCValues gv;

  /* Select on some events on the root window, if this fails, then
     there is already a WM running and the X Error handler will catch
     it, terminating ratpoison. */
  XSelectInput(dpy, RootWindow (dpy, screen_num),
               PropertyChangeMask | ColormapChangeMask
               | SubstructureRedirectMask | SubstructureNotifyMask );
  XSync (dpy, False);

  /* Build the display string for each screen */
  s->display_string = xmalloc (strlen(DisplayString (dpy)) + 21);
  sprintf (s->display_string, "DISPLAY=%s", DisplayString (dpy));
  if (strrchr (DisplayString (dpy), ':'))
    {
      char *dot;

      dot = strrchr(s->display_string, '.');
      if (dot)
	sprintf(dot, ".%i", screen_num);
    }

  s->screen_num = screen_num;
  s->root = RootWindow (dpy, screen_num);
  s->def_cmap = DefaultColormap (dpy, screen_num);
  XGetWindowAttributes (dpy, s->root, &s->root_attr);
  
  init_rat_cursor (s);

  s->fg_color = BlackPixel (dpy, s->screen_num);
  s->bg_color = WhitePixel (dpy, s->screen_num);

  /* Setup the GC for drawing the font. */
  gv.foreground = s->fg_color;
  gv.background = s->bg_color;
  gv.function = GXcopy;
  gv.line_width = 1;
  gv.subwindow_mode = IncludeInferiors;
  gv.font = defaults.font->fid;
  s->normal_gc = XCreateGC(dpy, s->root, 
			   GCForeground | GCBackground | GCFunction 
			   | GCLineWidth | GCSubwindowMode | GCFont, 
			   &gv);

  /* Create the program bar window. */
  s->bar_is_raised = 0;
  s->bar_window = XCreateSimpleWindow (dpy, s->root, 0, 0,
				       1, 1, 1, s->fg_color, s->bg_color);

  /* Setup the window that will recieve all keystrokes once the prefix
     key has been pressed. */
  s->key_window = XCreateSimpleWindow (dpy, s->root, 0, 0, 1, 1, 0, WhitePixel (dpy, s->screen_num), BlackPixel (dpy, s->screen_num));
  XSelectInput (dpy, s->key_window, KeyPressMask );
  XMapWindow (dpy, s->key_window);

  /* Create the input window. */
  s->input_window = XCreateSimpleWindow (dpy, s->root, 0, 0, 
  					 1, 1, 1, s->fg_color, s->bg_color);
  XSelectInput (dpy, s->input_window, KeyPressMask );

  /* Create the frame indicator window */
  s->frame_window = XCreateSimpleWindow (dpy, s->root, 1, 1, 1, 1, 1, 
					 s->fg_color, s->bg_color);

  /* Create the help window */
  s->help_window = XCreateSimpleWindow (dpy, s->root, 0, 0, s->root_attr.width,
					s->root_attr.height, 1, s->fg_color, s->bg_color);
  XSelectInput (dpy, s->help_window, KeyPressMask);

  XSync (dpy, 0);
}

void
clean_up ()
{
  int i;

  for (i=0; i<num_screens; i++)
    {
      XDestroyWindow (dpy, screens[i].bar_window);
      XDestroyWindow (dpy, screens[i].key_window);
      XDestroyWindow (dpy, screens[i].input_window);
      XDestroyWindow (dpy, screens[i].frame_window);
      XDestroyWindow (dpy, screens[i].help_window);

      XFreeCursor (dpy, screens[i].rat);
      XFreeColormap (dpy, screens[i].def_cmap);
      XFreeGC (dpy, screens[i].normal_gc);
    }

  XFreeFont (dpy, defaults.font);

  XSetInputFocus (dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
  XCloseDisplay (dpy);
}

/* Given a root window, return the screen_info struct */
screen_info *
find_screen (Window w)
{
  int i;

  for (i=0; i<num_screens; i++)
    if (screens[i].root == w) return &screens[i];

   return NULL;
 }
