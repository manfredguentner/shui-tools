
/* Copyright (c) 2026, Manfred Güntner
    SPDX-License-Identifier: BSD-2-Clause */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h> 
#include <stdlib.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <stdbool.h>

#define MAX_PROMPT_LEN 2048

void usage(char *prog) {
    printf("Usage: %s [-bhilmnruv] [-c char] [-p text]\n", prog);
    printf("  -b         Display bar text in bold\n");
    printf("  -c char    Set the filler char (default: '_').\n");
    printf("  -h         Display this help message\n");
    printf("  -i         Display bar text italic\n");
    printf("  -l         Left align\n");
    printf("  -m         Fill only to the middle of terminal\n");
    printf("  -n         Returns columns-length_of_text\n");
    printf("  -p text    Set the bar text\n");
    printf("  -r         Right align\n");
    printf("  -u         Displays bar text underlined\n");
    printf("  -v         Displays bar text in reverse color\n");
}

int main(int argc, char **argv) {
    int op;
//    opterr = 0;
    char prompt[MAX_PROMPT_LEN] = "";
    int promptlen = 0;
    bool nflag = false;
    bool rflag = false;
    bool lflag = false;
    bool mflag = false;

    char fc = ' ';

    const char *codes[8];
    int code_count = 0;

    while ((op = getopt(argc, argv, "bc:hilmnp:ruv")) != -1) {
        switch (op) {
            case 'b':
                codes[code_count++] = "1";
                break;
            case 'c':
                if (optarg[0] == '\0') fc = '_';
                else fc = optarg[0];
                break;
            case 'h':
                usage(argv[0]);
                return 0;
            case 'i':
                codes[code_count++] = "3";
                break;
            case 'l':
                lflag = true;
                break;
            case 'm':
                mflag = true;
                break;
            case 'n':
                nflag = true;
                break;
            case 'p':
                promptlen = snprintf(prompt, MAX_PROMPT_LEN, "%s", optarg);
                if (promptlen >= MAX_PROMPT_LEN) {
                    fprintf(stderr, "Error: Prompt too long.\n");
                    return 1;
                }
                break;
            case 'r':
                rflag = true;
                break;
            case 'u':
                codes[code_count++] = "4";
                break;
            case 'v':
                codes[code_count++] = "7";
                break;
            case '?':
              if (optopt == 'c' || optopt == 'p') {
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
              } else if (isprint(optopt)) {
                fprintf(stderr, "Unknown option `-%c'.\n", optopt);
              }else {
                fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
              }
              return 1;
            default:
              abort(); 
        }
    }

    /* rflag and lflag cant coexist */
    if (rflag == true && lflag == true) {
      lflag = false;
      rflag = false;
    }

    int td = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (td == -1) {
        perror("open tty");
        return 1;
    }

    struct winsize w = {0};
    if (ioctl(td, TIOCGWINSZ, &w) == -1 || w.ws_col == 0) {
        perror("ioctl");
        close(td);
        return 1;
    }

    /* calculate the filling */
    int fill_count = 0;
    if (mflag) {
      fill_count = (w.ws_col / 2) - promptlen;
    } else {
      fill_count = w.ws_col - promptlen;
    }
    if (fill_count < 0) fill_count = 0;

    if (nflag) {
      printf("%d\n", fill_count);
      close(td);
      return 0;
    }

    /* build and print ansi sequence */
    if (code_count > 0) {
      printf("\033[");
      for (int i = 0; i < code_count; i++) {
        if (i == code_count - 1) {
          printf("%s", codes[i]);
        } else {
          printf("%s;", codes[i]);
        }
      }
      printf("m");
    }

    /* standard */
    if (lflag == false && rflag == false) {    
      if (mflag == false) {
        printf("%.*s", promptlen, prompt);
        for (int i = 0; i < fill_count; i++) {
          putchar(fc);
        }
      } else {
        for (int i = 0; i < fill_count; i++) {
          putchar(fc);
        }
        printf("%.*s", promptlen, prompt);
        for (int i = 0; i < w.ws_col - (promptlen + fill_count); i++) {
          putchar(fc);
        }
      }
    }

    /* lflag */
    if (lflag == true && rflag == false) {    
      printf("%.*s", promptlen, prompt);
      //for (int i = 0; i < half_fill_count + remain_fill_count; i++) {
      for (int i = 0; i < fill_count; i++) {
        putchar(fc);
      }
    }

    /* rflag */
    if (lflag == false && rflag == true) {    
      char nc = ' ';

      if (mflag == true) {
        /* reset all ANSI */
        if (code_count > 0) {
          printf("\033[0m");
        }

        for (int i = 0; i < w.ws_col - (fill_count + promptlen); i++) {
          putchar(nc);
        }
      
        /* restore ANSI */
        if (code_count > 0) {
          printf("\033[");
          for (int i = 0; i < code_count; i++) {
            if (i == code_count - 1) {
              printf("%s", codes[i]);
            } else {
              printf("%s;", codes[i]);
            }
          }
          printf("m");
        }
      } 
      /* and print the real payload */  
      for (int i = 0; i < fill_count; i++) {
        putchar(fc);
      }
      printf("%.*s", promptlen, prompt);
    }

    if (code_count > 0) {
        printf("\033[0m");
    } 
    putchar('\n');

    close(td);
    return 0;
}

