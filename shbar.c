/* Copyright (c) 2026, Manfred Güntner
    SPDX-License-Identifier: BSD-2-Clause */

/* shbar is a simple divider, header and footer bar */

#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h> 
#include <stdlib.h>
#include <ctype.h>
#include <sys/ioctl.h>

void usage(char *prog) {
  printf("Usage: %s [-hb] [-c char] [-p text]\n", prog);
  printf("  -h         Display this help message\n");
  printf("  -p text    Set the bar text\n");
  printf("  -b         Display bar text in reverse video\n");
  printf("  -c char    Set the filler char.\n");
}

void repeat_char(char c, int n) {
  int i;
  for (i = 0; i < n; i++) {
    putchar(c);
  }
}

int 
main(int argc, char **argv) {
  int op;
  opterr = 0;
  char prompt[1024];
  int promptlen = 0;
  int bflag = 0;
  char fc = '_';
 
  while ((op = getopt (argc, argv, "hbc:p:")) != -1)
  {
    switch (op)
      {
      case 'h':
        usage(argv[0]);
        return 0;
      case 'b':
        bflag = 1;
        break;
      case 'c':
        fc = optarg[0];
        break;
      case 'p':
        promptlen = snprintf(prompt, 1024, "%s", optarg);
        break;    
      case '?':
        if (optopt == 'p' || optopt == 'c')
          fprintf (stderr, "Option -%c requires an argument.\n", optopt);
        else if (isprint (optopt))
          fprintf (stderr, "Unknown option `-%c'.\n", optopt);
        else
          fprintf (stderr,
                   "Unknown option character `\\x%x'.\n",
                   optopt);
        return 1;
      default:
        abort ();
      }
  }

  /* open tty */
  int td = -1;
  td = open("/dev/tty", O_RDWR | O_NOCTTY);
  if (td == -1) {
    perror("open tty");
    return 1;
  }
  /* terminal dimensions */
  struct winsize w = {0};
  if (ioctl(td, TIOCGWINSZ, &w) == -1 || w.ws_col == 0) {
    perror ("ioctl");
    close(td);
    return 1;
  }
  
  /* repeat_char(fc,2); */
  int diff = 1;

  int fill = w.ws_col - (promptlen + diff);
  if (fill < 0) {
    fill = 0;
  }

  if (bflag == 0) {
    printf("%.*s", w.ws_col - diff, prompt);
  } else {
    printf("\033[7m%.*s\033[0m", w.ws_col - diff, prompt);
  }
  
  repeat_char(fc, fill); 
  printf("\n");
  
  /* close tty */
  close(td);

  /* Thats all folks */
  return 0;
}
