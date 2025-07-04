#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

/*********** defines   *****************/

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  PAGE_UP,
  PAGE_DOWN,
  HOME_KEY,
  END_KEY,
  DEL_KEY
};

/*********** data   *****************/

typedef struct erow {
  int size;
  char *chars;
} erow;

struct editorConfig {
  int cx, cy;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  char *highlight_query;
  int sel_start_x;
  int sel_start_y;
  int selecting;
  char *clipboard;
  struct termios orig_termios;
};

struct editorConfig E;

/*********** append buffer   *****************/
struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(new + ab->len, s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}


/*********** terminal   *****************/

// if a system call fails, print the error message and exit the program
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(s);
  exit(1);
}

int editorReadKey(void) {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

void disableRawMode(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode(void) {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);
  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}


int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    return -1;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

static char *strcasestr_impl(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            const char *h, *n;
            for (h = haystack, n = needle; *h && *n; h++, n++) {
                if (tolower((unsigned char)*h) != tolower((unsigned char)*n)) {
                    break;
                }
            }
            if (*n == '\0') {
                return (char *)haystack;
            }
        }
    }
    return NULL;
}

int editorLineNumberWidth(void) {
  int digits = 1;
  int max_line = E.numrows > 0 ? E.numrows : 1;
  while (max_line >= 10) {
    max_line /= 10;
    digits++;
  }
  return digits + 1; // +1 for space after line number
}

/*********** output   *****************/

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

void editorScroll(void) {
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  
  int available_width = E.screencols - editorLineNumberWidth();
  if (E.cx < E.coloff) {
    E.coloff = E.cx;
  }
  if (E.cx >= E.coloff + available_width) {
    E.coloff = E.cx - available_width + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  int y;
  int line_num_width = editorLineNumberWidth();

  // Determine the selection start and end points, regardless of cursor direction
  int start_y, start_x, end_y, end_x;
  int selection_is_active = E.selecting;
  if (selection_is_active) {
    if (E.sel_start_y < E.cy || (E.sel_start_y == E.cy && E.sel_start_x <= E.cx)) {
      start_y = E.sel_start_y;
      start_x = E.sel_start_x;
      end_y = E.cy;
      end_x = E.cx;
    } else {
      start_y = E.cy;
      start_x = E.cx;
      end_y = E.sel_start_y;
      end_x = E.sel_start_x;
    }
  }

  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    
    // Draw line number
    char line_num[16];
    if (filerow >= E.numrows) {
      snprintf(line_num, sizeof(line_num), "%*s", line_num_width, "~");
    } else {
      snprintf(line_num, sizeof(line_num), "%*d ", line_num_width - 1, filerow + 1);
    }
    abAppend(ab, line_num, line_num_width);
    
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome), "Cilo editor -- version 0.0.1");
        int available_width = E.screencols - line_num_width;
        if (welcomelen > available_width) welcomelen = available_width;
        int padding = (available_width - welcomelen) / 2;
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      }
    } else {
      erow *row = &E.row[filerow];
      int available_width = E.screencols - line_num_width;
      
      // Highlighting Logic
      int is_row_selected = selection_is_active && (filerow >= start_y && filerow <= end_y);
      int sel_start_col = is_row_selected ? (filerow == start_y ? start_x : 0) : -1;
      int sel_end_col = is_row_selected ? (filerow == end_y ? end_x : row->size) : -1;
      
      // Selection highlighting overrides search highlighting
      char *highlight_search = E.highlight_query;
      if (is_row_selected) {
        highlight_search = NULL;
      }
      
      int visible_len = row->size - E.coloff;
      if (visible_len < 0) visible_len = 0;
      if (visible_len > available_width) visible_len = available_width;
      char *visible_start = row->chars + E.coloff;

      if (!is_row_selected && !highlight_search) {
        abAppend(ab, visible_start, visible_len);
      } else {
        // Find intersection of visible area and selection area
        int vis_sel_start = sel_start_col != -1 ? (E.coloff > sel_start_col ? E.coloff : sel_start_col) : -1;
        int vis_sel_end = sel_end_col != -1 ? ((E.coloff + visible_len) < sel_end_col ? (E.coloff + visible_len) : sel_end_col) : -1;

        if (is_row_selected && vis_sel_start < vis_sel_end) {
          // Draw with selection highlighting
          int pre_len = vis_sel_start - E.coloff;
          if (pre_len > 0) abAppend(ab, visible_start, pre_len);
          
          abAppend(ab, "\x1b[7m", 4);
          abAppend(ab, row->chars + vis_sel_start, vis_sel_end - vis_sel_start);
          abAppend(ab, "\x1b[m", 3);

          int post_start_offset = vis_sel_end - E.coloff;
          int post_len = visible_len - post_start_offset;
          if (post_len > 0) abAppend(ab, visible_start + post_start_offset, post_len);
        } else if (highlight_search) {
          // Draw with search highlighting
            char *current = visible_start;
            int query_len = strlen(highlight_search);
            char *end_of_visible = visible_start + visible_len;
            
            while(current < end_of_visible) {
                char *match = strcasestr_impl(current, highlight_search);
                if (match && match < end_of_visible) {
                    abAppend(ab, current, match - current);
                    abAppend(ab, "\x1b[7m", 4);
                    int match_len = (match + query_len > end_of_visible) ? (end_of_visible - match) : query_len;
                    abAppend(ab, match, match_len);
                    abAppend(ab, "\x1b[m", 3);
                    current = match + query_len;
                } else {
                    abAppend(ab, current, end_of_visible - current);
                    break;
                }
            }
        } else {
            abAppend(ab, visible_start, visible_len);
        }
      }
    }
    
    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines",
    E.filename ? E.filename : "[No Name]", E.numrows);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
    E.cy + 1, E.numrows);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen(void) {
  editorScroll();
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  int line_num_width = editorLineNumberWidth();
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.cx - E.coloff) + line_num_width + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*********** file i/o *****************/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++) {
    totlen += E.row[j].size + 1;
  }
  *buflen = totlen;
  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }
  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
                           line[linelen - 1] == '\r'))
      linelen--;
    
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    int at = E.numrows;
    E.row[at].size = linelen;
    E.row[at].chars = malloc(linelen + 1);
    memcpy(E.row[at].chars, line, linelen);
    E.row[at].chars[linelen] = '\0';
    E.numrows++;
  }
  free(line);
  fclose(fp);
}

void editorSave(void) {
  if (E.filename == NULL) return;
  int len;
  char *buf = editorRowsToString(&len);
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }
  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*********** editor operations *****************/

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;
  
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
  
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numrows++;
}

void editorInsertNewline(void) {
  if (E.cy >= E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    if (E.cx == 0) {
      editorInsertRow(E.cy, "", 0);
    } else if (E.cx >= row->size) {
      editorInsertRow(E.cy + 1, "", 0);
    } else {
      editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
      row = &E.row[E.cy];
      row->size = E.cx;
      row->chars[row->size] = '\0';
    }
  }
  E.cy++;
  E.cx = 0;
}

void editorInsertChar(int c) {
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }

  erow *row = &E.row[E.cy];
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[E.cx + 1], &row->chars[E.cx], row->size - E.cx + 1);
  row->size++;
  row->chars[E.cx] = c;
  E.cx++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  free(E.row[at].chars);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
}

void editorDelChar(void) {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;

  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    // Normal character deletion
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    // At beginning of line - join with previous line
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

char *editorGetSelection(size_t *buflen) {
    if (!E.selecting) return NULL;

    int start_y, start_x, end_y, end_x;
    if (E.sel_start_y < E.cy || (E.sel_start_y == E.cy && E.sel_start_x <= E.cx)) {
        start_y = E.sel_start_y;
        start_x = E.sel_start_x;
        end_y = E.cy;
        end_x = E.cx;
    } else {
        start_y = E.cy;
        start_x = E.cx;
        end_y = E.sel_start_y;
        end_x = E.sel_start_x;
    }

    if (start_y < 0 || end_y >= E.numrows) return NULL;

    struct abuf ab = ABUF_INIT;

    if (start_y == end_y) {
        erow *row = &E.row[start_y];
        if (start_x >= row->size || start_x >= end_x) {
             abFree(&ab);
             return NULL;
        }
        int len = end_x - start_x;
        if (len > row->size - start_x) len = row->size - start_x;
        abAppend(&ab, &row->chars[start_x], len);
    } else {
        erow *row = &E.row[start_y];
        int len = row->size - start_x;
        if (len > 0) abAppend(&ab, &row->chars[start_x], len);
        abAppend(&ab, "\n", 1);

        for (int i = start_y + 1; i < end_y; i++) {
            row = &E.row[i];
            abAppend(&ab, row->chars, row->size);
            abAppend(&ab, "\n", 1);
        }

        row = &E.row[end_y];
        if (end_x > 0) {
             if (end_x > row->size) end_x = row->size;
             abAppend(&ab, row->chars, end_x);
        }
    }
    
    if (buflen) *buflen = ab.len;
    return ab.b;
}

void editorDeleteSelection(void) {
    if (!E.selecting) return;

    int start_y, start_x, end_y, end_x;
    if (E.sel_start_y < E.cy || (E.sel_start_y == E.cy && E.sel_start_x <= E.cx)) {
        start_y = E.sel_start_y;
        start_x = E.sel_start_x;
        end_y = E.cy;
        end_x = E.cx;
    } else {
        start_y = E.cy;
        start_x = E.cx;
        end_y = E.sel_start_y;
        end_x = E.sel_start_x;
    }
    
    if (start_y < 0 || end_y >= E.numrows) return;

    if (start_y == end_y) {
        erow *row = &E.row[start_y];
        if (start_x >= row->size || start_x >= end_x) return;
        int len = end_x - start_x;
        if (len > row->size - start_x) len = row->size - start_x;
        
        memmove(&row->chars[start_x], &row->chars[start_x + len], row->size - start_x - len + 1);
        row->size -= len;
    } else {
        erow *first_row = &E.row[start_y];
        erow *last_row = &E.row[end_y];
        
        char *last_line_remainder = &last_row->chars[end_x];
        int remainder_len = last_row->size - end_x;
        
        first_row->size = start_x;
        first_row->chars[first_row->size] = '\0';
        editorRowAppendString(first_row, last_line_remainder, remainder_len);

        for (int i = start_y + 1; i <= end_y; i++) {
            editorDelRow(start_y + 1);
        }
    }
    
    E.cy = start_y;
    E.cx = start_x;
    E.selecting = 0;
}

/*********** input   *****************/
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';
  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();
    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      if (callback) callback(buf, c);
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage("");
        if (callback) callback(buf, c);
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
    if (callback) callback(buf, c);
  }
}

void editorFindCallback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;

  free(E.highlight_query);
  E.highlight_query = strdup(query);

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1) direction = 1;
  int current = last_match;
  int i;
  for (i = 0; i < E.numrows; i++) {
    current += direction;
    if (current == -1) current = E.numrows - 1;
    else if (current == E.numrows) current = 0;
    
    erow *row = &E.row[current];
    char *match = strcasestr_impl(row->chars, query);
    if (match) {
      last_match = current;
      E.cy = current;
      E.cx = match - row->chars;
      E.rowoff = E.numrows;
      break;
    }
  }
}

void editorFind(void) {
  int saved_cx = E.cx;
  int saved_cy = E.cy;
  int saved_rowoff = E.rowoff;

  char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);
  
  free(E.highlight_query);
  E.highlight_query = NULL;

  if (query) {
    free(query);
  } else {
    E.cx = saved_cx;
    E.cy = saved_cy;
    E.rowoff = saved_rowoff;
  }
}

void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx++;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows) {
        E.cy++;
      }
      break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void editorProcessKeypress(void) {
  int c = editorReadKey();

  switch (c) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case CTRL_KEY('f'):
      editorFind();
      break;

    case '\r':
      editorInsertNewline();
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
      editorDelChar();
      break;

    case DEL_KEY:
      {
        if (E.cy >= E.numrows) break;
        erow *row = &E.row[E.cy];

        if (E.cx < row->size) {
          editorRowDelChar(row, E.cx);
        } else if (E.cy < E.numrows - 1) {
          erow *next_row = &E.row[E.cy + 1];
          editorRowAppendString(row, next_row->chars, next_row->size);
          editorDelRow(E.cy + 1);
        }
      }
      break;

    case HOME_KEY:
      E.cx = 0;
      break;
    
    case END_KEY:
      if (E.cy < E.numrows)
        E.cx = E.row[E.cy].size;
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        int times = E.screenrows;
        while (times--)
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
    
    case CTRL_KEY('c'): // Copy
      if (E.selecting) {
        free(E.clipboard);
        E.clipboard = editorGetSelection(NULL);
        E.selecting = 0;
        editorSetStatusMessage("Copied selection to clipboard");
      } else if (E.cy < E.numrows) {
        free(E.clipboard);
        E.clipboard = strdup(E.row[E.cy].chars);
        editorSetStatusMessage("Copied line to clipboard");
      }
      break;

    case CTRL_KEY('x'): // Cut
      if (E.selecting) {
        free(E.clipboard);
        E.clipboard = editorGetSelection(NULL);
        editorDeleteSelection(); // Deletes and turns off selection
        editorSetStatusMessage("Cut selection to clipboard");
      } else if (E.cy < E.numrows) {
        free(E.clipboard);
        E.clipboard = strdup(E.row[E.cy].chars);
        editorDelRow(E.cy);
        editorSetStatusMessage("Cut line to clipboard");
      }
      break;

    case CTRL_KEY('v'): // Paste
      if (E.selecting) {
        editorDeleteSelection(); // If something is selected, paste replaces it
      }
      if (E.clipboard) {
        char *s = E.clipboard;
        while (*s) {
          if (*s == '\n' || *s == '\r') {
            editorInsertNewline();
          } else {
            editorInsertChar(*s);
          }
          s++;
        }
        editorSetStatusMessage("Pasted from clipboard");
      }
      break;

    case CTRL_KEY('b'): // Begin/End selection
      if (E.selecting) {
        E.selecting = 0;
        editorSetStatusMessage("Selection mode OFF");
      } else {
        E.selecting = 1;
        E.sel_start_x = E.cx;
        E.sel_start_y = E.cy;
        editorSetStatusMessage("Selection mode ON. Press ESC to cancel.");
      }
      break;

    case '\x1b': // Escape key
      if (E.selecting) {
        E.selecting = 0;
        editorSetStatusMessage("Selection cancelled");
      }
      break;

    default:
      editorInsertChar(c);
      break;
  }
}
/*** init ***/

void initEditor(void) {
  E.cx = 0;
  E.cy = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.highlight_query = NULL;
  E.sel_start_x = -1;
  E.sel_start_y = -1;
  E.selecting = 0;
  E.clipboard = NULL;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}