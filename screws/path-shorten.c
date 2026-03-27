#!/usr/bin/env -S ccraft
// path-shorten - shorten paths using minimal unique prefixes
//
// For each directory level (except the last), finds the shortest prefix
// that uniquely identifies it among its siblings. The final component
// (filename) is kept unchanged.
//
// Usage:
//   path-shorten PATH           - shorten single path
//   ... | path-shorten          - shorten paths in stdin text
//
// Examples:
//   path-shorten /home/luthor/bin/bad/parse_number.h
//   -> /h/l/bin/bad/parse_number.h
//
//   echo "error in /home/luthor/src/main.c" | path-shorten
//   -> error in /h/l/src/main.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>

#define MAX_PARTS 256
#define MAX_PATH 4096
#define MAX_LINE 8192

static void
die(const char *msg) {
    fprintf(stderr, "path-shorten: %s\n", msg);
    exit(1);
}

static int
is_path_char(char c) {
    return isalnum(c) || c == '/' || c == '.' || c == '_' || c == '-';
}

static int
is_slash(char c) {
    return c == '/';
}

static int
is_dot_entry(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static int
starts_with(const char *name, const char *prefix, size_t len) {
    return strncmp(name, prefix, len) == 0;
}

static int
is_unique(int count) {
    return count == 1;
}

static int
is_empty(size_t n) {
    return n == 0;
}

static int
is_last(size_t i, size_t n) {
    return i == n - 1;
}

static int
has_content(const char *s) {
    return s[0] != '\0';
}

static int
ends_with_slash(const char *s) {
    size_t len = strlen(s);
    return len > 0 && s[len - 1] == '/';
}

static int
is_absolute(const char *path) {
    return path[0] == '/';
}

static int
has_slash(const char *s) {
    return strchr(s, '/') != NULL;
}

static int
should_read_stdin(int argc) {
    return argc == 1 && !isatty(STDIN_FILENO);
}

static int
needs_leading_slash(int absolute, const char *output) {
    return absolute && output[0] != '/';
}

static int
count_matches(DIR *dir, const char *target, size_t len) {
    struct dirent *e;
    int count = 0;

    rewinddir(dir);
    while ((e = readdir(dir)) != NULL) {
        if (is_dot_entry(e->d_name)) {
            continue;
        }
        if (starts_with(e->d_name, target, len)) {
            count++;
        }
    }
    return count;
}

static size_t
find_min_prefix(const char *parent, const char *target) {
    DIR *dir;
    size_t len;
    size_t max;

    dir = opendir(parent);
    if (dir == NULL) {
        return strlen(target);
    }

    max = strlen(target);
    for (len = 1; len <= max; len++) {
        if (is_unique(count_matches(dir, target, len))) {
            closedir(dir);
            return len;
        }
    }

    closedir(dir);
    return max;
}

static void
append_slash(char *s) {
    if (ends_with_slash(s)) {
        return;
    }
    strcat(s, "/");
}

static void
prepend_slash(char *s) {
    memmove(s + 1, s, strlen(s) + 1);
    s[0] = '/';
}

static void
shorten_path(const char *input, char *output, size_t size) {
    char *path;
    char *parts[MAX_PARTS];
    size_t n = 0;
    char *p;
    char parent[MAX_PATH];
    char shortened[256];
    size_t i;
    size_t len;
    int absolute;

    path = strdup(input);
    if (path == NULL) {
        strncpy(output, input, size - 1);
        output[size - 1] = '\0';
        return;
    }

    absolute = is_absolute(path);

    p = strtok(path, "/");
    while (p != NULL && n < MAX_PARTS) {
        parts[n++] = p;
        p = strtok(NULL, "/");
    }

    if (is_empty(n)) {
        strncpy(output, input, size - 1);
        output[size - 1] = '\0';
        free(path);
        return;
    }

    output[0] = '\0';
    strcpy(parent, absolute ? "/" : ".");

    for (i = 0; i < n; i++) {
        if (is_last(i, n)) {
            if (has_content(output)) {
                strcat(output, "/");
            }
            strcat(output, parts[i]);
            break;
        }

        len = find_min_prefix(parent, parts[i]);
        strncpy(shortened, parts[i], len);
        shortened[len] = '\0';

        if (has_content(output)) {
            strcat(output, "/");
        }
        strcat(output, shortened);

        append_slash(parent);
        strcat(parent, parts[i]);
    }

    if (needs_leading_slash(absolute, output)) {
        prepend_slash(output);
    }

    free(path);
}

static void
extract_path(const char *line, size_t *i, char *path, size_t size) {
    size_t j = 0;

    while (line[*i] != '\0' && is_path_char(line[*i]) && j < size - 1) {
        path[j++] = line[(*i)++];
    }
    path[j] = '\0';
}

static void
process_stdin(void) {
    char line[MAX_LINE];
    char path[MAX_PATH];
    char shortened[MAX_PATH];
    size_t i;

    while (fgets(line, sizeof(line), stdin) != NULL) {
        i = 0;
        while (line[i] != '\0') {
            if (is_path_char(line[i])) {
                extract_path(line, &i, path, sizeof(path));

                if (has_slash(path)) {
                    shorten_path(path, shortened, sizeof(shortened));
                    fputs(shortened, stdout);
                } else {
                    fputs(path, stdout);
                }
            } else {
                putchar(line[i++]);
            }
        }
    }
}

int
main(int argc, char **argv) {
    char shortened[MAX_PATH];

    if (should_read_stdin(argc)) {
        process_stdin();
        return 0;
    }

    if (argc != 2) {
        die("usage: path-shorten PATH");
    }

    shorten_path(argv[1], shortened, sizeof(shortened));
    printf("%s\n", shortened);
    return 0;
}
