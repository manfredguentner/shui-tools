/* Copyright (c) 2026, Manfred Güntner
   SPDX-License-Identifier: BSD-2-Clause */

/* shselect.c a Selector for terminal */
/* The UTF-8 support was created with the help of AI */

#include <ctype.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <stdint.h>

int SELECT_HEIGHT = 4;
/* set to 0 to disable status line */
int STATUS_HEIGHT = 1; 

typedef struct {
  char **items;
  size_t count;
  size_t capacity;
} LineArray;

static void
usage(const char *prog) {
  printf("Usage: %s [-bcghmnsv] [-r height] [-p text]\n", prog);
  printf("  -b         Display the bottom bar in reverse video\n");
  printf("  -g         Disable the bottom bar\n");
  printf("  -c         Show the cursor in the first column instead of\n");
  printf("             using reverse video\n");
  printf("  -h         Display this help message\n");
  printf("  -m         Enable multi-selection mode\n");
  printf("  -n         Output the selected line number\n");
  printf("  -p text    Set the bottom bar text\n");
  printf("  -r height  Number of visible rows (default: 4)\n");
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

static void
write_truncated(int fd, const char *text, size_t avail_cols) {
  if (avail_cols == 0) {
    return;
  }

  size_t len = strlen(text);
  size_t pos = 0;
  size_t chunk_start = 0;
  size_t used_cols = 0;

  while (pos < len) {
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
}

static void
print_out(int s, int n, size_t pos, char* line) {
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
  char status_prompt[1024] = 
        "* < back . > forward . ^ up . v down . enter submit . q quit *";
  int pret = 0;

  while ((op = getopt(argc, argv, "bcghmnsvr:p:")) != -1) {
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
        SELECT_HEIGHT = (int)strtol(optarg, &end, 10);
        /* if (optarg == end || *end != '\0' || SELECT_HEIGHT < 1) { */
        /* if (optarg == end || *end != '\0') { */
        if (optarg == end || *end != '\0' || SELECT_HEIGHT < 0) {
          SELECT_HEIGHT = 4;
        }
        break;
      case 'p':
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
    printf("0 NULL\n");
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
    line_array_free(&lines);
    fprintf(stderr, "Cant determine cursor position\n");
    return 1;
  }

  /* compute visible area and scroll terminal if not enough space below */
  int visible_rows = SELECT_HEIGHT;
  if ((size_t)visible_rows > lines.count) {
    visible_rows = (int)lines.count;
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
  }

  /* hide cursor during selection (unless -b is active) */
  if (!rvflag) {
    write_str(tty_fd, "\x1B[?25l");
  }

  size_t cursor = 0;
  size_t top_line = 0;
  size_t selected_count = 0;
  int exit_action = 0;
  /* 0 = quit (return 0), 1 = select (ENTER), 2 = abort (return -1) */
  int running = 1;

  /* main loop */
  while (running) {
    /* draw status/prompt line */
    if (has_status) {
      char sseq[64];
      int sslen = snprintf(sseq, sizeof(sseq), "\x1B[%d;1H",
                           start_row + visible_rows);
      if (sslen > 0) {
        write_all(tty_fd, sseq, (size_t)sslen);
      }
     
      if (bflag ) {
        write_str(tty_fd, "\x1B[7m");
      } 
      write_truncated(tty_fd, status_prompt,
                      w.ws_col > 0 ? (size_t)w.ws_col - 1 : 0);
      write_str(tty_fd, "\x1B[K");
      write_str(tty_fd, "\x1B[0m");
    }

    if (rvflag) {
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

      /* Begin of ESC seq with one space or only one space */
      if (!rvflag && line_idx == cursor) {
        write_str(tty_fd, "\x1B[7m ");
      } else {
        write_str(tty_fd, " ");
      }

      size_t max_cols = w.ws_col > 0 ? (size_t)w.ws_col - 1 : 0;
      size_t prefix_len = 0;
      if (selected_count > 0) {
        /* const char *prefix = selected[line_idx] ? "[x] " : "[ ] "; */
        const char *prefix = selected[line_idx] ? "+ " : "  ";
        /* when changing the prefix, then update the prefix_len */
        prefix_len = 2;
        if (prefix_len > max_cols) {
          prefix_len = max_cols;
        }
        write_all(tty_fd, prefix, prefix_len);
      }

      size_t avail_cols = max_cols - prefix_len;
      write_truncated(tty_fd, lines.items[line_idx], avail_cols);

      if (!rvflag && line_idx == cursor) {
        write_str(tty_fd, "\x1B[0m");
      }
      write_str(tty_fd, "\x1B[K");
    }

    if (rvflag) {
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
              size_t max_top = lines.count - page;
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
              /* Home (optional, praktisch bei langen Listen) */
              cursor = 0;
              top_line = 0;
            } else if (strcmp(csi, "F") == 0 || strcmp(csi, "4~") == 0) {
              /* End (optional) */
              cursor = lines.count - 1;
              top_line = lines.count - (size_t)visible_rows;
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
                top_line = lines.count - (size_t)visible_rows;
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
  if (exit_action == 1) {
    if (selected_count > 0) {
      size_t i;
      for (i = 0; i < lines.count; i++) {
        if (selected[i]) {
          /* printf("%zu %s\n", i + 1, lines.items[i]); */
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
      /* printf("%zu %s\n", cursor + 1, lines.items[cursor]); */
    }
    ret_code = 10;
  } else if (exit_action == 0) {
    if (vflag == 1) {
      print_out(sflag, nflag, cursor + 1, lines.items[cursor]);
      /* printf("%zu %s\n", cursor + 1, lines.items[cursor]); */
    }
    ret_code = 0;
  }

  free(selected);
  line_array_free(&lines);
  return ret_code;
}

