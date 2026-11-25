/* Copyright (c) 2026, Manfred Güntner
   SPDX-License-Identifier: BSD-2-Clause */

/* shselect.c a Selector for terminal */
/* The UTF-8 support was created with the help of AI */

#include <ctype.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

int SELECT_HEIGHT = 4;
/* set to 0 to disable status line */
int STATUS_HEIGHT = 1;

typedef struct {
  char **items;
  size_t count;
  size_t capacity;
} LineArray;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} StrBuf;

typedef struct {
  int bold;
  int dim;
  int italic;
  int underline;
  int blink;
  int reverse;
  int hidden;
  int strike;
  char fg[64];
  char bg[64];
} SgrState;

static void
usage(const char *prog) {
  printf("Usage: %s [-bcghlmnsv] [-r height] [-p text]\n", prog);
  printf("  -b         Display the bottom bar in reverse video\n");
  printf("  -g         Disable the bottom bar\n");
  printf("  -c         Show the cursor in the first column instead of\n");
  printf("             using reverse video\n");
  printf("  -h         Display this help message\n");
  printf("  -l         Behave like an ANSI code aware pager\n");
  printf("  -m         Enable multi-selection mode\n");
  printf("  -n         Output the selected line number\n");
  printf("  -p text    Set the bottom bar text\n");
  printf("  -r height  Number of visible rows (default: 4, or full screen with -l)\n");
  printf("  -s         Output the selected line text\n");
  printf("  -v         When exiting with 'x' or 'q', output the line\n");
  printf("             currently under the cursor\n");
}

static int
line_array_init(LineArray *arr) {
  arr->capacity = 16;
  arr->count = 0;
  arr->items = malloc(arr->capacity * sizeof(char *));
  return arr->items != NULL ? 0 : -1;
}

static int
line_array_push(LineArray *arr, char *line) {
  if (arr->count >= arr->capacity) {
    size_t new_cap = arr->capacity * 2;
    char **tmp = realloc(arr->items, new_cap * sizeof(char *));
    if (!tmp) {
      return -1;
    }
    arr->items = tmp;
    arr->capacity = new_cap;
  }
  arr->items[arr->count++] = line;
  return 0;
}

static void
line_array_free(LineArray *arr) {
  size_t i;
  for (i = 0; i < arr->count; i++) {
    free(arr->items[i]);
  }
  free(arr->items);
  arr->items = NULL;
  arr->count = 0;
  arr->capacity = 0;
}

static int
strbuf_init(StrBuf *sb) {
  sb->cap = 128;
  sb->len = 0;
  sb->data = malloc(sb->cap);
  if (!sb->data) {
    return -1;
  }
  sb->data[0] = '\0';
  return 0;
}

static int
strbuf_append(StrBuf *sb, const char *s, size_t n) {
  if (sb->len + n + 1 > sb->cap) {
    size_t new_cap = sb->cap;
    while (sb->len + n + 1 > new_cap) {
      new_cap *= 2;
    }
    char *tmp = realloc(sb->data, new_cap);
    if (!tmp) {
      return -1;
    }
    sb->data = tmp;
    sb->cap = new_cap;
  }
  if (n > 0) {
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
  }
  sb->data[sb->len] = '\0';
  return 0;
}

static void
write_all(int fd, const char *buf, size_t len) {
  size_t written = 0;
  while (written < len) {
    ssize_t n = write(fd, buf + written, len - written);
    if (n <= 0) {
      break;
    }
    written += (size_t)n;
  }
}

static void
write_str(int fd, const char *s) {
  write_all(fd, s, strlen(s));
}

static int
utf8_decode(const char *s, size_t len, size_t *pos, uint32_t *cp_out) {
  /* strict RFC-3629-UTF-8 decoding with length validation
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

static int
cp_width(uint32_t cp) {
  if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) {
    return 0; /* Steuerzeichen */
  }
  /* Haeufige Zeichen mit Breite 0 (Combining Marks, ZWJ, Variation Selectors) */
  if ((cp >= 0x0300 && cp <= 0x036F) ||
      (cp >= 0x200B && cp <= 0x200F) ||
      (cp >= 0xFE00 && cp <= 0xFE0F) ||
      cp == 0xFEFF) {
    return 0;
  }
  /* Haeufige Zeichen mit Breite 2 (CJK, Hangul, Fullwidth, Emojis) */
  if ((cp >= 0x1100 && cp <= 0x115F) ||
      (cp >= 0x2E80 && cp <= 0x9FFF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) ||
      (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFF01 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) ||
      (cp >= 0x1F300 && cp <= 0x1FAFF)) {
    return 2;
  }
  return 1;
}

/* Hilfsfunktion: nächste Escape-Sequenz ab pos parsen.
   Gibt Endposition p zurück. Bei CSI-SGR ('m') ist *is_sgr = 1. */
static size_t
parse_esc_seq(const char *text, size_t len, size_t pos, int *is_sgr) {
  *is_sgr = 0;

  if (pos + 1 < len && text[pos + 1] == '[') {
    size_t p = pos + 2;
    while (p < len &&
           (unsigned char)text[p] >= 0x20 &&
           (unsigned char)text[p] <= 0x3F) {
      p++;
    }
    if (p < len &&
        (unsigned char)text[p] >= 0x40 &&
        (unsigned char)text[p] <= 0x7E) {
      if (text[p] == 'm') {
        *is_sgr = 1;
      }
      p++;
    }
    return p;
  }

  if (pos + 1 < len && text[pos + 1] == ']') {
    size_t p = pos + 2;
    while (p < len) {
      if ((unsigned char)text[p] == 0x07) {
        return p + 1;
      }
      if ((unsigned char)text[p] == 0x1B &&
          p + 1 < len && text[p + 1] == '\\') {
        return p + 2;
      }
      p++;
    }
    return p;
  }

  if (pos + 1 < len) {
    if ((unsigned char)text[pos + 1] >= 0x20 &&
        (unsigned char)text[pos + 1] <= 0x2F &&
        pos + 2 < len) {
      return pos + 3;
    }
    return pos + 2;
  }

  return pos + 1;
}

static void
sgr_apply(SgrState *st, const char *seq, size_t len) {
  if (len < 3) {
    return;
  }
  const char *p = seq + 2;
  const char *end = seq + len - 1; /* vor 'm' */
  if (p >= end) {
    memset(st, 0, sizeof(*st));
    return;
  }

  /* Tokens durch ';' trennen */
  const char *toks[32];
  size_t tok_lens[32];
  size_t ntoks = 0;

  while (p <= end && ntoks < 32) {
    const char *semi = memchr(p, ';', (size_t)(end - p));
    if (!semi) {
      toks[ntoks] = p;
      tok_lens[ntoks] = (size_t)(end - p);
      ntoks++;
      break;
    }
    toks[ntoks] = p;
    tok_lens[ntoks] = (size_t)(semi - p);
    ntoks++;
    p = semi + 1;
  }

  size_t i = 0;
  while (i < ntoks) {
    char tok[64];
    size_t tl = tok_lens[i];
    if (tl >= sizeof(tok)) {
      tl = sizeof(tok) - 1;
    }
    memcpy(tok, toks[i], tl);
    tok[tl] = '\0';

    if (strchr(tok, ':') != NULL) {
      if (strncmp(tok, "38:", 3) == 0) {
        snprintf(st->fg, sizeof(st->fg), "%s", tok);
      } else if (strncmp(tok, "48:", 3) == 0) {
        snprintf(st->bg, sizeof(st->bg), "%s", tok);
      } else if (strncmp(tok, "4:", 2) == 0) {
        st->underline = (strcmp(tok, "4:0") != 0);
      }
      i++;
      continue;
    }

    int v = (tl == 0) ? 0 : atoi(tok);
    switch (v) {
      case 0:
        memset(st, 0, sizeof(*st));
        break;
      case 1:
        st->bold = 1;
        break;
      case 2:
        st->dim = 1;
        break;
      case 3:
        st->italic = 1;
        break;
      case 4:
        st->underline = 1;
        break;
      case 5:
      case 6:
        st->blink = 1;
        break;
      case 7:
        st->reverse = 1;
        break;
      case 8:
        st->hidden = 1;
        break;
      case 9:
        st->strike = 1;
        break;
      case 21:
      case 22:
        st->bold = 0;
        st->dim = 0;
        break;
      case 23:
        st->italic = 0;
        break;
      case 24:
        st->underline = 0;
        break;
      case 25:
        st->blink = 0;
        break;
      case 27:
        st->reverse = 0;
        break;
      case 28:
        st->hidden = 0;
        break;
      case 29:
        st->strike = 0;
        break;
      case 39:
        st->fg[0] = '\0';
        break;
      case 49:
        st->bg[0] = '\0';
        break;
      case 38:
      case 48: {
        char *target = (v == 38) ? st->fg : st->bg;
        size_t tsz = sizeof(st->fg);
        if (i + 2 < ntoks && tok_lens[i + 1] == 1 && toks[i + 1][0] == '5') {
          int c = atoi(toks[i + 2]);
          snprintf(target, tsz, "%d;5;%d", v, c);
          i += 2;
        } else if (i + 4 < ntoks && tok_lens[i + 1] == 1 && toks[i + 1][0] == '2') {
          int r = atoi(toks[i + 2]);
          int g = atoi(toks[i + 3]);
          int b = atoi(toks[i + 4]);
          snprintf(target, tsz, "%d;2;%d;%d;%d", v, r, g, b);
          i += 4;
        }
        break;
      }
      default:
        if ((v >= 30 && v <= 37) || (v >= 90 && v <= 97)) {
          snprintf(st->fg, sizeof(st->fg), "%d", v);
        } else if ((v >= 40 && v <= 47) || (v >= 100 && v <= 107)) {
          snprintf(st->bg, sizeof(st->bg), "%d", v);
        }
        break;
    }
    i++;
  }
}

static size_t
sgr_emit(const SgrState *st, char *out, size_t out_sz) {
  const char *parts[12];
  size_t n = 0;

  if (st->bold) parts[n++] = "1";
  if (st->dim) parts[n++] = "2";
  if (st->italic) parts[n++] = "3";
  if (st->underline) parts[n++] = "4";
  if (st->blink) parts[n++] = "5";
  if (st->reverse) parts[n++] = "7";
  if (st->hidden) parts[n++] = "8";
  if (st->strike) parts[n++] = "9";
  if (st->fg[0] != '\0') parts[n++] = st->fg;
  if (st->bg[0] != '\0') parts[n++] = st->bg;

  if (n == 0) {
    if (out_sz > 0) out[0] = '\0';
    return 0;
  }

  size_t used = 0;
  int w = snprintf(out, out_sz, "\x1B[");
  if (w < 0 || (size_t)w >= out_sz) {
    return 0;
  }
  used = (size_t)w;

  size_t i;
  for (i = 0; i < n; i++) {
    w = snprintf(out + used, out_sz - used, "%s%s", (i > 0) ? ";" : "", parts[i]);
    if (w < 0 || used + (size_t)w >= out_sz) {
      break;
    }
    used += (size_t)w;
  }

  if (used + 1 < out_sz) {
    out[used++] = 'm';
    out[used] = '\0';
  }
  return used;
}

static int
wrap_lines_ansi(const LineArray *src, size_t max_cols, LineArray *dst) {
  if (line_array_init(dst) != 0) {
    return -1;
  }
  if (max_cols == 0) {
    max_cols = 1;
  }

  SgrState sgr;
  memset(&sgr, 0, sizeof(sgr));

  size_t i;
  for (i = 0; i < src->count; i++) {
    const char *line = src->items[i];
    size_t len = strlen(line);
    size_t pos = 0;
    size_t used_cols = 0;

    StrBuf sb;
    if (strbuf_init(&sb) != 0) {
      line_array_free(dst);
      return -1;
    }

    char sgr_seq[256];
    size_t sgr_len = sgr_emit(&sgr, sgr_seq, sizeof(sgr_seq));
    if (sgr_len > 0 && strbuf_append(&sb, sgr_seq, sgr_len) != 0) {
      free(sb.data);
      line_array_free(dst);
      return -1;
    }

    while (pos < len) {
      if ((unsigned char)line[pos] == 0x1B) {
        int is_sgr = 0;
        size_t next_pos = parse_esc_seq(line, len, pos, &is_sgr);
        if (is_sgr) {
          if (strbuf_append(&sb, line + pos, next_pos - pos) != 0) {
            free(sb.data);
            line_array_free(dst);
            return -1;
          }
          sgr_apply(&sgr, line + pos, next_pos - pos);
        }
        pos = next_pos;
        continue;
      }

      size_t prev_pos = pos;
      uint32_t cp = 0;
      int valid = utf8_decode(line, len, &pos, &cp);

      if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) {
        if (cp == '\t') {
          if (used_cols + 1 > max_cols && used_cols > 0) {
            if (line_array_push(dst, sb.data) != 0) {
              free(sb.data);
              line_array_free(dst);
              return -1;
            }
            if (strbuf_init(&sb) != 0) {
              line_array_free(dst);
              return -1;
            }
            sgr_len = sgr_emit(&sgr, sgr_seq, sizeof(sgr_seq));
            if (sgr_len > 0 && strbuf_append(&sb, sgr_seq, sgr_len) != 0) {
              free(sb.data);
              line_array_free(dst);
              return -1;
            }
            used_cols = 0;
          }
          if (strbuf_append(&sb, " ", 1) != 0) {
            free(sb.data);
            line_array_free(dst);
            return -1;
          }
          used_cols++;
        }
        continue;
      }

      int cw = valid ? cp_width(cp) : 1;
      if (used_cols + (size_t)cw > max_cols && used_cols > 0) {
        if (line_array_push(dst, sb.data) != 0) {
          free(sb.data);
          line_array_free(dst);
          return -1;
        }
        if (strbuf_init(&sb) != 0) {
          line_array_free(dst);
          return -1;
        }
        sgr_len = sgr_emit(&sgr, sgr_seq, sizeof(sgr_seq));
        if (sgr_len > 0 && strbuf_append(&sb, sgr_seq, sgr_len) != 0) {
          free(sb.data);
          line_array_free(dst);
          return -1;
        }
        used_cols = 0;
      }

      if (valid) {
        if (strbuf_append(&sb, line + prev_pos, pos - prev_pos) != 0) {
          free(sb.data);
          line_array_free(dst);
          return -1;
        }
      } else {
        if (strbuf_append(&sb, "\xEF\xBF\xBD", 3) != 0) {
          free(sb.data);
          line_array_free(dst);
          return -1;
        }
      }
      used_cols += (size_t)cw;
    }

    if (line_array_push(dst, sb.data) != 0) {
      free(sb.data);
      line_array_free(dst);
      return -1;
    }
  }

  return 0;
}

static size_t
write_truncated(int fd, const char *text, size_t avail_cols, int ansi_aware) {
  if (avail_cols == 0) {
    return 0;
  }

  size_t len = strlen(text);
  size_t pos = 0;
  size_t chunk_start = 0;
  size_t used_cols = 0;

  while (pos < len) {
    if (ansi_aware && (unsigned char)text[pos] == 0x1B) {
      if (pos > chunk_start) {
        write_all(fd, text + chunk_start, pos - chunk_start);
      }

      int is_sgr = 0;
      size_t next_pos = parse_esc_seq(text, len, pos, &is_sgr);
      if (is_sgr) {
        write_all(fd, text + pos, next_pos - pos);
      }
      pos = next_pos;
      chunk_start = pos;
      continue;
    }

    size_t prev_pos = pos;
    uint32_t cp = 0;
    int valid = utf8_decode(text, len, &pos, &cp);

    /* C0-, DEL- und C1-Steuerzeichen abfangen */
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) {
      if (prev_pos > chunk_start) {
        write_all(fd, text + chunk_start, prev_pos - chunk_start);
      }
      if (cp == '\t') {
        if (used_cols + 1 > avail_cols) {
          chunk_start = pos;
          break;
        }
        write_str(fd, " ");
        used_cols++;
      }
      chunk_start = pos;
      continue;
    }

    int cw = valid ? cp_width(cp) : 1;
    if (used_cols + (size_t)cw > avail_cols) {
      pos = prev_pos;
      break;
    }

    if (!valid) {
      if (prev_pos > chunk_start) {
        write_all(fd, text + chunk_start, prev_pos - chunk_start);
      }
      write_str(fd, "\xEF\xBF\xBD"); /* U+FFFD sauber ausgeben */
      chunk_start = pos;
    }

    used_cols += (size_t)cw;
  }

  if (pos > chunk_start) {
    write_all(fd, text + chunk_start, pos - chunk_start);
  }

  return used_cols;
}

static void
print_out(int s, int n, size_t pos, char *line) {
  if ((n == 1 && s == 1) || (n == 0 && s == 0)) {
    printf("%zu %s\n", pos, line);
  } else if (n == 1 && s == 0) {
    printf("%zu\n", pos);
  } else if (n == 0 && s == 1) {
    printf("%s\n", line);
  }
}

int
main(int argc, char **argv) {
  int op;
  opterr = 0;
  int multi = 0;
  int bflag = 0;
  int rvflag = 0;
  int vflag = 0;
  int nflag = 0;
  int sflag = 0;
  int lflag = 0;
  int rflag = 0;
  int pflag = 0;
  char status_prompt[1024] =
      "* < back . > forward . ^ up . v down . enter submit . q quit *";
  int pret = 0;

  while ((op = getopt(argc, argv, "bcghlmnsvr:p:")) != -1) {
    char *end;

    switch (op) {
      case 'h':
        usage(argv[0]);
        return 0;
      case 'b':
        bflag = 1;
        break;
      case 'c':
        rvflag = 1;
        break;
      case 'g':
        STATUS_HEIGHT = 0;
        break;
      case 'l':
        lflag = 1;
        break;
      case 'm':
        multi = 1;
        break;
      case 'v':
        vflag = 1;
        break;
      case 'n':
        nflag = 1;
        break;
      case 's':
        sflag = 1;
        break;
      case 'r':
        rflag = 1;
        SELECT_HEIGHT = (int)strtol(optarg, &end, 10);
        if (optarg == end || *end != '\0' || SELECT_HEIGHT < 0) {
          SELECT_HEIGHT = 4;
        }
        break;
      case 'p':
        pflag = 1;
        pret = snprintf(status_prompt, sizeof(status_prompt), "%s", optarg);
        if (pret == 0) {
          STATUS_HEIGHT = 0;
        }
        break;
      case '?':
        if (optopt == 'r' || optopt == 'p') {
          fprintf(stderr, "Option -%c requires an argument.\n", optopt);
        } else if (isprint(optopt)) {
          fprintf(stderr, "Unknown option `-%c'.\n", optopt);
        } else {
          fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
        }
        return 1;
      default:
        abort();
    }
  }

  /* Im Pager-Modus (-l) standardmäßig maximale Höhe nutzen, außer -r wurde gesetzt */
  if (lflag && !rflag) {
    SELECT_HEIGHT = 0;
  }

  /* init the textbuffer */
  LineArray lines;
  if (line_array_init(&lines) != 0) {
    perror("malloc");
    return 1;
  }

  /* prepare select */
  fd_set fds;
  struct timeval tv;

  /* are there data from stdin ? */
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  tv.tv_sec = 0;
  tv.tv_usec = 200000;

  int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
  if (ret == -1) {
    perror("select error");
    line_array_free(&lines);
    return 1;
  } else if (ret > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
    size_t line_cap = 128;
    size_t line_len = 0;
    char *line_buf = malloc(line_cap);
    if (!line_buf) {
      perror("malloc");
      line_array_free(&lines);
      return 1;
    }

    int pc;
    while ((pc = getchar()) != EOF) {
      if (pc == '\r') {
        continue;
      }
      if (pc == '\n') {
        line_buf[line_len] = '\0';
        char *copy = malloc(line_len + 1);
        if (!copy || line_array_push(&lines, copy) != 0) {
          free(copy);
          free(line_buf);
          line_array_free(&lines);
          perror("malloc");
          return 1;
        }
        memcpy(copy, line_buf, line_len + 1);
        line_len = 0;
      } else if (pc == '\t') {
        while (line_len + 4 >= line_cap) {
          size_t new_cap = line_cap * 2;
          char *tmp = realloc(line_buf, new_cap);
          if (!tmp) {
            free(line_buf);
            line_array_free(&lines);
            perror("realloc");
            return 1;
          }
          line_buf = tmp;
          line_cap = new_cap;
        }
        memset(line_buf + line_len, ' ', 4);
        line_len += 4;
      } else {
        if (line_len + 1 >= line_cap) {
          size_t new_cap = line_cap * 2;
          char *tmp = realloc(line_buf, new_cap);
          if (!tmp) {
            free(line_buf);
            line_array_free(&lines);
            perror("realloc");
            return 1;
          }
          line_buf = tmp;
          line_cap = new_cap;
        }
        line_buf[line_len++] = (char)pc;
      }
    }

    /* handle last line without trailing newline */
    if (line_len > 0) {
      line_buf[line_len] = '\0';
      if (line_array_push(&lines, line_buf) != 0) {
        free(line_buf);
        line_array_free(&lines);
        perror("realloc");
        return 1;
      }
    } else {
      free(line_buf);
    }
  }
  /* end data from stdin */

  /* no data from stdin */
  if (lines.count == 0) {
    line_array_free(&lines);
    if (!lflag) {
      printf("0 NULL\n");
    }
    return 1;
  }

  unsigned char *selected = calloc(lines.count, sizeof(unsigned char));
  if (!selected) {
    perror("calloc");
    line_array_free(&lines);
    return 1;
  }

  /* open /dev/tty */
  int tty_fd = open("/dev/tty", O_RDWR);
  if (tty_fd < 0) {
    perror("open /dev/tty");
    free(selected);
    line_array_free(&lines);
    return 1;
  }

  /* determine the proportions */
  /* result in: w.ws_row , w.ws_col */
  struct winsize w;
  if (ioctl(tty_fd, TIOCGWINSZ, &w) == -1) {
    perror("ioctl");
    close(tty_fd);
    free(selected);
    line_array_free(&lines);
    return 1;
  }

  /* Im Pager-Modus (-l) lange Zeilen ANSI-aware umbrechen */
  LineArray wrapped_lines;
  const LineArray *display_lines = &lines;
  if (lflag) {
    size_t wrap_cols = w.ws_col > 0 ? (size_t)w.ws_col : 80;
    if (wrap_lines_ansi(&lines, wrap_cols, &wrapped_lines) != 0) {
      perror("malloc");
      close(tty_fd);
      free(selected);
      line_array_free(&lines);
      return 1;
    }
    display_lines = &wrapped_lines;
  }

  /* -r 0 means max height of terminal */
  if (SELECT_HEIGHT == 0) {
    SELECT_HEIGHT = w.ws_row;
  }
  /* addon for status_prompt */
  int has_status = (STATUS_HEIGHT > 0);

  /* set terminal into raw mode */
  struct termios oldt, newt;
  if (tcgetattr(tty_fd, &oldt) == -1) {
    perror("tcgetattr");
    close(tty_fd);
    free(selected);
    if (lflag) {
      line_array_free(&wrapped_lines);
    }
    line_array_free(&lines);
    return 1;
  }

  newt = oldt;
  newt.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
  newt.c_cc[VMIN] = 1;
  newt.c_cc[VTIME] = 0;
  if (tcsetattr(tty_fd, TCSANOW, &newt) == -1) {
    perror("tcsetattr");
    close(tty_fd);
    free(selected);
    if (lflag) {
      line_array_free(&wrapped_lines);
    }
    line_array_free(&lines);
    return 1;
  }

  /* send query : ESC[6n */
  write_str(tty_fd, "\x1B[6n");

  /* read answer ESC[<row>;<col>R */
  char buf[32];
  size_t idx = 0;
  while (idx < sizeof(buf) - 1) {
    ssize_t n = read(tty_fd, &buf[idx], 1);
    if (n <= 0) {
      break;
    }
    if (buf[idx] == 'R') {
      idx++;
      break;
    }
    idx++;
  }
  buf[idx] = '\0';

  int row = 0, col = 0;
  if (sscanf(buf, "\x1B[%d;%dR", &row, &col) != 2) {
    tcsetattr(tty_fd, TCSANOW, &oldt);
    close(tty_fd);
    free(selected);
    if (lflag) {
      line_array_free(&wrapped_lines);
    }
    line_array_free(&lines);
    fprintf(stderr, "Cant determine cursor position\n");
    return 1;
  }

  /* compute visible area and scroll terminal if not enough space below */
  int visible_rows = SELECT_HEIGHT;
  if ((size_t)visible_rows > display_lines->count) {
    visible_rows = (int)display_lines->count;
  }
  if (visible_rows > (int)w.ws_row - has_status) {
    visible_rows = (int)w.ws_row - has_status;
  }
  if (visible_rows < 1) {
    visible_rows = 1;
  }

  int total_rows = visible_rows + (has_status ? STATUS_HEIGHT : 0);
  int start_row = row;
  if (row + total_rows - 1 > w.ws_row) {
    int k;
    for (k = 0; k < total_rows - 1; k++) {
      write_str(tty_fd, "\n");
    }
    start_row = w.ws_row - total_rows + 1;
    if (start_row < 1) {
      start_row = 1;
    }
  }

  /* hide cursor during selection/paging (unless -c without -l is active) */
  if (lflag || !rvflag) {
    write_str(tty_fd, "\x1B[?25l");
  }

  size_t cursor = 0;
  size_t top_line = 0;
  size_t selected_count = 0;
  size_t max_top = (display_lines->count > (size_t)visible_rows)
                       ? (display_lines->count - (size_t)visible_rows)
                       : 0;
  int exit_action = 0;
  /* 0 = quit (return 0), 1 = select (ENTER), 2 = abort (return 10) */
  int running = 1;

  /* main loop */
  while (running) {
    /* draw status/prompt line */
    if (has_status) {
      if (lflag && !pflag) {
        size_t first_vis = top_line + 1;
        size_t last_vis = top_line + (size_t)visible_rows;
        if (last_vis > display_lines->count) {
          last_vis = display_lines->count;
        }
        int pct = (display_lines->count > 0)
                      ? (int)((last_vis * 100) / display_lines->count)
                      : 100;
        snprintf(status_prompt, sizeof(status_prompt),
                 "* %zu-%zu/%zu (%d%%) . < back . > forward . ^ up . v down . q quit *",
                 first_vis, last_vis, display_lines->count, pct);
      }

      char sseq[64];
      int sslen = snprintf(sseq, sizeof(sseq), "\x1B[%d;1H",
                           start_row + visible_rows);
      if (sslen > 0) {
        write_all(tty_fd, sseq, (size_t)sslen);
      }

      if (bflag) {
        write_str(tty_fd, "\x1B[7m");
      }
      write_truncated(tty_fd, status_prompt,
                      w.ws_col > 0 ? (size_t)w.ws_col - 1 : 0, lflag);
      write_str(tty_fd, "\x1B[K");
      write_str(tty_fd, "\x1B[0m");
    }

    if (!lflag && rvflag) {
      /* hide briefly during redraw to avoid flicker */
      write_str(tty_fd, "\x1B[?25l");
    }

    /* draw visible rows */
    int i;
    for (i = 0; i < visible_rows; i++) {
      size_t line_idx = top_line + (size_t)i;
      char seq[64];
      int slen = snprintf(seq, sizeof(seq), "\x1B[%d;1H", start_row + i);
      if (slen > 0) {
        write_all(tty_fd, seq, (size_t)slen);
      }

      if (lflag) {
        size_t max_cols = w.ws_col > 0 ? (size_t)w.ws_col : 80;
        size_t written_cols =
            write_truncated(tty_fd, display_lines->items[line_idx], max_cols, 1);
        write_str(tty_fd, "\x1B[0m");
        if (written_cols < max_cols) {
          write_str(tty_fd, "\x1B[K");
        }
      } else {
        /* Begin of ESC seq with one space or only one space */
        if (!rvflag && line_idx == cursor) {
          write_str(tty_fd, "\x1B[7m ");
        } else {
          write_str(tty_fd, " ");
        }

        size_t max_cols = w.ws_col > 0 ? (size_t)w.ws_col - 1 : 0;
        size_t prefix_len = 0;
        if (selected_count > 0) {
          const char *prefix = selected[line_idx] ? "+ " : "  ";
          /* when changing the prefix, then update the prefix_len */
          prefix_len = 2;
          if (prefix_len > max_cols) {
            prefix_len = max_cols;
          }
          write_all(tty_fd, prefix, prefix_len);
        }

        size_t avail_cols = max_cols - prefix_len;
        write_truncated(tty_fd, display_lines->items[line_idx], avail_cols, 0);

        if (!rvflag && line_idx == cursor) {
          write_str(tty_fd, "\x1B[0m");
        }
        write_str(tty_fd, "\x1B[K");
      }
    }

    if (!lflag && rvflag) {
      char cseq[64];
      int cslen = snprintf(cseq, sizeof(cseq), "\x1B[%d;1H\x1B[?25h",
                           start_row + (int)(cursor - top_line));
      if (cslen > 0) {
        write_all(tty_fd, cseq, (size_t)cslen);
      }
    }

    /* read input key */
    char c;
    if (read(tty_fd, &c, 1) <= 0) {
      break;
    }

    if (lflag) {
      /* Pager-Tasten */
      size_t page = (size_t)visible_rows;
      size_t half = page > 1 ? page / 2 : 1;

      if (c == 'q') {
        exit_action = 0;
        running = 0;
      } else if (c == 'x') {
        exit_action = 2;
        running = 0;
      } else if (c == '\n' || c == '\r' || c == 'j') {
        if (top_line < max_top) {
          top_line++;
        }
      } else if (c == 'k') {
        if (top_line > 0) {
          top_line--;
        }
      } else if (c == ' ' || c == 'f') {
        if (top_line + page < max_top) {
          top_line += page;
        } else {
          top_line = max_top;
        }
      } else if (c == 'b') {
        if (top_line > page) {
          top_line -= page;
        } else {
          top_line = 0;
        }
      } else if (c == 'd') {
        if (top_line + half < max_top) {
          top_line += half;
        } else {
          top_line = max_top;
        }
      } else if (c == 'u') {
        if (top_line > half) {
          top_line -= half;
        } else {
          top_line = 0;
        }
      } else if (c == 'g') {
        top_line = 0;
      } else if (c == 'G') {
        top_line = max_top;
      } else if (c == '\x1B') {
        fd_set kfds;
        struct timeval ktv;
        FD_ZERO(&kfds);
        FD_SET(tty_fd, &kfds);
        ktv.tv_sec = 0;
        ktv.tv_usec = 20000;

        if (select(tty_fd + 1, &kfds, NULL, NULL, &ktv) > 0) {
          char seq1;
          if (read(tty_fd, &seq1, 1) == 1) {
            if (seq1 == '[') {
              char csi[16];
              size_t csi_len = 0;
              char ch;
              while (csi_len < sizeof(csi) - 1 && read(tty_fd, &ch, 1) == 1) {
                csi[csi_len++] = ch;
                if ((unsigned char)ch >= 0x40 && (unsigned char)ch <= 0x7E) {
                  break;
                }
              }
              csi[csi_len] = '\0';

              if (strcmp(csi, "A") == 0) {
                if (top_line > 0) {
                  top_line--;
                }
              } else if (strcmp(csi, "B") == 0) {
                if (top_line < max_top) {
                  top_line++;
                }
              } else if (strcmp(csi, "5~") == 0 || strcmp(csi, "D") == 0) {
                if (top_line > page) {
                  top_line -= page;
                } else {
                  top_line = 0;
                }
              } else if (strcmp(csi, "6~") == 0 || strcmp(csi, "C") == 0) {
                if (top_line + page < max_top) {
                  top_line += page;
                } else {
                  top_line = max_top;
                }
              } else if (strcmp(csi, "H") == 0 || strcmp(csi, "1~") == 0) {
                top_line = 0;
              } else if (strcmp(csi, "F") == 0 || strcmp(csi, "4~") == 0) {
                top_line = max_top;
              }
            } else if (seq1 == 'O') {
              char seq2;
              if (read(tty_fd, &seq2, 1) == 1) {
                if (seq2 == 'A' && top_line > 0) {
                  top_line--;
                } else if (seq2 == 'B' && top_line < max_top) {
                  top_line++;
                } else if (seq2 == 'H') {
                  top_line = 0;
                } else if (seq2 == 'F') {
                  top_line = max_top;
                }
              }
            }
          }
        }
      }
    } else {
      /* Selector-Tasten */
      if (c == '\n' || c == '\r') {
        exit_action = 1;
        running = 0;
      } else if (c == 'q') {
        exit_action = 0;
        running = 0;
      } else if (c == 'x') {
        exit_action = 2;
        running = 0;
      } else if (c == 'm' || c == 's' || c == '+') {
        if (multi == 1) {
          /* toggle multi-select on current line */
          if (selected[cursor]) {
            selected[cursor] = 0;
            selected_count--;
          } else {
            selected[cursor] = 1;
            selected_count++;
          }
        }
      } else if (c == '\x1B') {
        /* check if escape sequence follows */
        fd_set kfds;
        struct timeval ktv;
        FD_ZERO(&kfds);
        FD_SET(tty_fd, &kfds);
        ktv.tv_sec = 0;
        ktv.tv_usec = 20000;

        if (select(tty_fd + 1, &kfds, NULL, NULL, &ktv) > 0) {
          char seq1;
          if (read(tty_fd, &seq1, 1) == 1) {
            if (seq1 == '[') {
              /* read full CSI sequence up to final byte (0x40..0x7E) */
              char csi[16];
              size_t csi_len = 0;
              char ch;
              while (csi_len < sizeof(csi) - 1 && read(tty_fd, &ch, 1) == 1) {
                csi[csi_len++] = ch;
                if ((unsigned char)ch >= 0x40 && (unsigned char)ch <= 0x7E) {
                  break;
                }
              }
              csi[csi_len] = '\0';

              if (strcmp(csi, "A") == 0) {
                /* cursor up */
                if (cursor > 0) {
                  cursor--;
                }
              } else if (strcmp(csi, "B") == 0) {
                /* cursor down */
                if (cursor + 1 < lines.count) {
                  cursor++;
                }
              } else if (strcmp(csi, "5~") == 0 || strcmp(csi, "D") == 0) {
                /* PageUp */
                size_t page = (size_t)visible_rows;
                if (top_line > 0) {
                  size_t new_top = (top_line > page) ? (top_line - page) : 0;
                  size_t delta = top_line - new_top;
                  top_line = new_top;
                  cursor -= delta;
                } else {
                  cursor = 0;
                }
              } else if (strcmp(csi, "6~") == 0 || strcmp(csi, "C") == 0) {
                /* PageDown */
                size_t page = (size_t)visible_rows;
                if (top_line < max_top) {
                  size_t new_top = top_line + page;
                  if (new_top > max_top) {
                    new_top = max_top;
                  }
                  size_t delta = new_top - top_line;
                  top_line = new_top;
                  cursor += delta;
                } else {
                  cursor = lines.count - 1;
                }
              } else if (strcmp(csi, "H") == 0 || strcmp(csi, "1~") == 0) {
                /* Home */
                cursor = 0;
                top_line = 0;
              } else if (strcmp(csi, "F") == 0 || strcmp(csi, "4~") == 0) {
                /* End */
                cursor = lines.count - 1;
                top_line = max_top;
              }
            } else if (seq1 == 'O') {
              char seq2;
              if (read(tty_fd, &seq2, 1) == 1) {
                if (seq2 == 'A' && cursor > 0) {
                  cursor--;
                } else if (seq2 == 'B' && cursor + 1 < lines.count) {
                  cursor++;
                } else if (seq2 == 'H') {
                  cursor = 0;
                  top_line = 0;
                } else if (seq2 == 'F') {
                  cursor = lines.count - 1;
                  top_line = max_top;
                }
              }
            }
          }
        }
      }

      /* adjust scroll viewport */
      if (cursor < top_line) {
        top_line = cursor;
      } else if (cursor >= top_line + (size_t)visible_rows) {
        top_line = cursor - (size_t)visible_rows + 1;
      }
    }
  }

  /* clear menu area and show cursor */
  char clr[64];
  int clen = snprintf(clr, sizeof(clr), "\x1B[%d;1H\x1B[J\x1B[?25h", start_row);
  if (clen > 0) {
    write_all(tty_fd, clr, (size_t)clen);
  }

  /* restore terminal mode */
  tcsetattr(tty_fd, TCSANOW, &oldt);
  close(tty_fd);

  int ret_code = 0;
  if (!lflag) {
    if (exit_action == 1) {
      if (selected_count > 0) {
        size_t i;
        for (i = 0; i < lines.count; i++) {
          if (selected[i]) {
            print_out(sflag, nflag, i + 1, lines.items[i]);
          }
        }
      } else {
        print_out(sflag, nflag, cursor + 1, lines.items[cursor]);
      }
      ret_code = 0;
    } else if (exit_action == 2) {
      if (vflag == 1) {
        print_out(sflag, nflag, cursor + 1, lines.items[cursor]);
      }
      ret_code = 10;
    } else if (exit_action == 0) {
      if (vflag == 1) {
        print_out(sflag, nflag, cursor + 1, lines.items[cursor]);
      }
      ret_code = 0;
    }
  } else if (exit_action == 2) {
    ret_code = 10;
  }

  free(selected);
  if (lflag) {
    line_array_free(&wrapped_lines);
  }
  line_array_free(&lines);
  return ret_code;
}

