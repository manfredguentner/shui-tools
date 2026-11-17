/* Copyright (c) 2026, Manfred Güntner
   SPDX-License-Identifier: BSD-2-Clause */

/* shedit.c a textarea for terminals */
/* The UTF-8 support RFC-3629 and Markus Kuhn wcwidth.c was created
   with the help of AI */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <ctype.h>
#include <stdint.h>


#define INITIAL_CAPACITY 4096

/* global scope vars */
char lbuf[2048];
int lbuflen;
int EDITOR_HEIGHT = 3;

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
} Buffer;

typedef struct {
  uint32_t first;
  uint32_t last;
} Utf8Interval;

typedef struct {
  int fd;
  unsigned char buf[256];
  size_t pos;
  size_t len;
} InputBuf;

Buffer* init_buffer(void) {
  Buffer *b = (Buffer*)malloc(sizeof(Buffer));
  b->capacity = INITIAL_CAPACITY;
  b->data = (char*)malloc(b->capacity);
  b->data[0] = '\0';
  b->length = 0;
  return b;
}

void ensure_capacity(Buffer *b, size_t needed) {
  if (b->length + needed + 1 > b->capacity) {
    while (b->length + needed + 1 > b->capacity) {
      b->capacity *= 2;
    }
    b->data = (char*)realloc(b->data, b->capacity);
  }
}

void insert_at(Buffer *b, size_t pos, const char *str) {
  size_t str_len = strlen(str);
  ensure_capacity(b, str_len);
  memmove(b->data + pos + str_len, b->data + pos, b->length - pos + 1);
  memcpy(b->data + pos, str, str_len);
  b->length += str_len;
  b->data[b->length] = '\0';
}

static void buf_append(Buffer *b, const char *s, size_t len) {
  ensure_capacity(b, len);
  memcpy(b->data + b->length, s, len);
  b->length += len;
  b->data[b->length] = '\0';
}

static void write_all(int fd, const char *p, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, p + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    off += (size_t)n;
  }
}

static int in_intervals(uint32_t cp, const Utf8Interval *table, size_t n) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (cp < table[mid].first) hi = mid;
    else if (cp > table[mid].last) lo = mid + 1;
    else return 1;
  }
  return 0;
}

static int utf8_decode(const char *s, size_t len, size_t *pos, uint32_t *cp_out)
{
  /* strict RFC-3629-UTF-8 decoding with lenght validation
     return: 1 = valid, 0 = invalid (*cp_out = 0xFFFD, *pos moves 1 further)
  */

  if (*pos >= len) {
    *cp_out = 0;
    return 0;
  }
  const unsigned char *u = (const unsigned char *)s + *pos;
  size_t rem = len - *pos;
  unsigned char b0 = u[0];

  if (b0 < 0x80) {
    (*pos)++;
    *cp_out = b0;
    return 1;
  }
  if (b0 >= 0xC2 && b0 <= 0xDF && rem >= 2) {
    if ((u[1] & 0xC0) == 0x80) {
      *pos += 2;
      *cp_out = ((uint32_t)(b0 & 0x1F) << 6) | (u[1] & 0x3F);
      return 1;
    }
  } else if (b0 >= 0xE0 && b0 <= 0xEF && rem >= 3) {
    unsigned char b1 = u[1], b2 = u[2];
    int b1_ok = (b0 == 0xE0) ? (b1 >= 0xA0 && b1 <= 0xBF) :
                (b0 == 0xED) ? (b1 >= 0x80 && b1 <= 0x9F) :
                               ((b1 & 0xC0) == 0x80);
    if (b1_ok && (b2 & 0xC0) == 0x80) {
      *pos += 3;
      *cp_out = ((uint32_t)(b0 & 0x0F) << 12) |
                ((uint32_t)(b1 & 0x3F) << 6)  | (b2 & 0x3F);
      return 1;
    }
  } else if (b0 >= 0xF0 && b0 <= 0xF4 && rem >= 4) {
    unsigned char b1 = u[1], b2 = u[2], b3 = u[3];
    int b1_ok = (b0 == 0xF0) ? (b1 >= 0x90 && b1 <= 0xBF) :
                (b0 == 0xF4) ? (b1 >= 0x80 && b1 <= 0x8F) :
                               ((b1 & 0xC0) == 0x80);
    if (b1_ok && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
      *pos += 4;
      *cp_out = ((uint32_t)(b0 & 0x07) << 18) |
                ((uint32_t)(b1 & 0x3F) << 12) |
                ((uint32_t)(b2 & 0x3F) << 6)  | (b3 & 0x3F);
      return 1;
    }
  }

  (*pos)++;
  *cp_out = 0xFFFD;
  return 0;
}

static int is_combining_mark(uint32_t cp) {
  static const Utf8Interval combining[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0900, 0x0902}, {0x093A, 0x093A},
    {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D}, {0x0951, 0x0957},
    {0x0962, 0x0963}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},
    {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F}, {0xE0100, 0xE01EF}
  };
  return in_intervals(cp, combining, sizeof(combining) / sizeof(combining[0]));
}

static int is_zero_width_format(uint32_t cp) {
  return cp == 0x061C ||
         (cp >= 0x200B && cp <= 0x200F) ||
         (cp >= 0x202A && cp <= 0x202E) ||
         (cp >= 0x2060 && cp <= 0x206F) ||
         cp == 0xFEFF;
}

static int codepoint_width(uint32_t cp) {
  static const Utf8Interval wide[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
    {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
    {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
    {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE},
    {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
    {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
    {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
    {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E},
    {0x3040, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF},
    {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF01, 0xFF60}, {0xFFE0, 0xFFE6},
    {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F200, 0x1F2FF}, {0x1F300, 0x1F64F},
    {0x1F680, 0x1F6FF}, {0x1F900, 0x1FAFF}, {0x20000, 0x2FFFD},
    {0x30000, 0x3FFFD}
  };
  if (cp == 0 || cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
  if (is_combining_mark(cp) || is_zero_width_format(cp)) return 0;
  if ((cp >= 0x1160 && cp <= 0x11FF) || 
      (cp >= 0xD7B0 && cp <= 0xD7FF)) {
    return 0;
  }

  if (in_intervals(cp, wide, sizeof(wide) / sizeof(wide[0]))) return 2;
  return 1;
}

static int is_emoji_base(uint32_t cp) {
  return cp == 0x00A9 || cp == 0x00AE || cp == 0x203C || cp == 0x2049 ||
         cp == 0x2122 || cp == 0x2139 || (cp >= 0x2194 && cp <= 0x21AA) ||
         (cp >= 0x231A && cp <= 0x23FF) || cp == 0x24C2 ||
         (cp >= 0x25AA && cp <= 0x27BF) || cp == 0x2934 || cp == 0x2935 ||
         (cp >= 0x2B05 && cp <= 0x2B55) || cp == 0x3030 || cp == 0x303D ||
         cp == 0x3297 || cp == 0x3299 ||
         (cp >= 0x1F000 && cp <= 0x1FAFF);
}

static int is_hangul_v(uint32_t cp) {
  return (cp >= 0x1160 && cp <= 0x11A7) || (cp >= 0xD7B0 && cp <= 0xD7C6);
}

static int is_hangul_t(uint32_t cp) {
  return (cp >= 0x11A8 && cp <= 0x11FF) || (cp >= 0xD7CB && cp <= 0xD7FB);
}

static size_t utf8_next_grapheme(const char *s, size_t len, 
                                 size_t pos, int *width_out) {
  /* returns byte-length of the next graphem-cluster from pos on
     and optional its terminal-width in *width_out
  */

  if (pos >= len) {
    if (width_out) *width_out = 0;
    return 0;
  }

  size_t p = pos;
  uint32_t cp0 = 0;
  if (!utf8_decode(s, len, &p, &cp0)) {
    if (width_out) *width_out = 1; /* render as U+FFFD */
    return p - pos;
  }

  if (cp0 == '\n') {
    if (width_out) *width_out = 0;
    return p - pos;
  }

  int w = codepoint_width(cp0);
  int is_ri = (cp0 >= 0x1F1E6 && cp0 <= 0x1F1FF);
  int can_zwj = is_emoji_base(cp0);
  int in_zwj = 0;
  uint32_t prev_cp = cp0;

  while (p < len) {
    size_t next_p = p;
    uint32_t cp = 0;
    if (!utf8_decode(s, len, &next_p, &cp) || cp == '\n') break;

    if (in_zwj) {
      if (is_emoji_base(cp)) {
        p = next_p;
        in_zwj = 0;
        w = 2;
        prev_cp = cp;
        continue;
      }
      break;
    }
    if (cp == 0x200D && can_zwj) { /* ZWJ in Emoji-Sequenz */
      p = next_p;
      in_zwj = 1;
      prev_cp = cp;
      continue;
    }
    if (cp == 0xFE0F) { /* Emoji display */
      p = next_p;
      if (w == 1 && is_emoji_base(prev_cp)) w = 2;
      prev_cp = cp;
      continue;
    }
    if (cp >= 0x1F3FB && cp <= 0x1F3FF && can_zwj) { /* Skin Tone Modifier */
      p = next_p;
      w = 2;
      prev_cp = cp;
      continue;
    }
    if (cp == 0x20E3) { /* Combining Enclosing Keycap (z. B. 1️⃣) */
      p = next_p;
      w = 2;
      prev_cp = cp;
      continue;
    }
    if (is_combining_mark(cp)) {
      p = next_p;
      prev_cp = cp;
      continue;
    }
    if (is_ri && cp >= 0x1F1E6 && cp <= 0x1F1FF) { /* Flag pairs */
      p = next_p;
      is_ri = 0;
      w = 2;
      prev_cp = cp;
      continue;
    }
    if (((prev_cp >= 0x1100 && prev_cp <= 0x115F) ||
         (prev_cp >= 0xA960 && prev_cp <= 0xA97C)) &&
        is_hangul_v(cp)) {
      p = next_p;
      prev_cp = cp;
      continue;
    }
    if ((is_hangul_v(prev_cp) && (is_hangul_v(cp) || is_hangul_t(cp))) ||
        (is_hangul_t(prev_cp) && is_hangul_t(cp))) {
      p = next_p;
      prev_cp = cp;
      continue;
    }
    break;
  }

  if (width_out) *width_out = w;
  return p - pos;
}

static size_t utf8_prev_grapheme(const char *s, size_t line_start, size_t pos) {
  /* returns the start position of the graphem-cluster right before pos */

  if (pos <= line_start) return line_start;
  if ((unsigned char)s[pos - 1] < 0x80) return pos - 1;

  size_t scan = pos - 1;
  while (scan > line_start && (unsigned char)s[scan - 1] >= 0x80) {
    scan--;
  }
  if (scan > line_start) scan--;

  size_t last = scan;
  while (scan < pos) {
    size_t glen = utf8_next_grapheme(s, pos, scan, NULL);
    if (glen == 0) break;
    last = scan;
    scan += glen;
  }
  return last;
}

static int get_col_at(const char *s, size_t len, 
                      size_t line_start, size_t byte_pos) {

  int col = 0;
  size_t p = line_start;
  while (p < byte_pos) {
    int w = 0;
    size_t glen = utf8_next_grapheme(s, len, p, &w);
    if (glen == 0) break;
    col += w;
    p += glen;
  }
  return col;
}

static size_t clamp_byte_pos(const Buffer *b, size_t byte_pos) {
  /* keeps byte_pos after external editing within buffer and 
     on a graphem border 
  */

  if (byte_pos > b->length) return b->length;
  size_t ls = byte_pos;
  while (ls > 0 && b->data[ls - 1] != '\n') ls--;
  size_t p = ls;
  while (p < byte_pos) {
    size_t glen = utf8_next_grapheme(b->data, b->length, p, NULL);
    if (glen == 0 || p + glen > byte_pos) break;
    p += glen;
  }
  return p;
}

static void append_grapheme_safe(Buffer *frame, const char *s, size_t len,
                                 size_t pos, size_t glen) {
  size_t tmp = pos;
  uint32_t cp = 0;
  if (!utf8_decode(s, len, &tmp, &cp)) {
    buf_append(frame, "\xEF\xBF\xBD", 3); /* U+FFFD */
    return;
  }
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
    return; /* Keine rohen Steuerzeichen ans Terminal schicken */
  }
  buf_append(frame, s + pos, glen);
}

static int in_has_data(InputBuf *in) {
  if (in->pos < in->len) return 1;
  fd_set fds;
  struct timeval tv = {0, 0};
  int r;
  do {
    FD_ZERO(&fds);
    FD_SET(in->fd, &fds);
    r = select(in->fd + 1, &fds, NULL, NULL, &tv);
  } while (r == -1 && errno == EINTR);
  return r > 0;
}

static int in_read_byte(InputBuf *in, unsigned char *out) {
  if (in->pos < in->len) {
    *out = in->buf[in->pos++];
    return 1;
  }
  ssize_t n;
  do {
    n = read(in->fd, in->buf, sizeof(in->buf));
  } while (n == -1 && errno == EINTR);
  if (n <= 0) return (int)n;
  in->pos = 1;
  in->len = (size_t)n;
  *out = in->buf[0];
  return 1;
}

static int in_wait_byte(InputBuf *in, unsigned char *out, int timeout_ms) {
  if (in->pos < in->len) {
    *out = in->buf[in->pos++];
    return 1;
  }
  fd_set fds;
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  int r;
  do {
    FD_ZERO(&fds);
    FD_SET(in->fd, &fds);
    r = select(in->fd + 1, &fds, NULL, NULL, &tv);
  } while (r == -1 && errno == EINTR);
  if (r <= 0) return 0;
  return in_read_byte(in, out);
}

void render(int td, Buffer *frame, Buffer *b, int start_row, int start_col,
            size_t byte_pos, int term_width, const char *sprompt, 
            int draw_status, int bf) {

  frame->length = 0;

  int row = 0;
  int col = 0;
  int view_width = term_width - start_col + 1;
  if (view_width < 1) view_width = 1;

  /* 1. calculate the absolute visual cursor position (row and col in text) */
  size_t it = 0;
  while (it < byte_pos && it < b->length) {
    if (b->data[it] == '\n') {
      row++;
      col = 0;
      it++;
    } else {
      int w = 0;
      size_t glen = utf8_next_grapheme(b->data, byte_pos, it, &w);
      if (glen == 0) break;
      col += w;
      it += glen;
    }
  }

  int top_line = 0;
  if (row >= EDITOR_HEIGHT) {
    top_line = row - EDITOR_HEIGHT + 1;
  }

  /* 2. Horizontal Scroll-Offset */
  int col_offset = 0;
  if (col >= view_width) {
    col_offset = col - view_width + 1;
  }

  /* start synchronized update. hide cursor while redraw */
  buf_append(frame, "\033[?2026h\033[?25l", 14);

  /* determine start of top_line only once */
  size_t cur_line_start = 0;
  int line = 0;
  while (line < top_line && cur_line_start < b->length) {
    if (b->data[cur_line_start] == '\n') line++;
    cur_line_start++;
  }

  /* 3. render rows */
  int r;
  for (r = 0; r < EDITOR_HEIGHT; r++) {
    size_t line_start = cur_line_start;
    size_t line_end = line_start;
    while (line_end < b->length && b->data[line_end] != '\n') {
      line_end++;
    }
    cur_line_start = (line_end < b->length) ? (line_end + 1) : b->length;

    lbuflen = sprintf(lbuf, "\033[%d;%dH", start_row + r, start_col);
    buf_append(frame, lbuf, (size_t)lbuflen);

    size_t current_byte = line_start;
    int current_visual_col = 0;

    while (current_byte < line_end && current_visual_col < col_offset) {
      int w = 0;
      size_t glen = utf8_next_grapheme(b->data, line_end, current_byte, &w);
      if (glen == 0) break;
      current_visual_col += w;
      current_byte += glen;
    }

    int remaining_to_write = view_width;

    if (current_visual_col > col_offset) {
      int pad = current_visual_col - col_offset;
      while (pad > 0 && remaining_to_write > 0) {
        buf_append(frame, " ", 1);
        remaining_to_write--;
        pad--;
      }
    }

    size_t write_ptr = current_byte;
    while (write_ptr < line_end && remaining_to_write > 0) {
      int w = 0;
      size_t glen = utf8_next_grapheme(b->data, line_end, write_ptr, &w);
      if (glen == 0) break;

      if (remaining_to_write >= w) {
        append_grapheme_safe(frame, b->data, line_end, write_ptr, glen);
        write_ptr += glen;
        remaining_to_write -= w;
      } else {
        while (remaining_to_write > 0) {
          buf_append(frame, " ", 1);
          remaining_to_write--;
        }
        break;
      }
    }

    buf_append(frame, "\033[K", 3);
  }

  /* 4. Status display (only on init or after external editor) */
  if (draw_status) {
    if (bf == 0) {
      lbuflen = sprintf(lbuf, "\033[%d;%dH\033[2m", 
                        start_row + EDITOR_HEIGHT, start_col);
    } else {
      lbuflen = sprintf(lbuf, "\033[%d;%dH\033[7m", 
                        start_row + EDITOR_HEIGHT, start_col);
    }
    buf_append(frame, lbuf, (size_t)lbuflen);

    size_t plen = strlen(sprompt);
    size_t p = 0;
    int pcol = 0;
    while (p < plen) {
      int w = 0;
      size_t glen = utf8_next_grapheme(sprompt, plen, p, &w);
      if (glen == 0 || pcol + w > view_width) break;
      append_grapheme_safe(frame, sprompt, plen, p, glen);
      pcol += w;
      p += glen;
    }
    buf_append(frame, "\033[0m\033[K", 7);
  }

  /* 5. Set cursor position, shoow cursor again & end update */
  int final_row = start_row + (row - top_line);
  int final_col = start_col + (col - col_offset);

  lbuflen = sprintf(lbuf, "\033[%d;%dH\033[?25h\033[?2026l", final_row, 
                    final_col);
  buf_append(frame, lbuf, (size_t)lbuflen);

  write_all(td, frame->data, frame->length);
}

void get_cursor_position(int fd, int *row, int *col) {
  char buf[32];
  int idx = 0;
  struct termios old_termios;
  tcgetattr(fd, &old_termios);
  struct termios new_termios = old_termios;
  new_termios.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(fd, TCSANOW, &new_termios);

  lbuflen = sprintf(lbuf, "\033[6n");
  write_all(fd, lbuf, (size_t)lbuflen);

  while (idx < (int)sizeof(buf) - 1) {
    if (read(fd, buf + idx, 1) != 1) break;
    if (buf[idx] == 'R') {
      idx++;
      break;
    }
    idx++;
  }
  buf[idx] = '\0';
  tcsetattr(fd, TCSANOW, &old_termios);

  if (sscanf(buf, "\033[%d;%dR", row, col) != 2) {
    *row = 1;
    *col = 1;
  }
}

int call_editor(Buffer *buf, int tty_fd, const struct termios *orig_termios) {
  char filename[64];
  int fd;
  const char *editor;
  struct termios saved_termios;
  int has_saved_termios;
  pid_t pid;
  int status;
  pid_t w;
  char tmp[4096];
  ssize_t fbr;

  sprintf(filename, "/tmp/shtext-%lu.tmp", (unsigned long)getpid());

  fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd == -1) {
    perror("open");
    return -1;
  }

  if (buf->data != NULL) {
    size_t to_write = buf->length;
    size_t written = 0;
    while (written < to_write) {
      ssize_t n = write(fd, buf->data + written, to_write - written);
      if (n == -1) {
        if (errno == EINTR) continue;
        perror("write");
        close(fd);
        unlink(filename);
        return -1;
      }
      written += (size_t)n;
    }
  }

  close(fd);

  editor = getenv("EDITOR");
  if (editor == NULL || editor[0] == '\0') {
    editor = "vi";
  }

  /* Empty buffered output before fork() */
  fflush(stdout);

  /* save current (raw-)mode of tty and restore normal mode */
  has_saved_termios = (tcgetattr(tty_fd, &saved_termios) == 0);
  if (orig_termios != NULL) {
    tcsetattr(tty_fd, TCSADRAIN, orig_termios);
  }

  pid = fork();
  if (pid == -1) {
    perror("fork");
    if (has_saved_termios) {
      tcsetattr(tty_fd, TCSADRAIN, &saved_termios);
    }
    unlink(filename);
    return -1;
  }

  if (pid == 0) {
    /* childprozess */
    char cmd[1024];

    /* reconnect stdin and stdout with the real terminal */
    if (dup2(tty_fd, STDIN_FILENO) == -1 ||
        dup2(tty_fd, STDOUT_FILENO) == -1) {
      perror("dup2");
      _exit(127);
    }
    if (tty_fd > STDERR_FILENO) {
      close(tty_fd);
    }

    /* execute over /bin/sh, so, for example, EDITOR="nano -w" works */
    if (strlen(editor) > sizeof(cmd) - sizeof(" \"$1\"")) {
      fprintf(stderr, "EDITOR-Befehl ist zu lang\n");
      _exit(127);
    }
    sprintf(cmd, "%s \"$1\"", editor);

    execl("/bin/sh", "sh", "-c", cmd, "sh", filename, (char *)NULL);
    perror("execl");
    _exit(127);
  }

  /* parentprozess: Wait for end of editor */
  do {
    w = waitpid(pid, &status, 0);
  } while (w == -1 && errno == EINTR);

  /* Restore raw mode again */
  if (has_saved_termios) {
    tcsetattr(tty_fd, TCSADRAIN, &saved_termios);
  }

  if (w == -1) {
    perror("waitpid");
    unlink(filename);
    return -1;
  }

  fd = open(filename, O_RDONLY);
  if (fd == -1) {
    perror("open");
    unlink(filename);
    return -1;
  }

  buf->length = 0;
  if (buf->data != NULL) {
    buf->data[0] = '\0';
  }

  while ((fbr = read(fd, tmp, sizeof(tmp))) > 0 ||
         (fbr == -1 && errno == EINTR)) {
    if (fbr == -1) continue;
    ssize_t k;
    for (k = 0; k < fbr; k++) {
      if (tmp[k] == '\t') {
        insert_at(buf, buf->length, "    ");
      } else {
        ensure_capacity(buf, 1);
        buf->data[buf->length] = tmp[k];
        buf->length++;
        buf->data[buf->length] = '\0';
      }
    }
  }
  if (fbr == -1) {
    perror("Fehler beim Lesen");
    close(fd);
    unlink(filename);
    return -1;
  }

  close(fd);
  unlink(filename);
  return 0;
}

void usage(char *prog) {
  printf("Usage: %s [-bhx][-r rows][-p text]\n", prog);
  printf("  -h         Display this help message\n");
  printf("  -x         Enables ENTER as submit key\n");
  printf("  -b         Display the status bar in reverse video\n");
  printf("  -p text    Set the status bar text\n");
  printf("  -r height  Number of visible rows (default: 3)\n");
  printf("  -o         Turns shedit into a one line input\n");
  printf("             sets -r1 -x disables the control squences\n");
}

/* main */

int main(int argc, char **argv) {
  int escaped = 0;

  char status_prompt[1024] =
      "ctrl-j newline "
      "ctrl-d submit "
      "ctrl-e editor "
      "ctrl-x abort";
 
  int ekey = 0;
  int pflag = 0;
  int bflag = 0;
  int oflag = 0;  

  /* getopt */
  int op;
  opterr = 0;

  while ((op = getopt(argc, argv, "bhxr:p:o")) != -1) {
    char *end;

    switch (op) {
      case 'h':
        usage(argv[0]);
        return 0;
      case 'b':
        bflag = 1;
        break;
      case 'p':
        pflag = 1;
        snprintf(status_prompt, sizeof(status_prompt), "%s", optarg);
        break;
      case 'r':
        EDITOR_HEIGHT = (int)strtol(optarg, &end, 10);
        if (optarg == end || *end != '\0' || EDITOR_HEIGHT < 0) { 
        /* if (optarg == end || *end != '\0') { */
          EDITOR_HEIGHT = 3;
        }
        break;
      case 'x':
        ekey = 1;
        if (pflag == 0) {
          char sp[1024] =
              "ctrl-j newline "
              "enter submit "
              "ctrl-e editor "
              "ctrl-x abort";
          snprintf(status_prompt, sizeof(status_prompt), "%s", sp);
        }
        break;
      case 'o':
        oflag = 1;
        break;
      case '?':
        if (optopt == 'r' || optopt == 'p')
          fprintf(stderr, "Option -%c requires an argument.\n", optopt);
        else if (isprint(optopt))
          fprintf(stderr, "Unknown option `-%c'.\n", optopt);
        else
          fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
        return 1;
      default:
        abort();
    }
  }
  /* getopt end */

  /* -o overrides all options */
  if (oflag == 1) {
    ekey = 1;
    EDITOR_HEIGHT = 1;
    if (pflag == 0) {
      char zp[30] = "enter submit esc cancel";
      snprintf(status_prompt, sizeof(status_prompt), "%s", zp);
    }
  }

  /* init the textbuffer & frame buffer */
  Buffer *buf = init_buffer();
  Buffer *frame = init_buffer();

  /* prepare select */
  fd_set fds;
  struct timeval tv;

  /* are there data from stdin ? */
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  tv.tv_sec = 0;
  tv.tv_usec = 200000;
  int ret;
  int pc;
  ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
  if (ret == -1) {
    perror("select error");
  } else if (ret > 0) {
    if (FD_ISSET(STDIN_FILENO, &fds)) {
      while ((pc = getchar()) != EOF) {
        if (pc == '\t') {
          insert_at(buf, buf->length, "    ");
        } else {
          ensure_capacity(buf, 1);
          buf->data[buf->length] = (char)pc;
          buf->length++;
          buf->data[buf->length] = '\0';
        }
      }
    }
  }
  /* end data from stdin */


  struct termios orig_termios;

  int td = open("/dev/tty", O_RDWR | O_NOCTTY);
  if (td == -1) {
    free(frame->data);
    free(frame);
    free(buf->data);
    free(buf);
    return 1;
  }

  int start_row, start_col;
  get_cursor_position(td, &start_row, &start_col);

  tcgetattr(td, &orig_termios);
  struct termios raw = orig_termios;
  /* disable ICRNL, so ENTER (13) won't be changed to CTRL-J (10) */
  raw.c_iflag &= ~(ICRNL);
  raw.c_lflag &= ~(ICANON | ECHO | ISIG);
  tcsetattr(td, TCSANOW, &raw);

  struct winsize w = {0};
  if (ioctl(td, TIOCGWINSZ, &w) == -1 || w.ws_col == 0) {
    w.ws_col = 80;
    w.ws_row = 24;
  }
  int term_width = w.ws_col;

  /* -r 0 means max height of terminal */
  if (EDITOR_HEIGHT == 0) {
    EDITOR_HEIGHT = w.ws_row;
  }
  /* if there is not enough space for EDITOR_HEIGHT + status (1 line) */
  int needed_total = EDITOR_HEIGHT + 1;
  int available = w.ws_row - start_row + 1;
  if (available < needed_total) {
    int space_needed = needed_total - available;

    /* 1. jump to last line of terminal */
    lbuflen = sprintf(lbuf, "\033[%d;1H", w.ws_row);
    write_all(td, lbuf, (size_t)lbuflen);

    /* 2. for every line needed print a \n to scroll the terminal */
    int s;
    for (s = 0; s < space_needed; s++) {
      write_all(td, "\n", 1);
    }

    /* 3. Since the terminal content moved up the startpos did also */
    start_row -= space_needed;
    if (start_row < 1) start_row = 1;
  }

  lbuflen = sprintf(lbuf, "\033[%d;%dH", start_row, start_col);
  write_all(td, lbuf, (size_t)lbuflen);

  InputBuf in = { td, {0}, 0, 0 };
  size_t byte_pos = 0;
  int need_status = 1;

  render(td, frame, buf, start_row, start_col, byte_pos, 
         term_width, status_prompt, need_status, bflag);
  need_status = 0;

  int running = 1;
  while (running) {
    unsigned char uc = 0;
    if (in_read_byte(&in, &uc) <= 0) break;

    if ((uc == 3 || uc == 4) && (oflag == 0)) {
      running = 0;
    } else if (uc == 5 && oflag == 0) {
      if (call_editor(buf, td, &orig_termios) == -1) {
        escaped = 1;
        running = 0;
      } else {
        in.pos = in.len = 0;
        byte_pos = clamp_byte_pos(buf, byte_pos);
        need_status = 1;
      }
    } else if (uc == 24) {
      /* CTRL-X is cancel */
      escaped = 1;
      running = 0;
    } else if (uc == 13) {
      /* ENTER: break row at the current position */
      if (ekey == 0) {
        /* ekey == 1 turns Enter into submit */
        insert_at(buf, byte_pos, "\n");
        byte_pos += 1;
      } else {
        running = 0;
      }
    } else if (uc == 10 && oflag == 0) {
      /* CTRL-J: New empty row below the current row */
      size_t p = byte_pos;
      while (p < buf->length && buf->data[p] != '\n') p++;

      if (p < buf->length && buf->data[p] == '\n') {
        insert_at(buf, p + 1, "\n");
        byte_pos = p + 1;
      }else {
        insert_at(buf, p, "\n");
        byte_pos = p + 1;
      }
    } else if (uc == 127 || uc == 8) {
      if (byte_pos > 0) {
        size_t ls = byte_pos;
        while (ls > 0 && buf->data[ls - 1] != '\n') ls--;
        size_t p = (byte_pos > ls) ? utf8_prev_grapheme(buf->data, ls, 
                    byte_pos) : byte_pos - 1;

        size_t len = byte_pos - p;
        memmove(buf->data + p, buf->data + byte_pos, 
                buf->length - byte_pos + 1);

        buf->length -= len;
        byte_pos = p;
      }
    } else if (uc == 27) {
      unsigned char seq0 = 0;
      if (!in_wait_byte(&in, &seq0, 200)) {
        unsigned char c1 = 0;
        if (in_wait_byte(&in, &c1, 1500)) {
          if (c1 == 'x' || c1 == 'q') {
            escaped = 1;
            running = 0;
          } else if (c1 == 'd' && oflag == 0) {
            running = 0;
          } else if (c1 == 'e' && oflag == 0) {
            if (call_editor(buf, td, &orig_termios) == -1) {
              escaped = 1;
              running = 0;
            } else {
              in.pos = in.len = 0;
              byte_pos = clamp_byte_pos(buf, byte_pos);
              need_status = 1;
            }
          }
        } else {
          escaped = 1;
          running = 0;
        }
      } else if (seq0 == '[') {
        unsigned char seq1 = 0;
        if (in_wait_byte(&in, &seq1, 50)) {
          if (seq1 == 'A') {
            /* UP */
            if (byte_pos > 0) {
              size_t cur_start = byte_pos;
              while (cur_start > 0 && buf->data[cur_start - 1] != '\n') {
                cur_start--;
              }
              if (cur_start > 0) {
                int target_col = get_col_at(buf->data, buf->length, 
                                            cur_start, byte_pos);
                size_t prev_end = cur_start - 1;
                size_t prev_start = prev_end;
                while (prev_start > 0 && buf->data[prev_start - 1] != '\n') {
                  prev_start--;
                }
                size_t final_p = prev_start;
                int count = 0;
                while (final_p < prev_end) {
                  int cw = 0;
                  size_t glen = utf8_next_grapheme(buf->data, prev_end, 
                                                   final_p, &cw);
                  if (glen == 0) break;
                  if (cw > 0 && count + cw > target_col) break;
                  if (count >= target_col && cw == 0) break;
                  count += cw;
                  final_p += glen;
                }
                byte_pos = final_p;
              } else {
                byte_pos = 0;
              }
            }
          } else if (seq1 == 'B') {
            /* DOWN */
            size_t cur_start = byte_pos;
            while (cur_start > 0 && buf->data[cur_start - 1] != '\n') {
              cur_start--;
            }
            size_t p = byte_pos;
            while (p < buf->length && buf->data[p] != '\n') p++;
            if (p < buf->length) {
              int target_col = get_col_at(buf->data, buf->length, 
                                          cur_start, byte_pos);
              size_t next_start = p + 1;
              size_t next_end = next_start;
              while (next_end < buf->length && buf->data[next_end] != '\n') {
                next_end++;
              }
              size_t final_p = next_start;
              int count = 0;
              while (final_p < next_end) {
                int cw = 0;
                size_t glen = utf8_next_grapheme(buf->data, next_end, 
                                                 final_p, &cw);
                if (glen == 0) break;
                if (cw > 0 && count + cw > target_col) break;
                if (count >= target_col && cw == 0) break;
                count += cw;
                final_p += glen;
              }
              byte_pos = final_p;
            } else {
              byte_pos = buf->length;
            }
          } else if (seq1 == 'C') {
            /* RIGHT */
            if (byte_pos < buf->length) {
              byte_pos += utf8_next_grapheme(buf->data, buf->length, 
                                             byte_pos, NULL);
            }
          } else if (seq1 == 'D') {
            /* LEFT */
            if (byte_pos > 0) {
              size_t ls = byte_pos;
              while (ls > 0 && buf->data[ls - 1] != '\n') ls--;
              if (byte_pos > ls) {
                byte_pos = utf8_prev_grapheme(buf->data, ls, byte_pos);
              } else {
                byte_pos--;
              }
            }
          }
        }
      } else if (seq0 == 'x' || seq0 == 'q') {
        escaped = 1;
        running = 0;
      } else if (seq0 == 'd') {
        running = 0;
      } else if (seq0 == 'e') {
        if (call_editor(buf, td, &orig_termios) == -1) {
          escaped = 1;
          running = 0;
        } else {
          in.pos = in.len = 0;
          byte_pos = clamp_byte_pos(buf, byte_pos);
          need_status = 1;
        }
      }
    } else if (uc == '\t') {
      insert_at(buf, byte_pos, "    ");
      byte_pos += 4;
    } else {
      int expected = 0;
      if (uc >= 0x20 && uc < 0x7F) expected = 1;
      else if (uc >= 0xC2 && uc <= 0xDF) expected = 2;
      else if ((uc & 0xF0) == 0xE0) expected = 3;
      else if ((uc & 0xF8) == 0xF0 && uc <= 0xF4) expected = 4;

      if (expected > 0) {
        char str[5] = {0};
        str[0] = (char)uc;
        int valid = 1;
        int k;
        for (k = 1; k < expected; k++) {
          unsigned char cont = 0;
          if (!in_wait_byte(&in, &cont, 50) || (cont & 0xC0) != 0x80) {
            valid = 0;
            break;
          }
          str[k] = (char)cont;
        }
        if (valid) {
          size_t chk = 0;
          uint32_t cp = 0;
          if (utf8_decode(str, (size_t)expected, &chk, &cp) &&
              !(cp < 0x20 || (cp >= 0x7F && cp < 0xA0))) {
            insert_at(buf, byte_pos, str);
            byte_pos += (size_t)expected;
          }
        }
      }
    }

    if (running && !in_has_data(&in)) {
      render(td, frame, buf, start_row, start_col, byte_pos, 
             term_width, status_prompt, need_status, bflag);
      need_status = 0;
    }
  }

  /* Cleanup */
  frame->length = 0;
  buf_append(frame, "\033[?2026h\033[?25l", 14);
  int r;
  for (r = 0; r <= EDITOR_HEIGHT; r++) {
    lbuflen = sprintf(lbuf, "\033[%d;%dH\033[K", start_row + r, start_col);
    buf_append(frame, lbuf, (size_t)lbuflen);
  }
  lbuflen = sprintf(lbuf, "\033[%d;%dH\033[?25h\033[?2026l", start_row, 
                    start_col);
  buf_append(frame, lbuf, (size_t)lbuflen);
  write_all(td, frame->data, frame->length);

  tcsetattr(td, TCSANOW, &orig_termios);
  close(td);

  if (escaped == 0) {
    printf("%s", buf->data);
  }
  free(frame->data);
  free(frame);
  free(buf->data);
  free(buf);

  return (escaped == 0) ? 0 : 1;
}

