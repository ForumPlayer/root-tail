/*
 * Copyright 2001 by Marco d'Itri <md@linux.it>
 * Copyright 2000,2001,2002,2003,2004
 *           Marc Lehmann <pcg@goof.com>,
 *           Chris Moore <chris.moore@mail.com>,
 *           and many others, see README
 *
 * Original version by Mike Baker.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */

#include "config.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <locale.h>
#include <ctype.h>
#include <stdarg.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xfixes.h>

#if HAS_REGEX
#include <regex.h>
#endif

#define SHADE_X 2
#define SHADE_Y 2

/* some italic fonts still go over the margin - this margin of error cleans up the mess */
#define MARGIN_OF_ERROR 2

/* data structures */
struct logfile_entry
{
  struct logfile_entry *next;

  char *fname;                  /* name of file                                 */
  char *desc;                   /* alternative description                      */
  char *buf;                    /* text read but not yet displayed              */
  const char *fontname;
  XFontSet fontset;
  int font_height;
  int font_ascent;
  FILE *fp;                     /* FILE struct associated with file             */
  ino_t inode;                  /* inode of the file opened                     */
  off_t last_size;              /* file size at the last check                  */
  unsigned long color;          /* color to be used for printing                */
  const char *colorname;        /* color name/string                            */
  int partial;                  /* true if the last line isn't complete         */
  int lastpartial;              /* true if the previous output wasn't complete  */
  struct line_node *last;       /* last line we output                          */
  int modified;                 /* true if line is modified & needs displaying  */
};

struct line_node
{
  struct line_node *next;
  struct line_node *prev;
  struct logfile_entry *logfile;

  char *line;                   /* the text of the line (so far)                */
  int len;                      /* the length of the line (in bytes) so far     */
  int wrapped_left;             /* true if wrapped from the previous line       */
  int wrapped_right;            /* true if wrapped to the next line             */
  struct breakinfo *breaks;     /* array of indicies to spaces if wrapped_right */
  int num_words;                /* the number of words in the line              */
  int free_pixels;              /* the number of free pixels to spread out      */
};

struct breakinfo
{
  int index;                    /* index into string of start of substring       */
  int width;                    /* width in pixels of start of substring         */
  int len;                      /* length of substring                           */
};

struct displaymatrix
{
  char *line;
  int len;
  int offset;
  int buffer_size;
  unsigned long color;
};

/* global variables */
static struct line_node *linelist = NULL, *linelist_tail = NULL;
static struct displaymatrix *display;
static int continuation_width = -1;
static int continuation_color;
static int continuation_length;

/* HACK - ideally listlen will start at however many '~'s will fit on
 * the screen */
static unsigned int width = STD_WIDTH, height = STD_HEIGHT;
static int listlen = 50;
static int win_x = LOC_X, win_y = LOC_Y;
static int effect_x_space, effect_y_space; /* how much space does shading / outlining take up */
static int effect_x_offset, effect_y_offset; /* and how does it offset the usable space */
static int do_reopen;
static struct timeval interval = { 2, 400000 };

/* command line options */
static int opt_noinitial, opt_shade, opt_frame, opt_reverse, opt_nofilename,
  opt_outline, opt_noflicker, opt_whole, opt_update, opt_wordwrap,
  opt_justify, geom_mask, opt_minspace, opt_windowed, reload;
static const char *command = NULL,
  *fontname = USE_FONT, *dispname = NULL, *def_color = DEF_COLOR,
  *continuation = "|| ", *cont_color = DEF_CONT_COLOR;

struct logfile_entry *loglist = NULL, *loglist_tail = NULL;

static Display *disp;
static Window root;
static GC WinGC;

#if HAS_REGEX
struct re_list
{
  regex_t from;
  const char *to;
  struct re_list *next;
};
static struct re_list *re_head, *re_tail;
static char *transform_to = NULL;
static regex_t *transformre;
#endif


/* prototypes */
#ifdef USE_TOON_GET_ROOT_WINDOW
static Window ToonGetRootWindow(Display *, int, Window *);
#endif /* USE_TOON_GET_ROOT_WINDOW */

static void redraw (int);
static void refresh (int, int, int, int);

static void display_version (void);
static void display_help (char *);
static void install_signal (int, void (*)(int));
static void *xstrdup (const char *);
static void *xmalloc (size_t);
static void *xrealloc (void *, size_t);
static int daemonize (void);

/* signal handlers */
static void
list_files (int dummy)
{
  struct logfile_entry *e;

  fprintf (stderr, "Files opened:\n");
  for (e = loglist; e; e = e->next)
    fprintf (stderr, "\t%s (%s)\n", e->fname, e->desc);
}

static void
force_reopen (int dummy)
{
  do_reopen = 1;
}

static void
force_refresh (int dummy)
{
  redraw (1);
}

static void
blank_window (int dummy)
{
  XClearArea (disp, root, win_x, win_y, width + MARGIN_OF_ERROR, height, False);
  XFlush (disp);
  exit (0);
}

/* X related functions */
static unsigned long
GetColor (const char *ColorName)
{
  XColor Color;
  XWindowAttributes Attributes;

  XGetWindowAttributes (disp, root, &Attributes);
  Color.pixel = 0;

  if (!XParseColor (disp, Attributes.colormap, ColorName, &Color))
    fprintf (stderr, "can't parse %s\n", ColorName);
  else if (!XAllocColor (disp, Attributes.colormap, &Color))
    fprintf (stderr, "can't allocate %s\n", ColorName);

  return Color.pixel;
}

#ifndef USE_TOON_GET_ROOT_WINDOW
static void
find_root_window (Display *display, int screen_number)
{
  if (!root)
    {
      Atom SWM_VROOT = XInternAtom (display, "__SWM_VROOT", False);
      Atom NAUTILUS_DESKTOP_WINDOW_ID = XInternAtom (display, "NAUTILUS_DESKTOP_WINDOW_ID", False);

      Window unused, *windows = 0;
      unsigned int count;

      Atom type;
      int format;
      unsigned long nitems, bytes_after_return;
      unsigned char *virtual_root_window;

      root = RootWindow (display, screen_number);

      if (XGetWindowProperty (display, root, NAUTILUS_DESKTOP_WINDOW_ID,
                              0, 1, False, XA_WINDOW, &type, &format,
                              &nitems, &bytes_after_return,
                              &virtual_root_window) == Success
          && type == XA_WINDOW)
        {
          if (XQueryTree (display, *(Window *)virtual_root_window, &unused, &unused, &windows, &count))
            root = windows[count - 1];

          XFree (virtual_root_window);
        }
      else if (XQueryTree (display, root, &unused, &unused, &windows, &count))
        {
          int i;

          for (i = 0; i < count; i++)
            {
              if (XGetWindowProperty (display, windows[i], SWM_VROOT,
                                      0, 1, False, XA_WINDOW, &type, &format,
                                      &nitems, &bytes_after_return,
                                      &virtual_root_window) == Success
                   && type == XA_WINDOW)
                {
                  root = *(Window *)virtual_root_window;
                  XFree (virtual_root_window);
                  break;
                }
            }

          XFree (windows);
        }
      else
        fprintf (stderr, "Can't query tree on root window 0x%lx", root);
    }
}
#endif /* USE_TOON_GET_ROOT_WINDOW */

static void
InitWindow (void)
{
  XGCValues gcv;
  unsigned long gcm;
  int screen, ScreenWidth, ScreenHeight;
  struct logfile_entry *e;

  if (!(disp = XOpenDisplay (dispname)))
    {
      fprintf (stderr, "Can't open display %s.\n", dispname);
      exit (1);
    }

  screen = DefaultScreen (disp);
  ScreenHeight = DisplayHeight (disp, screen);
  ScreenWidth = DisplayWidth (disp, screen);

  if (opt_windowed)
    {
      XRectangle rect = { };
      XSetWindowAttributes attr;

      attr.background_pixmap = ParentRelative;
      attr.override_redirect = True;

      root = XCreateWindow (
        disp, DefaultRootWindow (disp), 0, 0, DisplayWidth (disp, screen), DisplayHeight (disp, screen),
        0, CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWBackPixmap, &attr);

      XMapWindow (disp, root);
      XLowerWindow (disp, root);

      XserverRegion region = XFixesCreateRegion (disp, &rect, 1);
      XFixesSetWindowShapeRegion (disp, root, ShapeInput, 0, 0, region);
      XFixesDestroyRegion (disp, region);
    }
  else
    find_root_window (disp, screen);

  gcm = GCBackground;
  gcv.graphics_exposures = True;
  WinGC = XCreateGC (disp, root, gcm, &gcv);
  XMapWindow (disp, root);

  XSetForeground (disp, WinGC, GetColor (DEF_COLOR));

  for (e = loglist; e; e = e->next)
    {
      char **missing_charset_list;
      int missing_charset_count;
      char *def_string;

      e->fontset = XCreateFontSet (disp, e->fontname,
                                   &missing_charset_list, &missing_charset_count,
                                   &def_string);
      if (missing_charset_count)
        {
#if 0
          fprintf (stderr,
                   "Missing charsets in String to FontSet conversion (%s)\n",
                   missing_charset_list[0]);
#endif
          XFreeStringList (missing_charset_list);
        }

      if (!e->fontset)
        {
          fprintf (stderr, "unable to create fontset for font '%s', exiting.\n", e->fontname);
          exit (1);
        }

      {
        XFontSetExtents *xfe = XExtentsOfFontSet (e->fontset);

        e->font_height = xfe->max_logical_extent.height;
        e->font_ascent = -xfe->max_logical_extent.y;
      }

      if (e->font_height > height - effect_y_space)
	{
	  fprintf(stderr, "\n  the display isn't tall enough to display a single line in font '%s'\n",
		  e->fontname);
	  fprintf(stderr, "\n  the geometry in use is %d pixels tall\n", height);
	  fprintf(stderr, "\n  font '%s' is %d pixels tall\n", e->fontname, e->font_height);
	  if (effect_y_space)
	    fprintf(stderr, "\n  the shade or outline options need an extra %d pixel%s of vertical space\n",
		    effect_y_space, effect_y_space == 1 ? "" : "s");
	  fprintf(stderr, "\n");
	  exit(1);
	}
    }

  if (geom_mask & XNegative)
    win_x = win_x + ScreenWidth - width;
  if (geom_mask & YNegative)
    win_y = win_y + ScreenHeight - height;

  {
    struct logfile_entry *e;

    for (e = loglist; e; e = e->next)
      e->color = GetColor (e->colorname);
  }

  XSelectInput (disp, root, ExposureMask | FocusChangeMask);
}

/*
 * if redraw () is passwd a non-zero argument, it does a complete
 * redraw, rather than an update.  if the argument is zero (and
 * -noflicker is in effect) then only the lines which have changed
 * since the last draw are redrawn.
 *
 * the rest is handled by regular refresh ()'es
 */
static void
redraw (int redraw_all)
{
  XSetClipMask (disp, WinGC, None);
  refresh (0, 32768, 1, redraw_all);
}

static void
draw_text (Display *disp, Window root, GC WinGC, int x, int y, struct line_node *line, int foreground)
{
  if (line->wrapped_right && opt_justify && line->breaks)
    {
      int i;
      for (i = 0; i < line->num_words; i++)
        XmbDrawString (disp, root, line->logfile->fontset, WinGC,
                       x + line->breaks[i].width + ((i * line->free_pixels) / (line->num_words - 1))
                         + continuation_width * line->wrapped_left, y,
                       line->line + line->breaks[i].index,
                       line->breaks[i].len);

      if (line->wrapped_left)
        {
          if (foreground) XSetForeground (disp, WinGC, continuation_color);
          XmbDrawString (disp, root, line->logfile->fontset, WinGC, x, y, continuation, continuation_length);
        }
    }
  else
    {
      XmbDrawString (disp, root, line->logfile->fontset, WinGC, x + continuation_width * line->wrapped_left,
                     y, line->line, line->len);

      if (line->wrapped_left)
        {
          if (foreground) XSetForeground (disp, WinGC, continuation_color);
          XmbDrawString (disp, root, line->logfile->fontset, WinGC, x, y, continuation, continuation_length);
        }
    }
}

/* Just redraw everything without clearing (i.e. after an EXPOSE event) */
static void
refresh (int miny, int maxy, int clear, int refresh_all)
{
  int lin = 0;
  int space = height;
  int offset;
  unsigned long black_color = GetColor ("black");
  struct line_node *line;
  int step_per_line;
  int foreground = 0;

  if (opt_reverse)
    offset = effect_y_offset;
  else
    offset = height + effect_y_offset;

  miny -= win_y;
  maxy -= win_y;

  if (clear && !opt_noflicker)
    XClearArea (disp, root, win_x, win_y, width + MARGIN_OF_ERROR, height, False);

  for (line = linelist; line; line = line->next, lin++)
    {
      struct displaymatrix *display_line;

      if (opt_noflicker && lin >= listlen)
        {
          int i = listlen;
          listlen *= 1.5;
          display = xrealloc(display, listlen * sizeof(struct displaymatrix));
          for (; i < listlen; i++)
            {
              display[i].line = xstrdup ("");
              display[i].len = 0;
              display[i].offset = 0;
              display[i].buffer_size = 0;
            }
        }

      display_line = display + lin;

      step_per_line = line->logfile->font_height + effect_y_space;
      if (step_per_line > space)
        break;

      if (!opt_reverse)
        offset -= step_per_line;

      offset += line->logfile->font_ascent;

      miny -= line->logfile->font_height;
      maxy += line->logfile->font_height;

      if (offset >= miny && offset <= maxy)
        {
          /* if this line is a different than it was, then it
           * needs displaying */
          if (!opt_noflicker
              || refresh_all
              || display_line->len != line->len
              || display_line->color != line->logfile->color
              || display_line->offset != offset
              || memcmp (display_line->line, line->line, line->len))
            {
              /* don't bother updating the record of what has been
               * displayed if -noflicker isn't in effect, since we redraw
               * the whole display every time anyway */
              if (opt_noflicker)
                {
                  /* update the record of what has been displayed;
                   * first make sure the buffer is big enough */
                  if (display_line->buffer_size < line->len)
                    {
                      display_line->buffer_size = line->len;
                      display_line->line = xrealloc (display_line->line, display_line->buffer_size);
                    }

                  display_line->len = line->len;
                  display_line->color = line->logfile->color;
                  display_line->offset = offset;
                  memcpy (display_line->line, line->line, line->len);

                  if (clear)
                    {
#ifdef DEBUG
                      static int toggle;
                      toggle = 1 - toggle;
                      XSetForeground (disp, WinGC, toggle ? GetColor ("cyan") : GetColor ("yellow"));
                      XFillRectangle (disp, root, WinGC, win_x, win_y + offset - line->logfile->font_ascent,
                                      width, step_per_line);
#else /* DEBUG */
                      XClearArea (disp, root, win_x, win_y + offset - line->logfile->font_ascent,
                                  width + MARGIN_OF_ERROR, step_per_line, False);
#endif /* DEBUG */
                    }
                }

              if (opt_outline)
                {
                  int x, y;
                  XSetForeground (disp, WinGC, black_color);

                  for (x = -1; x <= 1; x += 2)
                    for (y = -1; y <= 1; y += 2)
                      draw_text (disp, root, WinGC,
                                 win_x + effect_x_offset + x,
                                 win_y + y + offset, line, foreground = 0);
                }
              else if (opt_shade)
                {
                  XSetForeground (disp, WinGC, black_color);
                  draw_text (disp, root, WinGC,
                             win_x + effect_x_offset + SHADE_X,
                             win_y + offset + SHADE_Y, line, foreground = 0);
                }

              XSetForeground (disp, WinGC, line->logfile->color);
              draw_text (disp, root, WinGC,
                         win_x + effect_x_offset,
                         win_y + offset, line, foreground = 1);
            }
        }

      if (opt_reverse)
        offset += step_per_line;
      offset -= line->logfile->font_ascent;

      miny += line->logfile->font_height;
      maxy -= line->logfile->font_height;

      space -= step_per_line;
    }

  if (space > 0 && clear)
    {
#ifdef DEBUG
      XSetForeground (disp, WinGC, GetColor ("orange"));
      XFillRectangle (disp, root, WinGC, win_x, win_y + offset - (opt_reverse ? 0 : space),
                      width, space);
#else /* DEBUG */
      XClearArea (disp, root, win_x, win_y + offset - (opt_reverse ? 0 : space),
                  width + MARGIN_OF_ERROR, space, False);
#endif
    }

  /* at least one of the lines must fit in the allocated area.  we've
   * already checked at initialisation time that all the fonts are small
   * enough to fit at least one line in the display area, but assert it
   * again here to be sure */
  assert(line != linelist);

  /* any lines that didn't just get looked at are never going to be, so break the chain */
  if (line) line->prev->next = 0;

  /* and throw them all away */
  while (line)
    {
      struct line_node *this = line;
      line = line->next;
      if (this->logfile && this->logfile->last == this)
        this->logfile->last = NULL;
      free (this->line);
      free (this->breaks);
      free (this);
    }

  if (opt_frame)
    {
      XSetForeground (disp, WinGC, GetColor (def_color));
      /* note that XDrawRectangle() draws a rectangle one pixel bigger
       * in both dimensions than you ask for, hence the subtractions.
       * XFillRectangle() doesn't suffer from this problem */
      XDrawRectangle (disp, root, WinGC, win_x - 0, win_y - 0, width - 1, height - 1);
    }
}

#if HAS_REGEX
void void
transform_line (char *s)
{
#ifdef I_AM_Md
  int i;
  if (1)
    {
      for (i = 16; s[i]; i++)
        s[i] = s[i + 11];
    }
  s[i + 1] = '\0';
#endif

  if (transformre)
    {
      int i;
      regmatch_t matched[16];

      i = regexec (transformre, s, 16, matched, 0);
      if (i == 0)
        {                       /* matched */
          int match_start = matched[0].rm_so;
          int match_end = matched[0].rm_eo;
          int old_len = match_end - match_start;
          int new_len = strlen (transform_to);
          int old_whole_len = strlen (s);

          printf ("regexp was matched by '%s' - replace with '%s'\n", s, transform_to);
          printf ("match is from %d to %d\n", match_start, match_end);
          if (new_len > old_len)
            s = xrealloc (s, old_whole_len + new_len - old_len);

          if (new_len != old_len)
            {
              memcpy (s + match_end + new_len - old_len,
                      s + match_end,
                      old_whole_len - match_end);
              s[old_whole_len + new_len - old_len] = '\0';
            }

          memcpy (s + match_start,
                  transform_to,
                  new_len);
          printf ("transformed to '%s'\n", s);
        }
      else
        printf ("regexp was not matched by '%s'\n", s);
    }
}
#endif

/*
 * appends p2 to the end of p1, if p1 is not null
 * otherwise allocates a new string and copies p2 to it
 */
static char *
concat_line (char *p1, const char *p2)
{
  int l1 = p1 ? strlen (p1) : 0;
  int l2 = strlen (p2);
  char *r;

  assert (p2);

  if (p1)
    r = xrealloc(p1, l1 + l2 + 1);
  else
    r = xmalloc (l2 + 1);

  memcpy (r + l1, p2, l2);
  r[l1 + l2] = 0;

  return r;
}

/*
 * This routine can read a line of any length if it is called enough times.
 */
static int
lineinput (struct logfile_entry *logfile)
{
  char buff[1024], *p;
  int ch;
  /* HACK-2: add on the length of any partial line which we will be appending to */
  int ofs = logfile->buf ? strlen (logfile->buf) : 0;

  /* this loop ensures that the whole line is read, even if it's
   * longer than the buffer.  we need to do this because when --whole
   * is in effect we don't know whether to display the line or not
   * until we've seen how (ie. whether) it ends */
  do
    {
      p = buff;
      do
        {
          ch = fgetc (logfile->fp);

          if (ch == '\n' || ch == EOF)
            break;
          else if (ch == '\r')
            continue; /* skip */
          else if (ch == '\t')
            {
              do
                {
                  *p++ = ' ';
                  ofs++;
                }
              while (ofs & 7);
            }
          else
            {
              *p++ = ch;
              ofs++;
            }
        }
      while (p < buff + (sizeof buff) - 8 - 1);

      if (p == buff && ch == EOF)
        return 0;

      *p = 0;

      p = logfile->buf = concat_line (logfile->buf, buff);
    }
  while (ch != '\n' && ch != EOF);

  logfile->lastpartial = logfile->partial;
  /* there are 3 ways we could have exited the loop: reading '\n',
   * reaching EOF, or filling the buffer; the 2nd and 3rd of these
   * both result in a partial line */
  logfile->partial = ch != '\n';

  if (logfile->partial && opt_whole)
    return 0;

#if HAS_REGEX
  transform_line (logfile->buf);
#endif
  return 1;
}

/* input: reads file->fname
 * output: fills file->fp, file->inode
 * returns file->fp
 * in case of error, file->fp is NULL
 */
static FILE *
openlog (struct logfile_entry *file)
{
  struct stat stats;

  if ((file->fp = fopen (file->fname, "r")) == NULL)
    {
      file->fp = NULL;
      return NULL;
    }

  fstat (fileno (file->fp), &stats);
  if (S_ISFIFO (stats.st_mode))
    {
      if (fcntl (fileno (file->fp), F_SETFL, O_NONBLOCK) < 0)
        perror ("fcntl"), exit (1);
      file->inode = 0;
    }
  else
    file->inode = stats.st_ino;

  if (opt_noinitial)
    fseek (file->fp, 0, SEEK_END);
  else /* if (stats.st_size > (listlen + 1) * width)
        * HACK - 'width' is in pixels - how are we to know how much text will fit?
        * fseek (file->fp, -((listlen + 2) * width/10), SEEK_END); */
    fseek (file->fp, -5000, SEEK_END);

  file->last_size = stats.st_size;
  return file->fp;
}

static void
reopen (void)
{
  struct logfile_entry *e;

  for (e = loglist; e; e = e->next)
    {
      if (!e->inode)
        continue;               /* skip stdin */

      if (e->fp)
        fclose (e->fp);
      /* if fp is NULL we will try again later */
      openlog (e);
    }

  do_reopen = 0;
}

static void
check_open_files (void)
{
  struct logfile_entry *e;
  struct stat stats;

  for (e = loglist; e; e = e->next)
    {
      if (!e->inode)
        continue;               /* skip stdin */

      if (stat (e->fname, &stats) < 0)
        {                       /* file missing? */
          sleep (1);
          if (e->fp)
            fclose (e->fp);
          if (openlog (e) == NULL)
            continue;
          if (fstat (fileno (e->fp), &stats) < 0)
            continue;
        }

      if (stats.st_ino != e->inode)
        {                       /* file renamed? */
          if (e->fp)
            fclose (e->fp);
          if (openlog (e) == NULL)
            continue;
          if (fstat (fileno (e->fp), &stats) < 0)
            continue;
        }

      if (stats.st_size < e->last_size)
        {                       /* file truncated? */
          fseek (e->fp, 0, SEEK_SET);
          e->last_size = stats.st_size;
        }
    }
}

/*
 * insert a single node in the list of screen lines and return a
 * pointer to the new node.
 * the caller MUST then fill in ret->line and ret->len with valid
 * data.
 */
static struct line_node *
new_line_node (struct logfile_entry *log)
{
  struct line_node *new = xmalloc (sizeof (struct line_node));

  new->logfile = log;
  new->wrapped_left = 0;
  new->wrapped_right = 0;
  new->breaks = 0;

  assert(log);

  if (!log || !log->last)
    {
      new->next = linelist;
      new->next->prev = new;

      new->prev = NULL;
      linelist = new;
    }
  else
    {
      /* 2 pointers from the new node */
      new->next = log->last;
      new->prev = log->last->prev;

      /* 2 pointers back to the new node */
      if (new->next) new->next->prev = new;
      if (new->prev) new->prev->next = new;

      /* if this is a new first entry in the list then update
       * 'linelist' */
      if (log->last == linelist)
        linelist = new;
    }

  /* update the logfile record */
  if (log)
    log->last = new;

  return new;
}

/*
 * this is called after either adding a new line or appending to an
 * old one.  in both cases it's possible that the line no longer fits,
 * and needs wrapping.  this function checks the last line associated
 * with the supplied logfile.
 */
static void
possibly_split_long_line (struct logfile_entry *log)
{
  char *str = log->last->line;
  int l = strlen (str);
  char *p = str;
  struct line_node *line;
  int spaces;
  static struct breakinfo *breaks;
  static int break_buffer_size;

  /* only calculate the continuation's width once */
  if (continuation_width == -1)
    {
      continuation_length = strlen (continuation);
      continuation_width = XmbTextEscapement (log->fontset, continuation, continuation_length);
      continuation_color = GetColor (cont_color);

      /* make an array to store information about the location of
       * spaces in the line */
      if (opt_justify)
        {
          break_buffer_size = 32;
          breaks = xmalloc (break_buffer_size * sizeof (struct breakinfo));
        }
    }

  do
    {
      const char *beg = p;
      int start_w = log->last->wrapped_left ? continuation_width : 0;
      int w = start_w;
      int wrapped = 0;
      char *break_p = NULL;
      int width_at_break_p = 0;
      int prefix_len;

      spaces = 0;

      if (opt_justify)
        breaks[spaces].index = breaks[spaces].width = 0;

      while (*p)
        {
          int cw, len;

          /* find the length in bytes of the next multibyte character */
          len = mblen (p, l);
          if (len <= 0)
            len = 1; /* ignore (don't skip) illegal character sequences */

          /* find the width in pixels of the next character */
          cw = XmbTextEscapement (log->fontset, p, len);
          if (opt_wordwrap && len == 1 && p[0] == ' ' && p != break_p + 1)
            {
              break_p = p;
              width_at_break_p = w;
              spaces++;

              if (opt_justify)
                {
                  /* increase the size of the 'breaks' array when
                   * necessary */
                  if (spaces >= break_buffer_size)
                    {
                      break_buffer_size *= 1.5;
                      breaks = xrealloc (breaks, break_buffer_size * sizeof (struct breakinfo));
                    }

                  /* store information about (a) the location of each
                   * space */
                  breaks[spaces].index = p + 1 - beg;
                  /* (b) the width (in pixels) of the string up to
                   * this space */
                  breaks[spaces].width = cw + w - start_w;
                  /* (c) the length of each 'word' */
                  breaks[spaces-1].len = breaks[spaces].index - breaks[spaces-1].index;
                }
            }

          if (cw + w > width - effect_x_space)
            {
              if (p == beg)
                {
                  fprintf (stderr, "we can't even fit a single character onto the line\n");
                  if (len == 1) fprintf (stderr, "(the character we couldn't fit was '%c')\n", *p);
                  exit (1);
                }

              wrapped = 1;
              break;
            }

          w += cw;
          p += len;
          l -= len;
        }

      /* if we're wrapping at spaces, and the line is long enough to
       * wrap, and we've seen a space already, and the space wasn't
       * the first character on the line, then wrap at the space */
      if (!wrapped)
        break;

      /* choose where to break the line */
      if (opt_wordwrap && break_p && break_p != beg)
        {
          prefix_len = break_p - beg;
          p = break_p;
          w = width_at_break_p;

          /* if breaking at a space, skip all adjacent spaces */
          while (*p == ' ')
            {
              int len = mblen (p, l);
              if (len != 1) break;
              p++;
            }

          if (opt_justify)
            {
              spaces--;
              breaks[spaces].len--;
            }
        }
      else
        prefix_len = p - beg;

      /* make a copy of the tail end of the string */
      p = xstrdup (p);

      /* and reduce the size of the head of the string */
      log->last->line = xrealloc (log->last->line, prefix_len + 1);
      log->last->len = prefix_len;
      log->last->line[prefix_len] = '\0';

      /* note that the head was wrapped on it's right */
      log->last->wrapped_right = 1;

      /* 'spaces' includes any space we broke on; we can only justify
       * if there's at least one other space */
      if (opt_justify && spaces &&
          width - effect_x_space - width_at_break_p < spaces * log->font_height)
        {
          int i;
          log->last->free_pixels = width - effect_x_space - w;
          log->last->num_words = spaces + 1;
          log->last->breaks = malloc (log->last->num_words * sizeof (struct breakinfo));
          for (i = 0; i < log->last->num_words; i++)
            log->last->breaks[i] = breaks[i];
        }

      line = new_line_node (log);
      line->line = p;
      l = line->len = strlen (p);

      /* note that the tail end of the string is wrapped at its left */
      line->wrapped_left = 1;
    }
  while (l);
}

static void
insert_new_line (char *str, struct logfile_entry *log)
{
  struct line_node *new;
  new = new_line_node (log);
  new->line = str;
  new->len = strlen (str);

  possibly_split_long_line (log);
}

/*
 * append something to an existing physical line. this is done
 * by deleting the file on-screen, concatenating the new data to it
 * and splitting it again.
 */
static void
append_to_existing_line (char *str, struct logfile_entry *log)
{
  char *old, *new;

  assert(log);
  assert(log->last);

  old = log->last->line;
  assert(old);

  new = concat_line (old, str);
  free (str);
  log->last->line = new;
  log->last->len = strlen (new);
  possibly_split_long_line (log);
}

static void
main_loop (void)
{
  int lin;
  time_t lastreload;
  Region region = XCreateRegion ();
  XEvent xev;
  struct logfile_entry *lastprinted = NULL;
  struct logfile_entry *current;
  int need_update = 1;

  display = xmalloc (sizeof (struct displaymatrix) * listlen);

  lastreload = time (NULL);

  /* Initialize line_node */
  for (lin = 0; lin < listlen; lin++)
    {
      struct line_node *e = xmalloc (sizeof (struct line_node));
      e->line = xstrdup ("~");
      e->len = 1;
      e->logfile = loglist;     /* this is only needed to get a color for the '~' */
      e->wrapped_left = 0;
      e->wrapped_right = 0;
      e->breaks = 0;
      e->next = NULL;
      e->prev = linelist_tail;

      if (!linelist)
        linelist = e;
      if (linelist_tail)
        linelist_tail->next = e;
      linelist_tail = e;

      display[lin].line = xstrdup ("");
      display[lin].len = 0;
      display[lin].offset = 0;
      display[lin].buffer_size = 0;
    }

  for (;;)
    {
      /* read logs */
      for (current = loglist; current; current = current->next)
        {
          if (!current->fp)
            continue;           /* skip missing files */

          clearerr (current->fp);

          while (lineinput (current))
            {
              need_update = 1;
              /* if we're trying to update old partial lines in
               * place, and the last time this file was updated the
               * output was partial, and that partial line is not
               * too close to the top of the screen, then update
               * that partial line */
              if (opt_update && current->lastpartial && current->last)
                {
                  append_to_existing_line (current->buf, current);
                  current->buf = 0;
                  continue;
                }

              /* if all we just read was a newline ending a line that we've already displayed, skip it */
              if (current->buf[0] == '\0' && current->lastpartial)
                {
                  free(current->buf);
                  current->buf = 0;
                  continue;
                }

              /* print filename if any, and if last line was from
               * different file */
              if (lastprinted != current)
		{
                  current->last = 0;
		  if (!opt_nofilename && current->desc[0])
		    {
		      insert_new_line (xstrdup ("["), current);
		      append_to_existing_line (xstrdup (current->desc), current);
		      append_to_existing_line (xstrdup ("]"), current);
		    }
		}

              /* if we're dealing with partial lines, and the last
               * time we showed the line it wasn't finished ... */
              if (!opt_whole && current->lastpartial)
                {
                  /* if this is the same file we showed last then
                     append to the last line shown */
                  if (lastprinted == current)
                    append_to_existing_line (current->buf, current);
                  else
                    {
                      /* but if a different file has been shown in the
                       * mean time, make a new line, starting with the
                       * continuation string */
                      insert_new_line (current->buf, current);
                      current->last->wrapped_left = 1;
                    }
                }
              else
                /* otherwise just make a plain and simple new line */
                insert_new_line (current->buf, current);

              current->buf = 0;
              lastprinted = current;
            }
        }

      if (need_update)
        {
          redraw (0);
          need_update = 0;
        }
      else
        {
          XFlush (disp);

          if (!XPending (disp))
            {
              fd_set fdr;
              struct timeval to = interval;

              FD_ZERO (&fdr);
              FD_SET (ConnectionNumber (disp), &fdr);
              select (ConnectionNumber (disp) + 1, &fdr, 0, 0, &to);
            }
        }

      check_open_files ();

      if (do_reopen)
        reopen ();

      /* we ignore possible errors due to window resizing &c */
      while (XPending (disp))
        {
          XNextEvent (disp, &xev);

          switch (xev.type)
            {
            case Expose:
              {
                XRectangle r;

                r.x = xev.xexpose.x;
                r.y = xev.xexpose.y;
                r.width = xev.xexpose.width;
                r.height = xev.xexpose.height;

                XUnionRectWithRegion (&r, region, region);
              }
              break;
            default:
#ifdef DEBUGMODE
              fprintf (stderr, "PANIC! Unknown event %d\n", xev.type);
#endif
              break;
            }
        }

      /* reload if requested */
      if (reload && lastreload + reload < time (NULL))
        {
          if (command && command[0])
            system (command);

          reopen ();
          lastreload = time (NULL);
        }

      if (!XEmptyRegion (region))
        {
          XRectangle r;

          XSetRegion (disp, WinGC, region);
          XClipBox (region, &r);

          refresh (r.y, r.y + r.height, 0, 1);

          XDestroyRegion (region);
          region = XCreateRegion ();
        }
    }
}

int
main (int argc, char *argv[])
{
  int i;
  int opt_daemonize = 0;
  int opt_partial = 0, file_count = 0;
#if HAS_REGEX
  char *transform = NULL;
#endif

  setlocale (LC_CTYPE, "");     /* try to initialize the locale. */

  for (i = 1; i < argc; i++)
    {
      const char *arg = argv[i];

      if (arg[0] == '-' && arg[1] != '\0' && arg[1] != ',')
        {
          if (arg[1] == '-')
            arg++;

          if (!strcmp (arg, "-?") ||
              !strcmp (arg, "-help") || !strcmp (arg, "-h"))
            display_help (argv[0]);
          else if (!strcmp (arg, "-V"))
            display_version ();
          else if (!strcmp (arg, "-g") || !strcmp (arg, "-geometry"))
            geom_mask =
              XParseGeometry (argv[++i], &win_x, &win_y, &width, &height);
          else if (!strcmp (arg, "-display"))
            dispname = argv[++i];
          else if (!strcmp (arg, "-cont"))
            continuation = argv[++i];
          else if (!strcmp (arg, "-cont-color"))
            cont_color = argv[++i];
          else if (!strcmp (arg, "-font") || !strcmp (arg, "-fn"))
            fontname = argv[++i];
#if HAS_REGEX
          else if (!strcmp (arg, "-t"))
            {
              transform = argv[++i];
              transform_to = argv[++i];
              printf ("transform: '%s' to '%s'\n", transform, transform_to);
            }
#endif
          else if (!strcmp (arg, "-fork") || !strcmp (arg, "-f"))
            opt_daemonize = 1;
          else if (!strcmp (arg, "-reload"))
            {
              reload = atoi (argv[++i]);
              command = argv[++i];
            }
          else if (!strcmp (arg, "-windowed"))
            opt_windowed = 1;
          else if (!strcmp (arg, "-shade"))
            opt_shade = 1;
          else if (!strcmp (arg, "-outline"))
            opt_outline = 1;
          else if (!strcmp (arg, "-minspace"))
            opt_minspace = 1;
          else if (!strcmp (arg, "-noflicker"))
            opt_noflicker = 1;
          else if (!strcmp (arg, "-frame"))
            opt_frame = 1;
          else if (!strcmp (arg, "-no-filename"))
            opt_nofilename = 1;
          else if (!strcmp (arg, "-reverse"))
            opt_reverse = 1;
          else if (!strcmp (arg, "-whole"))
            opt_whole = 1;
          else if (!strcmp (arg, "-partial"))
            opt_partial = 1;
          else if (!strcmp (arg, "-update"))
            opt_update = opt_partial = 1;
          else if (!strcmp (arg, "-wordwrap"))
            opt_wordwrap = 1;
          else if (!strcmp (arg, "-justify"))
            opt_justify = 1;
          else if (!strcmp (arg, "-color"))
            def_color = argv[++i];
          else if (!strcmp (arg, "-noinitial"))
            opt_noinitial = 1;
          else if (!strcmp (arg, "-id"))
            {
              unsigned long id;

              if (sscanf (argv[++i], "%li", &id) == 1 && id)
                root = id;
            }
          else if (!strcmp (arg, "-interval") || !strcmp (arg, "-i"))
            {
              double iv = atof (argv[++i]);

              interval.tv_sec = (int) iv;
              interval.tv_usec = (iv - interval.tv_sec) * 1e6;
            }
          else
            {
              fprintf (stderr, "Unknown option '%s'.\n"
                       "Try --help for more information.\n", arg);
              exit (1);
            }
        }
      else
        {                       /* it must be a filename */
          struct logfile_entry *e;
          const char *fname, *desc, *fcolor = def_color;
          char *p;

          file_count++;

          /* this is not foolproof yet (',' in filenames are not allowed) */
          fname = desc = arg;
          if ((p = strchr (arg, ',')))
            {
              *p = '\0';
              fcolor = p + 1;

              if ((p = strchr (fcolor, ',')))
                {
                  *p = '\0';
                  desc = p + 1;
                }
            }

          e = xmalloc (sizeof (struct logfile_entry));
          e->partial = 0;
          e->buf = 0;

          if (arg[0] == '-' && arg[1] == '\0')
            {
              if ((e->fp = fdopen (0, "r")) == NULL)
                perror ("fdopen"), exit (1);
              if (fcntl (0, F_SETFL, O_NONBLOCK) < 0)
                perror ("fcntl"), exit (1);

              e->fname = NULL;
              e->inode = 0;
	      if (desc == arg)
		e->desc = xstrdup ("stdin");
	      else
		e->desc = xstrdup (desc);
            }
          else
            {
              e->fname = xstrdup (fname);

              if (openlog (e) == NULL)
                perror (fname), exit (1);

              e->desc = xstrdup (desc);
            }

          e->colorname = fcolor;
          e->partial = 0;
          e->fontname = fontname;
          e->last = NULL;
          e->next = NULL;

          if (!loglist)
            loglist = e;
          if (loglist_tail)
            loglist_tail->next = e;

          loglist_tail = e;
        }
    }

  if (!loglist)
    {
      fprintf (stderr, "You did not specify any files to tail\n"
               "use %s --help for help\n", argv[0]);
      exit (1);
    }

  if (opt_update && opt_whole)
    {
      fprintf (stderr, "Specify at most one of -update and -whole\n");
      exit (1);
    }
  else if (opt_partial && opt_whole)
    {
      fprintf (stderr, "Specify at most one of -partial and -whole\n");
      exit (1);
    }

  /* it doesn't make sense to justify if word wrap isn't on */
  if (opt_justify)
    opt_wordwrap = 1;

  /* HACK-7: do we want to allow both -shade and -outline? */
  if (opt_shade && opt_outline)
    {
      fprintf (stderr, "Specify at most one of -shade and -outline\n");
      exit (1);
    }

  if (opt_partial)
    /* if we specifically requested to see partial lines then don't insist on whole lines */
    opt_whole = 0;
  else if (file_count > 1)
    /* otherwise, if we're viewing multiple files, default to showing whole lines */
    opt_whole = 1;

#if HAS_REGEX
  if (transform)
    {
      int i;

      printf ("compiling regexp '%s'\n", transform);
      transformre = xmalloc (sizeof (regex_t));
      i = regcomp (transformre, transform, REG_EXTENDED);
      if (i != 0)
        {
          char buf[512];

          regerror (i, transformre, buf, sizeof (buf));
          fprintf (stderr, "Cannot compile regular expression: %s\n", buf);
        }
      else
        printf ("compiled '%s' OK to %x\n", transform, (int)transformre);
    }
#endif

  if (opt_outline && !opt_minspace)
    {
      /* adding outline increases the total width and height by 2
         pixels each, and offsets the text one pixel right and one
         pixel down */
      effect_x_space = effect_y_space = 2;
      effect_x_offset = effect_y_offset = 1;
    }
  else if (opt_shade && !opt_minspace)
    {
      /* adding a shadow increases the space used */
      effect_x_space = abs (SHADE_X);
      effect_y_space = abs (SHADE_Y);

      /* if the shadow is to the right and below then we don't need
       * to move the text to make space for it, but shadows to the left
       * and above need accomodating */
      effect_x_offset = SHADE_X > 0 ? 0 : -SHADE_X;
      effect_y_offset = SHADE_Y > 0 ? 0 : -SHADE_Y;
    }
  else
    {
      effect_x_space = effect_y_space = 0;
      effect_x_offset = effect_y_offset = 0;
    }

  InitWindow ();

  install_signal (SIGINT, blank_window);
  install_signal (SIGQUIT, blank_window);
  install_signal (SIGTERM, blank_window);
  install_signal (SIGHUP, force_reopen);
  install_signal (SIGUSR1, list_files);
  install_signal (SIGUSR2, force_refresh);

  if (opt_daemonize)
    daemonize ();

  main_loop ();

  exit (1);                     /* to make gcc -Wall stop complaining */
}

static void
install_signal (int sig, void (*handler) (int))
{
  struct sigaction action;

  action.sa_handler = handler;
  sigemptyset (&action.sa_mask);
  action.sa_flags = SA_RESTART;

  if (sigaction (sig, &action, NULL) < 0)
    fprintf (stderr, "sigaction (%d): %s\n", sig, strerror (errno)), exit (1);
}

static void *
xstrdup (const char *string)
{
  void *p;

  while ((p = strdup (string)) == NULL)
    {
      fprintf (stderr, "Memory exhausted in xstrdup ().\n");
      sleep (10);
    }

  return p;
}

static void *
xmalloc (size_t size)
{
  void *p;

  while ((p = malloc (size)) == NULL)
    {
      fprintf (stderr, "Memory exhausted in xmalloc ().\n");
      sleep (10);
    }

  return p;
}

static void *
xrealloc (void *ptr, size_t size)
{
  void *p;

  while ((p = realloc (ptr, size)) == NULL)
    {
      fprintf (stderr, "Memory exhausted in xrealloc ().\n");
      sleep (10);
    }

  return p;
}

static void
display_help (char *myname)
{
  printf ("Usage: %s [options] file1[,color[,desc]]"
          "[options] [file2[,color[,desc]] ...]\n", myname);
  printf (" -g | -geometry geometry   -g WIDTHxHEIGHT+X+Y\n"
          " -color    color           use color $color as default\n"
          " -reload sec command       reload after $sec and run command\n"
          " -id id                    window id to use instead of the root window\n"
          " -windowed                 create a window instead of writing to the root\n"
          " -font FONTSPEC            (-fn) font to use\n"
          " -f | -fork                fork into background\n"
          " -reverse                  print new lines at the top\n"
          " -whole                    wait for \\n before showing a line\n"
          " -partial                  show lines even if they don't end with a \\n\n"
          " -update                   allow updates to old partial lines\n"
          " -cont                     string to prefix continued partial lines with\n"
          "                           defaults to \"|| \"\n"
          " -wordwrap                 wrap long lines at spaces to avoid breaking words\n"
          " -shade                    add shading to font\n"
          " -outline                  add black outline to font\n"
          " -minspace                 force minimum line spacing\n"
          " -noinitial                don't display the last file lines on\n"
          "                           startup\n"
          " -i | -interval seconds    interval between checks (fractional\n"
          "                           values o.k.). Default 2.4 seconds\n"
          " -V                        display version information and exit\n"
          "\n");
  printf ("Example:\n%s -g 800x250+100+50 -font fixed /var/log/messages,green "
          "/var/log/secure,red,'ALERT'\n", myname);
  exit (0);
}

static void
display_version (void)
{
  printf ("root-tail version " VERSION "\n");
  exit (0);
}

static int
daemonize (void)
{
  pid_t pid;

  switch (pid = fork ())
    {
    case -1:
      return -1;
    case 0:
      break;
    default:
      /*printf ("%d\n", pid);*/
      exit (0);
    }

  if (setsid () == -1)
    return -1;

  return 0;
}

