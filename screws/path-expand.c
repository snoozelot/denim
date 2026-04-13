#!/usr/bin/env -S ccraft
// path-expand — expand shortened paths to full paths
//
// WHAT IT DOES
//
// For each directory level, expands a prefix to the full directory name
// if it uniquely matches exactly one entry. If ambiguous or no match,
// keeps the original. The reverse of path-shorten.
//
// USAGE
//
//   path-expand PATH           - expand single path
//   ... | path-expand          - expand paths in stdin text
//
// EXAMPLES
//
//   path-expand /h/l/bin/bad/parse_number.h
//   -> /home/luthor/bin/bad/parse_number.h
//
//   echo "error in /h/l/src/main.c" | path-expand
//   -> error in /home/luthor/src/main.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>

#define MAX_PARTS 256
#define MAX_PATH 4096
#define MAX_LINE 8192


// ==========================================================================
// Predicates
// ==========================================================================

static int
is_path_char(char c) {
    return isalnum(c) || c == '/' || c == '.' || c == '_' || c == '-';
}

static int
is_dot_entry(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static int
is_empty(size_t n) {
    return n == 0;
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
has_content(const char *s) {
    return s[0] != '\0';
}

static int
ends_with_slash(const char *s) {
    size_t len = strlen(s);
    return len > 0 && s[len - 1] == '/';
}

static int
should_read_stdin(int argc) {
    return argc == 1 && !isatty(STDIN_FILENO);
}

static int
starts_with(const char *name, const char *prefix) {
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

static int
needs_leading_slash(int absolute, const char *output) {
    return absolute && output[0] != '/';
}


// ==========================================================================
// String operations
// ==========================================================================

static void
die(const char *msg) {
    fprintf(stderr, "path-expand: %s\n", msg);
    exit(1);
}

static void
append_slash(char *s) {
    if (!ends_with_slash(s))
        strcat(s, "/");
}

static void
prepend_slash(char *s) {
    memmove(s + 1, s, strlen(s) + 1);
    s[0] = '/';
}


// ==========================================================================
// Directory matching
// ==========================================================================

// Find unique match for prefix in directory. Returns match or NULL.
static const char *
find_unique_match(const char *dir, const char *prefix) {
    DIR *d;
    struct dirent *e;
    static char match[256];
    int count = 0;

    d = opendir(dir);
    if (d == NULL)
        return NULL;

    while ((e = readdir(d)) != NULL) {
        if (is_dot_entry(e->d_name))
            continue;
        if (starts_with(e->d_name, prefix)) {
            if (count == 0)
                strncpy(match, e->d_name, sizeof(match) - 1);
            count++;
        }
    }
    closedir(d);

    return count == 1 ? match : NULL;
}


// ==========================================================================
// Path expansion
// ==========================================================================

static void
expand_path(const char *input, char *output, size_t size) {
    char *path;
    char *parts[MAX_PARTS];
    size_t n = 0;
    char *p;
    char parent[MAX_PATH];
    int absolute;
    const char *expanded;

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

    for (size_t i = 0; i < n; i++) {
        expanded = find_unique_match(parent, parts[i]);
        if (expanded == NULL)
            expanded = parts[i];

        if (has_content(output))
            strcat(output, "/");
        strcat(output, expanded);

        append_slash(parent);
        strcat(parent, expanded);
    }

    if (needs_leading_slash(absolute, output))
        prepend_slash(output);

    free(path);
}


// ==========================================================================
// Stream processing
// ==========================================================================

static void
extract_path(const char *line, size_t *i, char *path, size_t size) {
    size_t j = 0;

    while (line[*i] != '\0' && is_path_char(line[*i]) && j < size - 1)
        path[j++] = line[(*i)++];
    path[j] = '\0';
}

static void
process_stdin(void) {
    char line[MAX_LINE];
    char path[MAX_PATH];
    char expanded[MAX_PATH];
    size_t i;

    while (fgets(line, sizeof(line), stdin) != NULL) {
        i = 0;
        while (line[i] != '\0') {
            if (is_path_char(line[i])) {
                extract_path(line, &i, path, sizeof(path));

                if (has_slash(path)) {
                    expand_path(path, expanded, sizeof(expanded));
                    fputs(expanded, stdout);
                } else {
                    fputs(path, stdout);
                }
            } else {
                putchar(line[i++]);
            }
        }
    }
}


// ==========================================================================
// Main
// ==========================================================================

int
main(int argc, char **argv) {
    char expanded[MAX_PATH];

    if (should_read_stdin(argc)) {
        process_stdin();
        return 0;
    }

    if (argc != 2)
        die("usage: path-expand PATH");

    expand_path(argv[1], expanded, sizeof(expanded));
    printf("%s\n", expanded);
    return 0;
}
