/* Copyright (c) 2026, Manfred Güntner
    SPDX-License-Identifier: BSD-2-Clause */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h> 
#include <stdlib.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <stdbool.h>

#define MAX_PROMPT_LEN 2048

void usage(char *prog) {
    printf("Usage: %s [-hb] [-c char] [-p text]\n", prog);
    printf("  -h         Display this help message\n");
    printf("  -p text    Set the bar text\n");
    printf("  -b         Display bar text in reverse video\n");
    printf("  -c char    Set the filler char (default: '_').\n");
}

int main(int argc, char **argv) {
    int op;
    opterr = 0;
    char prompt[MAX_PROMPT_LEN] = "";
    int promptlen = 0;
    bool bflag = false;
    char fc = '_';

    while ((op = getopt(argc, argv, "hbc:p:")) != -1) {
        switch (op) {
            case 'h':
                usage(argv[0]);
                return 0;
            case 'b':
                bflag = true;
                break;
            case 'c':
                if (optarg[0] == '\0') fc = '_';
                else fc = optarg[0];
                break;
            case 'p':
                promptlen = snprintf(prompt, MAX_PROMPT_LEN, "%s", optarg);
                if (promptlen >= MAX_PROMPT_LEN) {
                    fprintf(stderr, "Error: Prompt too long.\n");
                    return 1;
                }
                break;
            case '?':
                return 1;
            default:
                return 1;
        }
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

    // Berechne die verfügbare Breite für den Prompt
    // Wir ziehen 1 ab für einen Sicherheitsabstand am Ende
    // int max_display_width = w.ws_col - 1;

    int display_len;
    if (promptlen > w.ws_col) {
      display_len = w.ws_col;
    } else {
      display_len = promptlen;
    }

    if (bflag) {
        printf("\033[7m");
    } 
    printf("%.*s", display_len, prompt);

    // Berechne die Füllung
    int fill_count = w.ws_col - display_len;
    if (fill_count < 0) fill_count = 0;

    for (int i = 0; i < fill_count; i++) {
        putchar(fc);
    }
    if (bflag) {
        printf("\033[0m");
    } 
    putchar('\n');

    close(td);
    return 0;
}

