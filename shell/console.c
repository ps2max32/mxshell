/* The console of a service (mx OS apps plan, step 2f-2): a service is a text
   program, and what it says (its mx_printf) is in the kernel's log with its
   process (mx-kernel 16). The shell reads the log each frame without
   waiting, keeps the last lines of the service beside it (the process
   BESIDE in the process list: mxlink), and shows them in a window. */

#include <mx/debug.h>
#include <mx/log.h>
#include <mx/proc.h>
#include <mx/string.h>

#include "shell.h"

#define LINES 64
#define COLS 80

static char lines[LINES][COLS];
static u32 nlines;          /* lines ended so far */
static char cur[COLS];      /* the one being written */
static u32 cur_len;
static u32 pos;             /* in the log */
static int service = -1;    /* its process, -1 not looked for, 0 none */
static char name[48];

/* the service beside, from the process list */
static void find_service(void) {
  struct mx_proc_info p[4];
  int n = mx_proc_list(p, 4);
  service = 0;
  name[0] = 0;
  for (int i = 0; i < n && i < 4; i++)
    if (p[i].state == MX_PROC_BESIDE) {
      service = p[i].id;
      const char *s = p[i].name;
      for (const char *q = s; *q; q++)
        if (*q == '/' || *q == ':')
          s = q + 1;
      u32 k = 0;
      for (; s[k] && k < sizeof name - 1; k++)
        name[k] = s[k];
      name[k] = 0;
    }
}

static void end_line(void) {
  cur[cur_len] = 0;
  memcpy(lines[nlines % LINES], cur, cur_len + 1);
  nlines++;
  cur_len = 0;
}

void console_poll(void) {
  if (service < 0)
    find_service();
  char b[80];
  int who, n;
  while ((n = mx_log_read(&pos, b, sizeof b, &who, 0)) > 0) {
    if (!service || who != service)
      continue;
    for (int i = 0; i < n; i++) {
      char c = b[i];
      if (c == '\n')
        end_line();
      else if (c == '\t' || (c >= ' ' && c < 127)) {
        if (cur_len == COLS - 1)
          end_line();
        cur[cur_len++] = c == '\t' ? ' ' : c;
      }
    }
  }
}

u32 console_lines(void) { return nlines; }

/* the window: the last lines that fit in (x0, y0)-(x1, y1) */
void console_draw(float x0, float y0, float x1, float y1) {
  char t[64];
  if (service > 0)
    mx_snprintf(t, sizeof t, "Console: %s (proces %d)", name, service);
  else
    mx_snprintf(t, sizeof t, "Console: geen service");
  gfx_text(FONT_BIG, x0 + 18, y0 + 38, t, RGB(0xF4F4F0u), 0.6f);
  const float step = 20, scale = 0.8f;
  u32 fit = (u32)((y1 - y0 - 76) / step), show = nlines < fit ? nlines : fit;
  float y = y0 + 70;
  for (u32 i = nlines - show; i < nlines; i++, y += step) {
    /* cut to the window's width */
    const char *l = lines[i % LINES];
    char c[COLS];
    u32 k = 0;
    for (; l[k]; k++) {
      c[k] = l[k];
      c[k + 1] = 0;
      if (gfx_width(FONT_SMALL, c, scale) > x1 - x0 - 36) {
        c[k] = 0;
        break;
      }
    }
    c[k] = 0;
    gfx_text(FONT_SMALL, x0 + 18, y, c, RGB(0xC8D8DCu), scale);
  }
  if (!nlines)
    gfx_text(FONT_SMALL, x0 + 18, y, service > 0 ? "(nog niets)" : "", RGB(0x98A8ACu), scale);
}
