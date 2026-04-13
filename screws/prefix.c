#!/usr/bin/env -S ccraft
// prefix — Prefix every line of input with a given string
//
// WHAT IT DOES
//
// Reads stdin and outputs each line prefixed with a given string.
// Useful for log formatting, indentation, or marking output.
//
// USAGE
//
//   prefix [-p PREFIX] [PREFIX]
//
// OPTIONS
//
//   -p PREFIX   Use PREFIX (allows '-h' as prefix without triggering help)
//   -h          Show help
//
// EXAMPLES
//
//   echo -e "foo\nbar" | prefix ">> "    # >> foo\n>> bar
//   cat log.txt | prefix "[app] "        # prefix each line

#include <stdio.h>
#include <unistd.h>

void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-p prefix] [prefix]\n", prog);
    fprintf(stderr, "  -p prefix  Use prefix (allows '-h' as prefix)\n");
    fprintf(stderr, "  -h         Show help\n");
}

int main(int argc, char *argv[]) {
    const char *prefix = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "hp:")) != -1) {
        switch (opt) {
        case 'h': usage(argv[0]); return 0;
        case 'p': prefix = optarg; break;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!prefix) prefix = (optind < argc) ? argv[optind] : "";

    int sol = 1; // start of line
    int c;

    while ((c = getchar()) != EOF) {
        if (sol) fputs(prefix, stdout);
        putchar(c);
        sol = (c == '\n');
    }

    return ferror(stdin) ? 1 : 0;
}
