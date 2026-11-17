/* Copyright (c) 2026, Manfred Güntner
   SPDX-License-Identifier: BSD-2-Clause */

/* shakey is a anykey reader with timeout. */
/* The list of available keys and combinations was created with help of AI. */

#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h> 
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <ctype.h>
#include <getopt.h>

void usage(const char *prog) {
  printf("Usage: %s -t timeout [-bnsc][-t timeout][-p text]\n", prog);
  printf("  -h    Show this help\n");
  printf("  -t    time to sleep in seconds. Fractional values are allowed.\n");
  printf("        0 is maximum wait. Maximum is one year.\n");
  printf("  -p    Displays text\n");
  printf("  -b    Turns reverse video for -p text on\n");
  printf("  -c    Turns cursor on\n");
  printf("  -n    Output is the hexadecimal sequence\n");
  printf("  -s    Output is the key-mapping\n");
}

/* help function to determine Modifier-Prefixes (xterm CSI Standard) */
static const char *get_mod_prefix(int mod) {
  switch (mod) {
    case 2: return "KEY_SHIFT_";
    case 3: return "KEY_ALT_";
    case 4: return "KEY_ALT_SHIFT_";
    case 5: return "KEY_CTRL_";
    case 6: return "KEY_CTRL_SHIFT_";
    case 7: return "KEY_CTRL_ALT_";
    case 8: return "KEY_CTRL_ALT_SHIFT_";
    default: return "KEY_";
  }
}

/* Parse CSI Escape-Sequenzen (ESC [ ...) */
static void parse_csi_sequence(const char *p, int plen, int alt_double, 
                               char *keycode, size_t maxlen) {
  /* Shift + Tab / Backtab */
  if (plen == 3 && p[2] == 'Z') {
    snprintf(keycode, maxlen, "%sSHIFT_TAB", alt_double ? "KEY_ALT_" : "KEY_");
    return;
  }

  /* Linux Console F1-F5 (\033[[A .. \033[[E) */
  if (p[2] == '[' && plen == 4) {
    switch (p[3]) {
      case 'A': snprintf(keycode, maxlen, "%sF1", 
                         alt_double ? "KEY_ALT_" : "KEY_"); 
                return;
      case 'B': snprintf(keycode, maxlen, "%sF2", 
                         alt_double ? "KEY_ALT_" : "KEY_");
                return;
      case 'C': snprintf(keycode, maxlen, "%sF3", 
                         alt_double ? "KEY_ALT_" : "KEY_");
                return;
      case 'D': snprintf(keycode, maxlen, "%sF4", 
                         alt_double ? "KEY_ALT_" : "KEY_");
                return;
      case 'E': snprintf(keycode, maxlen, "%sF5", 
                         alt_double ? "KEY_ALT_" : "KEY_");
                return;
    }
  }

  char final_c = p[plen - 1];
  char param_str[32] = {0};
  int p_str_len = plen - 3;
  if (p_str_len > 0 && p_str_len < (int)sizeof(param_str)) {
    memcpy(param_str, p + 2, p_str_len);
    param_str[p_str_len] = '\0';
  }

  int arg1 = 0, arg2 = 0;
  char *semi = strchr(param_str, ';');
  if (semi) {
    arg1 = atoi(param_str);
    arg2 = atoi(semi + 1);
  } else if (param_str[0] != '\0') {
    arg1 = atoi(param_str);
  }

  /* Modified arrow keys and Navigation (\033[1;5A etc.) */
  const char *base_name = NULL;
  switch (final_c) {
    case 'A': base_name = "UP"; break;
    case 'B': base_name = "DOWN"; break;
    case 'C': base_name = "RIGHT"; break;
    case 'D': base_name = "LEFT"; break;
    case 'H': base_name = "HOME"; break;
    case 'F': base_name = "END"; break;
    case 'P': base_name = "F1"; break;
    case 'Q': base_name = "F2"; break;
    case 'R': base_name = "F3"; break;
    case 'S': base_name = "F4"; break;
  }

  if (base_name != NULL) {
    int mod = (semi != NULL) ? arg2 : (arg1 > 1 ? arg1 : 1);
    if (alt_double && mod == 1) mod = 3;
    snprintf(keycode, maxlen, "%s%s", get_mod_prefix(mod), base_name);
    return;
  }

  /* Tilde-Sequenzes: Navigation & Funktion keys (\033[2~, \033[15;5~ etc.) */
  if (final_c == '~') {
    int mod = (semi != NULL) ? arg2 : 1;
    if (alt_double && mod == 1) mod = 3;

    switch (arg1) {
      case 1:  base_name = "HOME"; break;
      case 2:  base_name = "INSERT"; break;
      case 3:  base_name = "DELETE"; break;
      case 4:  base_name = "END"; break;
      case 5:  base_name = "PGUP"; break;
      case 6:  base_name = "PGDN"; break;
      case 7:  base_name = "HOME"; break;
      case 8:  base_name = "END"; break;
      case 11: base_name = "F1"; break;
      case 12: base_name = "F2"; break;
      case 13: base_name = "F3"; break;
      case 14: base_name = "F4"; break;
      case 15: base_name = "F5"; break;
      case 17: base_name = "F6"; break;
      case 18: base_name = "F7"; break;
      case 19: base_name = "F8"; break;
      case 20: base_name = "F9"; break;
      case 21: base_name = "F10"; break;
      case 23: base_name = "F11"; break;
      case 24: base_name = "F12"; break;
      case 25: base_name = "F13"; break;
      case 26: base_name = "F14"; break;
      case 28: base_name = "F15"; break;
      case 29: base_name = "F16"; break;
      case 31: base_name = "F17"; break;
      case 32: base_name = "F18"; break;
      case 33: base_name = "F19"; break;
      case 34: base_name = "F20"; break;
    }

    if (base_name != NULL) {
      snprintf(keycode, maxlen, "%s%s", get_mod_prefix(mod), base_name);
      return;
    }
  }

  /* Fallback for unknown CSI Sequenzes */
  snprintf(keycode, maxlen, "KEY_CSI_%s", p + 2);
}

/* Parsing SS3 Escape-Sequenzen (ESC O ...) */
static void parse_ss3_sequence(const char *p, int plen, 
                               int alt_double, char *keycode, size_t maxlen) {
  if (plen < 3) {
    snprintf(keycode, maxlen, "KEY_SS3");
    return;
  }

  char c2 = p[2];
  const char *name = NULL;

  switch (c2) {
    case 'A': name = "UP"; break;
    case 'B': name = "DOWN"; break;
    case 'C': name = "RIGHT"; break;
    case 'D': name = "LEFT"; break;
    case 'E': name = "KP5"; break;
    case 'F': name = "END"; break;
    case 'H': name = "HOME"; break;
    case 'P': name = "F1"; break;
    case 'Q': name = "F2"; break;
    case 'R': name = "F3"; break;
    case 'S': name = "F4"; break;
    case 'M': name = "KPENTER"; break;
    case 'X': name = "KPEQUAL"; break;
    case 'j': name = "KPMUL"; break;
    case 'k': name = "KPPLUS"; break;
    case 'l': name = "KPCOMMA"; break;
    case 'm': name = "KPMINUS"; break;
    case 'n': name = "KPDOT"; break;
    case 'o': name = "KPSLASH"; break;
    case 'p': name = "KP0"; break;
    case 'q': name = "KP1"; break;
    case 'r': name = "KP2"; break;
    case 's': name = "KP3"; break;
    case 't': name = "KP4"; break;
    case 'u': name = "KP5"; break;
    case 'v': name = "KP6"; break;
    case 'w': name = "KP7"; break;
    case 'x': name = "KP8"; break;
    case 'y': name = "KP9"; break;
    /* Legacy / VT-Kompatibilität */
    case 'G': name = "KP5"; break;
    case 'K': name = "KP0"; break;
    case 'L': name = "KP1"; break;
    case ';': name = "KP2"; break;
    case '2': name = "KP2"; break;
    case '3': name = "KP3"; break;
    case '4': name = "KP4"; break;
    case '5': name = "KP5"; break;
    case '6': name = "KP6"; break;
    case '7': name = "KP7"; break;
    case '8': name = "KP8"; break;
    case '9': name = "KP9"; break;
  }

  if (name != NULL) {
    snprintf(keycode, maxlen, "%s%s", alt_double ? "KEY_ALT_" : "KEY_", name);
  } else {
    snprintf(keycode, maxlen, "KEY_SS3_%c", c2);
  }
}

static void append_raw(char *raw, size_t raw_maxlen, int *raw_len, char b) {
  if (raw != NULL && raw_maxlen > 0 && (size_t)(*raw_len) + 1 < raw_maxlen) {
    raw[(*raw_len)++] = b;
    raw[*raw_len] = '\0';
  }
}

static int read_and_decode_key(int td, char *keycode, size_t maxlen,
                               char *raw, size_t raw_maxlen) {
/* reads one key press and ecodes all keys inkl. CTRL, ALT und UTF-8 */
  int raw_len = 0;
  if (raw != NULL && raw_maxlen > 0) {
    raw[0] = '\0';
  }

  char c;
  ssize_t rb = read(td, &c, 1);
  if (rb <= 0) {
    if (maxlen > 0) keycode[0] = '\0';
    return 0;
  }
  append_raw(raw, raw_maxlen, &raw_len, c);

  /* 1. non-ESC signs */
  if ((unsigned char)c != 27) {
    /* CTRL-Kombinationen & control signs */
    if (c == 0) {
      snprintf(keycode, maxlen, "KEY_CTRL_@");
      return raw_len;
    }
    if (c == 8) {
      snprintf(keycode, maxlen, "KEY_BACKSPACE");
      return raw_len;
    }
    if (c == 9) {
      snprintf(keycode, maxlen, "KEY_TAB");
      return raw_len;
    }
    if (c == 10 || c == 13) {
      snprintf(keycode, maxlen, "KEY_ENTER");
      return raw_len;
    }
    if (c >= 1 && c <= 26) {
      snprintf(keycode, maxlen, "KEY_CTRL_%c", 'A' + c - 1);
      return raw_len;
    }
    if (c == 28) {
      snprintf(keycode, maxlen, "KEY_CTRL_\\");
      return raw_len;
    }
    if (c == 29) {
      snprintf(keycode, maxlen, "KEY_CTRL_]");
      return raw_len;
    }
    if (c == 30) {
      snprintf(keycode, maxlen, "KEY_CTRL_^");
      return raw_len;
    }
    if (c == 31) {
      snprintf(keycode, maxlen, "KEY_CTRL__");
      return raw_len;
    }
    if (c == 32) {
      snprintf(keycode, maxlen, "KEY_SPACE");
      return raw_len;
    }
    if (c >= 33 && c <= 126) {
      snprintf(keycode, maxlen, "%c", c);
      return raw_len;
    }
    if ((unsigned char)c == 127) {
      snprintf(keycode, maxlen, "KEY_BACKSPACE");
      return raw_len;
    }

    /* UTF-8 Multibyte signs single press (ä, ö, ü, ß, €, etc.) */
    if ((unsigned char)c >= 128) {
      int ulen = 1;
      unsigned char uc = (unsigned char)c;
      if ((uc & 0xE0) == 0xC0) ulen = 2;
      else if ((uc & 0xF0) == 0xE0) ulen = 3;
      else if ((uc & 0xF8) == 0xF0) ulen = 4;

      keycode[0] = c;
      int i;
      for (i = 1; i < ulen && i < (int)maxlen - 1; i++) {
        char next_byte;
        if (read(td, &next_byte, 1) > 0) {
          keycode[i] = next_byte;
          append_raw(raw, raw_maxlen, &raw_len, next_byte);
        } else {
          break;
        }
      }
      keycode[i] = '\0';
      return raw_len;
    }
  }

  /* 2. ESC-Sequenzen buffering */
  char seq[64];
  int seq_len = 0;
  seq[seq_len++] = c; /* 27 */

  while (seq_len < (int)sizeof(seq) - 1) {
    fd_set fds;
    struct timeval stv;
    FD_ZERO(&fds);
    FD_SET(td, &fds);
    /* 100ms wait for the 2. Byte (pure ESC-keys), 25ms for follow-Bytes */
    stv.tv_sec = 0;
    stv.tv_usec = (seq_len == 1) ? 100000 : 25000;

    if (select(td + 1, &fds, NULL, NULL, &stv) <= 0) {
      break; /* Timeout reached -> Sequenz Ende */
    }

    char next_c;
    if (read(td, &next_c, 1) <= 0) {
      break;
    }
    seq[seq_len++] = next_c;
    append_raw(raw, raw_maxlen, &raw_len, next_c);

    /* early end, Sequenz is complete */
    if (seq_len >= 2 && seq[1] != '[' && seq[1] != 'O' && seq[1] != 27) {
      /* recognize ALT + UTF-8 Multibyte (z.B. Alt + ä/ö/ü/€) */
      unsigned char u = (unsigned char)seq[1];
      int expected_len = 2; /* ESC + 1 ASCII Byte */
      if ((u & 0xE0) == 0xC0) expected_len = 3; /* 2-Byte UTF-8 (ä, ö, ü, ..) */
      else if ((u & 0xF0) == 0xE0) expected_len = 4; /* 3-Byte UTF-8 (€, ...) */
      else if ((u & 0xF8) == 0xF0) expected_len = 5; /* 4-Byte UTF-8 */

      if (seq_len >= expected_len) {
        break; /* sign fully read */
      }
    } else if (seq[1] == 'O') {
      if (seq_len >= 3) break; /* SS3 Ende */
    } else if (seq[1] == '[') {
      char last = seq[seq_len - 1];
      if (seq_len >= 3 && last >= 0x40 && last <= 0x7E) {
        break; /* CSI Ende */
      }
    } else if (seq[1] == 27) {
      if (seq_len >= 3 && seq[2] != '[' && seq[2] != 'O') {
        break;
      }
      if (seq_len >= 4 && seq[2] == 'O') {
        break;
      }
      if (seq_len >= 4 && seq[2] == '[') {
        char last = seq[seq_len - 1];
        if (last >= 0x40 && last <= 0x7E) break;
      }
    }
  }
  seq[seq_len] = '\0';

  /* only ESC pressed */
  if (seq_len == 1) {
    snprintf(keycode, maxlen, "ESC");
    return raw_len;
  }

  /* check for Doppel-ESC (z.B. ALT + Cursor) */
  int alt_double = 0;
  const char *p = seq;
  int plen = seq_len;
  if (p[0] == 27 && p[1] == 27) {
    alt_double = 1;
    p++;
    plen--;
  }

  /* ALT + single ekys / UTF-8 signs (ESC + sign) */
  if (plen >= 2 && p[1] != '[' && p[1] != 'O') {
    char c2 = p[1];
    if (c2 == 27) {
      snprintf(keycode, maxlen, "KEY_ALT_ESC");
    } else if (c2 == 8 || c2 == 127) {
      snprintf(keycode, maxlen, "KEY_ALT_BACKSPACE");
    } else if (c2 == 9) {
      snprintf(keycode, maxlen, "KEY_ALT_TAB");
    } else if (c2 == 10 || c2 == 13) {
      snprintf(keycode, maxlen, "KEY_ALT_ENTER");
    } else if (c2 == 32) {
      snprintf(keycode, maxlen, "KEY_ALT_SPACE");
    } else if (c2 >= 1 && c2 <= 26) {
      snprintf(keycode, maxlen, "KEY_CTRL_ALT_%c", 'A' + c2 - 1);
    } else if ((unsigned char)c2 >= 128) {
      /* UTF-8 Zeichen nach ALT (z.B. KEY_ALT_ä) */
      snprintf(keycode, maxlen, "KEY_ALT_%s", p + 1);
    } else if (c2 >= 33 && c2 <= 126) {
      snprintf(keycode, maxlen, "KEY_ALT_%c", c2);
    } else {
      snprintf(keycode, maxlen, "KEY_ALT_0x%02X", (unsigned char)c2);
    }
    return raw_len;
  }

  /* SS3 Sequenzen */
  if (p[1] == 'O') {
    parse_ss3_sequence(p, plen, alt_double, keycode, maxlen);
    return raw_len;
  }

  /* CSI Sequenzen */
  if (p[1] == '[') {
    parse_csi_sequence(p, plen, alt_double, keycode, maxlen);
    return raw_len;
  }

  /* Unknown Escape-Sequenz */
  snprintf(keycode, maxlen, "ESC_%s", p + 1);
  return raw_len;
}

int main(int argc, char **argv) {
  int op;
  opterr = 0;
  double timeout = 0.000;
  char prompt[1024];
  int promptlen = 0;
  int bflag = 1;
  int cflag = 0;
  int rflag = 0;
  int xflag = 0;
  int nflag = 0;
  int sflag = 0;

  while ((op = getopt(argc, argv, "cbhnst:p:")) != -1) {
    switch (op) {
      case 'c':
        /* hide cursor on/off */
        cflag = 1;
        break;
      case 'h':
        usage(argv[0]);
        return 0;
      case 'b':
        /* reverse video prompt on/off */
        bflag = 0;
        break;
      case 'n':
        nflag = 1;
        break;
      case 's':
        sflag = 1;
        break;
      case 't': {
        char *endptr;
        timeout = strtod(optarg, &endptr);
        if (optarg == endptr || *endptr != '\0') {
          printf("-t: Invalid numeric input.\n");
          return 1;
        }
        break;
      }
      case 'p':
        promptlen = snprintf(prompt, sizeof(prompt), "%s", optarg);
        break;
      case '?':
        if (optopt == 't')
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

  if (timeout < 0) {
    printf("negative timeout. Cant do timetravel..\n");
    return 1;
  }

  if (timeout > 31536000) {
    printf("maximum timeout is 31536000 sec. Cant wait forever..\n");
    return 1;
  }

  if (timeout == 0) {
    timeout = 31536000;
  }

  /* TTY öffnen */
  int td = open("/dev/tty", O_RDWR | O_NOCTTY);
  if (td == -1) {
    perror("open tty");
    return 1;
  }

  /* TTY vorhanden? Sonst Fallback auf STDIN */
  int r;
  int usetty = 0;
  struct termios oldt, newt;
  r = tcgetattr(td, &oldt);
  if (r == -1) {
    close(td);
    td = STDIN_FILENO;
  } else {
    usetty = 1;
    newt = oldt;
    /* ICANON, ECHO, ISIG (für Ctrl+C etc.) und IXON 
       (für Ctrl+S/Q) ausschalten 
    */
    newt.c_lflag &= ~(ICANON | ECHO | ISIG);
    newt.c_iflag &= ~(IXON);
    r = tcsetattr(td, TCSANOW, &newt);
    if (r == -1) {
      usetty = 0;
      td = STDIN_FILENO;
    }
  }

  fd_set set;
  struct timeval tv;
  FD_ZERO(&set);
  FD_SET(td, &set);
  tv.tv_sec = (int)timeout;
  tv.tv_usec = (int)((timeout - (int)timeout) * 1000000);

  /* Cursor hide & print Prompt */
  if (cflag == 0) {
    write(td, "\033[?25l", 6);
  }
  if (bflag == 0) {
    write(td, "\033[7m", 4);
  }
  write(td, prompt, promptlen);
  /* Not sure if this line is useless :) */
  /* write(td, "\r", 1); */

  if (bflag == 0) {
    write(td, "\033[0m", 4);
  }

  int ret = select(td + 1, &set, NULL, NULL, &tv);
  if (ret > 0) {
    char keycode[64] = {0};
    char rawkey[64] = {0};
    int rawlen = read_and_decode_key(td, keycode, sizeof(keycode),
                                     rawkey, sizeof(rawkey));

    /* empty line, Cursor activate & Terminal reset */
    write(td, "\033[2K\r", 5);
    if (cflag == 0) {
      write(td, "\033[?25h", 6);
    }
    if (usetty == 1) {
      tcsetattr(td, TCSANOW, &oldt);
    }
    /* Keycode oder undecodierten Key ausgeben */
      /* this is how raw key can be printed out */
      /* fwrite(rawkey, 1, (size_t)rawlen, stdout); */

    if ((nflag == 1 && sflag == 1) || (nflag == 0 && sflag == 0)) {
      /* both */
      int i;
      printf("[");
      for (i = 0; i < rawlen; i++) {
        printf("%s%02X", (i > 0) ? " " : "", (unsigned char)rawkey[i]);
      }
      printf("] ");
      printf("%s\n", keycode);
    } else if (nflag == 1 && sflag == 0) {
      /* numeric */
      int i;
      for (i = 0; i < rawlen; i++) {
        printf("%s%02X", (i > 0) ? " " : "", (unsigned char)rawkey[i]);
      }
      printf("\n");
    } else if (nflag == 0 && sflag == 1) {
      /* key name */
      printf("%s\n", keycode);
    }      

  /* end of output */
  } else {
    /* Timeout: empty line & Terminal reset */
    write(td, "\033[2K\r", 5);
    if (cflag == 0) {
      write(td, "\033[?25h", 6);
    }
    if (usetty == 1) {
      tcsetattr(td, TCSANOW, &oldt);
    }
    puts("");
  }

  if (usetty == 1) {
    close(td);
  }

  return 0;
}

