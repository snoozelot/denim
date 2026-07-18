#!/usr/bin/env -S ccraft
// ssam — structural regex stream editor (Plan 9 sam subset)
//
// WHAT IT DOES
//   Read text, apply sam-language commands, write result.  Unlike sed
//   (line-by-line), ssam has a "dot" — a selection spanning any substring.
//   Commands transform dot or run over parts of it.  Commands compose
//   and nest — a tiny programming language for text transformation.
//
// USAGE
//   ssam [-n] [-i[.bak]] [-e cmd] [-f file] [file ...]
//
//   -e cmd      Execute cmd (can be repeated)
//   -f file     Read commands from file
//   -n          Suppress final implicit print
//   -i[.bak]    Edit files in-place (optional backup suffix)
//   -h          Show help
//
// EXAMPLES
//   ssam 's/foo/bar/g' file            Replace all
//   ssam -n ',x g/error/ p'            Print lines with "error"
//   ssam ',x/  +/ c/ /'                Collapse multiple spaces
//   ssam '1,10d' file                  Delete lines 1-10
//   ssam '/start/,/end/ p'             Print between patterns
//   ssam '2c\nCHANGED\n.'             Change line 2 (multi-line)
//   ssam '2d; u'                       Delete line 2, undo
//   ssam -e '# comment; s/a/b/'        Comment before command
//
// ============================================================================
// TUTORIAL: DOT AND STRUCTURAL COMMANDS
// ============================================================================
//
// DOT
// ---
// Dot is the current selection — a range [p1, p2) in the buffer.
// Every command operates on dot.  Addresses change dot:
//
//   #n         Character n            (#0 = start)
//   n          Line n (1-indexed)
//   /re/       Forward search
//   ?re?       Backward search
//   ,          Whole file (0,$)
//   a1+a2      a2 from end of a1
//   a1,a2      Range start a1 to end a2
//
// STRUCTURAL COMMANDS
// -------------------
// These run a subcommand on a narrowed selection:
//
//   x/re/ cmd    For EACH match of re in dot, run cmd on that match
//   g/re/ cmd    If dot CONTAINS a match for re, run cmd on all of dot
//   v/re/ cmd    If dot has NO match for re, run cmd on all of dot
//   y/re/ cmd    For text BETWEEN matches of re, run cmd on each gap
//
// x is the workhorse — loops like s///g but runs any command per
// match.  g/v are conditionals.  y processes gaps between matches.
//
// Commands chain: '2d; u' deletes then undoes.
// Commands nest: ',x/.*\n/ v/skip/ c/bad/' — for each line that
// doesn't contain "skip", replace it.
//
// ALL COMMANDS
// ------------
//   a/i/c/text/    Append/insert/change dot
//   d              Delete dot
//   s/re/text/     Substitute first match
//   s/re/text/g    All matches            sN/re/text/  Nth match
//   m/t addr       Move/copy dot after addr
//   p              Print dot
//   = / =#         Print line/char addresses
//   kx / 'x        Set/recall mark
//   e/r/w/W file   File operations
//   < / > / | cmd  Shell
//   cd             Change directory
//   u [n]          Undo
//   { cmds }       Group
//   #              Comment

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "ssam-regex.h"

// ============================================================================
// Configuration
// ============================================================================

#define INITIAL_CAP     4096
#define DEFAULT_MARK    '\''
#define TEMP_TEMPLATE   "/tmp/ssam.XXXXXX"
#define TMPL_SUFFIX     ".XXXXXX"
#define TMPL_SUFFIX_LEN 8
#define SUBST_INIT_CAP 64

// ============================================================================
// ANSI color codes for diff output
// ============================================================================

#define DIM "\x1b[2m"
#define RST "\x1b[0m"
#define RED "\x1b[31m"
#define GRN "\x1b[32m"

// ============================================================================
// Types
// ============================================================================

struct buf {
    char *data;
    size_t len;
    size_t cap;
};

struct range {
    size_t p1;      // start (inclusive)
    size_t p2;      // end (exclusive)
};

// Point after newline (nl) or end of string
static char *
line_after(char *p, char *nl) {
    return nl ? nl + 1 : p + strlen(p);
}

// Collect match positions from snapshot, apply replacements from end to start
// for O(n) total time (avoids O(n²) copy rebuilds on global substitutions).
struct subst_item {
    size_t pos;
    size_t match_len;
    char *replacement;
    size_t repl_len;
};

static int
subst_item_grow(struct subst_item **items, int *cap) {
    int newcap = *cap ? *cap * 2 : SUBST_INIT_CAP;
    struct subst_item *new = realloc(*items, newcap * sizeof(struct subst_item));
    if (!new) { return 0; }
    *items = new;
    *cap = newcap;
    return 1;
}

static void
subst_item_free_all(struct subst_item *items, int n) {
    for (int k = 0; k < n; k++) { free(items[k].replacement); }
    free(items);
}

#define MAX_UNDO 64

struct undo_snap {
    char *data;
    size_t len;
    struct range dot;
};

struct ssam {
    struct buf text;        // the buffer being edited
    struct range dot;       // current selection
    struct range mark[128]; // marks (set by kx, recall with 'x)
    int suppress_print;     // -n flag
    char *error;
    char *lastshell;        // last shell command
    struct undo_snap undo[MAX_UNDO];
    int undolen;
    int undodepth;          // >0 when executing inside x/y/g/v
    char *last_pattern;     // last regex pattern (for empty-pattern reuse)
    struct regex *cached_re;     // cached compiled regex
    char *cached_re_pat;        // pattern for cached regex
    const char *curfile;    // current file name (argv pointer, not owned)
};

// Forward declarations
struct cmd;
static void execute(struct ssam *s, struct cmd *c);
static void read_into_buf(struct buf *b, FILE *f);
static void dot_from_buf(struct ssam *s, struct buf *b);
static int fopen_error(const char *path);
static void read_path(struct buf *b, const char *path);
static struct regex *get_re_cached(struct ssam *s, const char *pattern);
static struct range match_to_range(const struct rematch *m, const char *text, size_t offset);
static void append_match_ref(struct buf *b, struct rematch *m);

// ============================================================================
// Domain predicates
// ============================================================================

static const char *
str_or_empty(const char *s)    { return s ? s : ""; }

// ============================================================================
// Character predicates
// ============================================================================

static int is_mark(int c)       { return c == '\''; }
static int is_slash(int c)      { return c == '/'; }
static int is_qmark(int c)      { return c == '?'; }
static int is_comma(int c)      { return c == ','; }
static int is_semi(int c)       { return c == ';'; }
static int is_minus(int c)      { return c == '-'; }
static int is_hash(int c)       { return c == '#'; }

static int is_dot_addr(int c)    { return c == '.'; }

static int
is_group_ref(const char *p) {
    return *p == '\\' && p[1] >= '0' && p[1] <= '9';
}

static int is_multiline_text(char *p)    { return *p == '\n'; }

static int
is_end_marker(const char *p, size_t len) { return len == 1 && p[0] == '.'; }

static int is_delimited_text(char *p)    { return !isalnum(*p); }

static int is_valid_mark_char(int c)    { return c && (isalnum(c) || is_mark(c)); }

// True if s starts with "cd" (not as prefix of longer word)
static int
is_cd_cmd(const char *s) {
    return s[0] == 'c' && s[1] == 'd' && !isalnum(s[2]);
}

// Range separator in address expression
static int
is_range_separator(int c)     { return is_comma(c) || is_semi(c); }

// Offset operator (+ or -)
static int
is_offset_op(int c)           { return is_plus(c) || is_minus(c); }

// Address expression character categories
static int is_addr_marker(int c)  { return is_dot_addr(c) || is_dollar(c) || is_hash(c) || is_mark(c); }
static int is_re_addr(int c)     { return is_slash(c) || is_qmark(c); }
static int is_addr_op(int c)     { return is_comma(c) || is_semi(c) || is_plus(c) || is_minus(c); }

static int
is_addr_char(char c) {
    return is_addr_marker(c) || is_re_addr(c) || is_addr_op(c) || is_digit(c);
}

static int
is_comment_start(char c, char next) { return is_hash(c) && !is_addr_char(next);
}

// ============================================================================
// Semantic helpers
// ============================================================================

// Regex is broken (NULL or compile error)
static int
re_is_error(struct regex *re) {
    return !re || re_error(re);
}

// Print regex error if present; return 1 if error
static int
re_report_error(struct regex *re) {
    if (re_is_error(re)) {
        if (re) { fprintf(stderr, "ssam: %s\n", re_error(re)); }
        return 1;
    }
    return 0;
}

// String is absent or empty
static int
is_empty_str(const char *s) {
    return !s || !*s;
}

// True for global substitution (num == -1)
static int
is_global_subst(int num) {
    return num == -1;
}

// Target match number for substitution: Nth or first
static int
nth_target(int num) {
    return num > 0 ? num : 1;
}

// True when inside a subcommand (x/y/g/v/group)
static int
is_inside_subcmd(struct ssam *s) {
    return s->undodepth > 0;
}

// True when at top-level command (not inside subcommand)
static int
is_top_level(struct ssam *s) {
    return s->undodepth == 0;
}

// Write all bytes, retry on short write
static int
write_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0) { return 0; }
        data += n;
        len -= (size_t)n;
    }
    return 1;
}

// Run subcommand with undo depth incremented
static void
run_subcmd(struct ssam *s, struct cmd *sub) {
    if (!sub) { return; }

    s->undodepth++;
    execute(s, sub);
    s->undodepth--;
}

// Parse unsigned integer, advance pointer
static size_t
parse_uint(char **pp) {
    char *p = *pp;
    size_t n = 0;

    while (isdigit(*p)) { n = n * 10 + (*p++ - '0'); }
    *pp = p;
    return n;
}

// Length of text, or 0 if null
static size_t
text_len(const char *t) {
    return t ? strlen(t) : 0;
}

// ============================================================================
// Buffer operations
// ============================================================================

static void
buf_init(struct buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void
buf_free(struct buf *b) {
    free(b->data);
    buf_init(b);
}

static void
nomem(void) {
    fprintf(stderr, "ssam: out of memory\n");
    exit(1);
}

static void
buf_grow(struct buf *b, size_t need) {
    if (b->len + need <= b->cap) { return; }

    size_t cap = b->cap ? b->cap * 2 : INITIAL_CAP;
    while (cap < b->len + need) { cap *= 2; }
    char *new = realloc(b->data, cap);
    if (!new) { nomem(); }
    b->data = new;
    b->cap = cap;
}

static void
buf_nulterm(struct buf *b) {
    buf_grow(b, 1);
    b->data[b->len] = '\0';
}

static void
buf_append(struct buf *b, const char *s, size_t len) {
    if (len == 0) { return; }

    buf_grow(b, len);
    memcpy(b->data + b->len, s, len);
    b->len += len;
    buf_nulterm(b);
}

static void
buf_append_str(struct buf *b, const char *s) {
    buf_append(b, s, strlen(s));
}

static void
buf_append_char(struct buf *b, char c) {
    buf_grow(b, 1);
    b->data[b->len++] = c;
}

static void
buf_replace(struct buf *b, size_t p1, size_t p2, const char *s, size_t len) {
    size_t old_len = p2 - p1;

    // Grow by the delta; existing capacity suffices when shrinking.
    if (len > old_len) {
        buf_grow(b, len - old_len);
    }

    if (p2 < b->len) {
        memmove(b->data + p1 + len, b->data + p2, b->len - p2);
    }
    if (s && len > 0) {
        memcpy(b->data + p1, s, len);
    }
    b->len = b->len - old_len + len;
    buf_nulterm(b);
}

static char *
buf_dup(struct buf *b, size_t p1, size_t p2) {
    size_t len = p2 - p1;
    char *s = malloc(len + 1);

    if (!s) { return NULL; }

    if (len > 0 && b->data) {
        memcpy(s, b->data + p1, len);
    }
    s[len] = '\0';
    return s;
}

// ============================================================================
// Line/position utilities
// ============================================================================

// Find start of line n (1-indexed). Line 0 = position 0.
static size_t
line_start(struct buf *b, int n) {
    if (n <= 0) { return 0; }

    size_t pos = 0;
    int line = 1;
    while (pos < b->len && line < n) {
        if (b->data[pos] == '\n') { line++; }
        pos++;
    }
    return pos;
}

// Find end of line containing pos (after newline or at EOF)
static size_t
line_end(struct buf *b, size_t pos) {
    while (pos < b->len && b->data[pos] != '\n') { pos++; }
    if (pos < b->len) { pos++; }  // include newline
    return pos;
}

// Find start of line containing pos
static size_t
line_begin(struct buf *b, size_t pos) {
    while (pos > 0 && b->data[pos - 1] != '\n') { pos--; }
    return pos;
}

// Position of the line before the one containing r.p1
static struct range
prev_line_pos(struct ssam *s, struct range r) {
    size_t start = line_begin(&s->text, r.p1);
    if (start > 0) { start = line_begin(&s->text, start - 1); }

    struct range prev;
    prev.p1 = start;
    prev.p2 = line_end(&s->text, start);
    return prev;
}

// Count line number at position
static int
line_number(struct buf *b, size_t pos) {
    int n = 1;

    for (size_t i = 0; i < pos && i < b->len; i++) {
        if (b->data[i] == '\n') { n++; }
    }
    return n;
}

// ============================================================================
// Regex helpers
// ============================================================================

// Search for pattern in [start, start+len), store result in out, return found flag.
// Temporarily null-terminates the slice (no-slice optimization).
static int
search_in_slice(struct regex *re, const char *data, size_t len,
                size_t offset, struct range *out) {
    if (len == 0) { return 0; }

    size_t end = offset + len;
    char saved = data[end];
    char *mutable = (char *)data;
    mutable[end] = '\0';

    struct rematch m[1];
    int found = re_search(re, mutable + offset, m, 1);

    if (found) {
        *out = match_to_range(m, mutable, offset);
    }

    mutable[end] = saved;
    return found;
}

static const char *use_pattern(struct ssam *s, const char *pattern);

static int
search_forward(struct ssam *s, const char *pattern, size_t from, struct range *out) {
    if (from > s->text.len) { return 0; }

    pattern = use_pattern(s, pattern);
    if (!pattern) { return 0; }

    struct regex *re = get_re_cached(s, pattern);
    if (re_report_error(re)) { return 0; }

    // Search from 'from' to end, then wrap to start
    int found = search_in_slice(re, s->text.data, s->text.len - from, from, out);
    if (!found) {
        found = search_in_slice(re, s->text.data, from, 0, out);
    }

    return found;
}

// Find last match in [0, limit).  Iterate all matches, keep the one before limit.
static int
last_match_before(const char *text, size_t len, struct regex *re,
                  size_t limit, struct range *out) {
    size_t offset = 0;
    size_t last_p1 = 0, last_p2 = 0;
    int found = 0;
    struct rematch m[1];

    while (re_search(re, text + offset, m, 1)) {
        struct range mr = match_to_range(m, text, offset);
        if (mr.p2 <= limit) { last_p1 = mr.p1; last_p2 = mr.p2; found = 1; }
        offset = mr.p1 + 1;
        if (mr.p1 == mr.p2) { offset++; }
        if (offset >= len) { break; }
    }

    if (found) { out->p1 = last_p1; out->p2 = last_p2; }
    return found;
}

static int
search_backward(struct ssam *s, const char *pattern, size_t from, struct range *out) {
    if (from == 0) { from = s->text.len; }

    pattern = use_pattern(s, pattern);
    if (!pattern) { return 0; }

    struct regex *re = get_re_cached(s, pattern);
    if (re_report_error(re)) { return 0; }

    // Find last match before 'from'; if none, wrap and find last overall
    const char *text = s->text.data;
    size_t len = s->text.len;
    int found = last_match_before(text, len, re, from, out);

    if (!found) {
        found = last_match_before(text, len, re, len, out);
    }

    return found;
}

// ============================================================================
// Parsing helpers
// ============================================================================

static char *
skip_space(char *p) {
    while (*p == ' ' || *p == '\t') { p++; }
    return p;
}

static char *
skip_separators(char *p) {
    while (*p == '\n' || *p == ';') { p++; }
    return p;
}

// Append escaped delimiter text: \n → newline, \t → tab, \r → CR,
// \\ → backslash, \delim → delimiter, other \X preserved for regex
static void
append_delim_escape(struct buf *b, char c, char delim) {
    switch (c) {
    case 'n':  buf_append_char(b, '\n'); break;
    case 't':  buf_append_char(b, '\t'); break;
    case 'r':  buf_append_char(b, '\r'); break;
    case '\\': buf_append_char(b, '\\'); break;
    default:
        if (c == delim) { buf_append_char(b, delim); }
        else { buf_append_char(b, '\\'); buf_append_char(b, c); }
        break;
    }
}

// Append next unescaped character; return 1 at delimiter (stop), 0 otherwise.
static int
append_unescaped(struct buf *b, char **pp, char delim) {
    char *p = *pp;

    if (*p == '\\' && p[1]) {
        p++;
        append_delim_escape(b, *p, delim);
        p++;
    } else if (*p == delim) {
        p++;
        *pp = p;
        return 1;
    } else {
        buf_append_char(b, *p++);
    }
    *pp = p;
    return 0;
}

// Extract delimited text, handling escapes
static char *
extract_delimited_dup(char **pp, char delim) {
    char *p = *pp;
    struct buf b;
    buf_init(&b);

    while (*p && !append_unescaped(&b, &p, delim)) {}

    buf_append_char(&b, '\0');
    *pp = p;
    return b.data;
}

// Read one line from multiline text, append to buffer. Return 1 if end
// marker found, 0 otherwise. Advances *pp past the line.
static int
append_line_to_buffer(struct buf *b, char **pp) {
    char *p = *pp;
    char *nl = strchr(p, '\n');
    size_t linelen = nl ? (size_t)(nl - p) : strlen(p);

    if (is_end_marker(p, linelen)) {
        *pp = line_after(p, nl);
        return 1;
    }
    buf_append(b, p, nl ? linelen + 1 : linelen);
    *pp = line_after(p, nl);
    return 0;
}

static char *
read_multiline_text(char **pp) {
    char *p = *pp + 1;
    struct buf b;
    buf_init(&b);

    while (*p) {
        if (append_line_to_buffer(&b, &p)) { break; }
    }

    buf_append_char(&b, '\0');
    *pp = p;
    return b.data;
}

static char *
extract_text_dup(char **pp) {
    char *p = *pp;

    if (!*p) { return NULL; }
    if (is_multiline_text(p))  { return read_multiline_text(pp); }
    if (!is_delimited_text(p)) { return NULL; }

    char delim = *p++;
    char *text = extract_delimited_dup(&p, delim);
    *pp = p;
    return text;
}

// Strip trailing spaces/tabs in-place
static void
strip_trailing(char *s, size_t *len) {
    while (*len > 0 && (s[*len-1] == ' ' || s[*len-1] == '\t')) {
        s[--*len] = '\0';
    }
}

// Extract token delimited by space/newline/brace/semicolon, strip trailing space
static char *
alloc_token(char *start, size_t len) {
    char *tok = malloc(len + 1);
    if (!tok) { return NULL; }

    memcpy(tok, start, len);
    tok[len] = '\0';
    strip_trailing(tok, &len);
    return tok;
}

// Extract token delimited by space/newline/brace/semicolon, strip trailing space
static char *
read_token(char **pp, int stop_at_semi) {
    char *p = skip_space(*pp);
    char *start = p;

    while (*p && *p != '\n' && *p != '}') {
        if (stop_at_semi && *p == ';') { break; }
        p++;
    }
    size_t len = p - start;

    *pp = p;
    if (len == 0) { return NULL; }
    return alloc_token(start, len);
}

static char *
read_cd_dir(char *p) {
    return read_token(&p, 1);
}

static char *
extract_shell_dup(char **pp) {
    return read_token(pp, 0);
}

// ============================================================================
// Address parsing
// ============================================================================

// Skip over a regex delimited by c (/, ?), including escapes
static char *
skip_regex_in_addr(char *p, char delim) {
    p++;
    while (*p && *p != delim) {
        if (*p == '\\' && p[1]) { p++; }
        p++;
    }
    if (*p == delim) { p++; }
    return p;
}

// Advance pointer past one address token (regex, mark, or literal char).
static char *
advance_over_addr(char *p) {
    if (is_slash(*p) || is_qmark(*p)) {
        return skip_regex_in_addr(p, *p);
    }

    if (is_mark(*p)) {
        p++;
        if (is_valid_mark_char(*p)) { p++; }
        return p;
    }

    return p + 1;
}

// Duplicate the address string at *pp, advance pointer.
static char *
collect_addr_string(char **pp) {
    char *p = skip_space(*pp);
    char *start = p;

    while (is_addr_char(*p)) {
        p = advance_over_addr(p);
    }
    if (p == start) { *pp = p; return NULL; }

    size_t len = p - start;
    char *s = malloc(len + 1);
    if (!s) { *pp = p; return NULL; }
    memcpy(s, start, len);
    s[len] = '\0';
    *pp = p;
    return s;
}

static struct range
eval_addr(struct ssam *s, char **pp, struct range base);

// --- Address type matchers ---

// Match current dot (.)
static int
try_dot_addr(char **pp, struct ssam *s, struct range *out) {
    char *p = *pp;
    if (!is_dot_addr(*p)) { return 0; }
    *out = s->dot;
    *pp = p + 1;
    return 1;
}

// Match end of file ($)
static int
try_end_addr(char **pp, struct ssam *s, struct range *out) {
    char *p = *pp;
    if (!is_dollar(*p)) { return 0; }
    out->p1 = out->p2 = s->text.len;
    *pp = p + 1;
    return 1;
}

// Match character position (#n)
static int
try_char_addr(char **pp, struct ssam *s, struct range *out) {
    char *p = *pp;
    if (!is_hash(*p)) { return 0; }
    p++;
    size_t n = parse_uint(&p);

    if (n > s->text.len) { n = s->text.len; }
    out->p1 = out->p2 = n;
    *pp = p;
    return 1;
}

// Match mark recall ('x)
static int
try_mark_addr(char **pp, struct ssam *s, struct range *out) {
    char *p = *pp;
    if (!is_mark(*p)) { return 0; }
    p++;

    int mark_idx = (unsigned char)*p;
    if (is_valid_mark_char(mark_idx)) {
        p++;
    } else {
        mark_idx = '\'';
    }
    *out = s->mark[mark_idx];
    *pp = p;
    return 1;
}

// Match forward search (/re/)
static int
try_forward_addr(char **pp, struct ssam *s, struct range base, struct range *out) {
    char *p = *pp;
    if (!is_slash(*p)) { return 0; }
    p++;

    char *re = extract_delimited_dup(&p, '/');
    int found = search_forward(s, re, base.p2, out);
    free(re);
    *pp = p;
    return found;
}

// Match backward search (?re?)
static int
try_backward_addr(char **pp, struct ssam *s, struct range base, struct range *out) {
    char *p = *pp;
    if (!is_qmark(*p)) { return 0; }
    p++;

    char *re = extract_delimited_dup(&p, '?');
    int found = search_backward(s, re, base.p1, out);
    free(re);
    *pp = p;
    return found;
}

// Match zero address (0)
static int
try_zero_addr(char **pp, struct range *out) {
    char *p = *pp;
    if (*p != '0' || isdigit(p[1])) { return 0; }
    out->p1 = out->p2 = 0;
    *pp = p + 1;
    return 1;
}

// Match line number address (n)
static int
try_line_addr(char **pp, struct ssam *s, struct range *out) {
    char *p = *pp;
    if (!isdigit(*p)) { return 0; }

    int n = parse_uint(&p);

    if (n == 0) {
        out->p1 = out->p2 = 0;
    } else {
        out->p1 = line_start(&s->text, n);
        out->p2 = line_end(&s->text, out->p1);
    }
    *pp = p;
    return 1;
}

// Parse simple address: try each address type, first match wins
static struct range
parse_simple_addr(struct ssam *s, char **pp, struct range base) {
    char *p = skip_space(*pp);
    struct range r = base;

    // Try each address type, first match wins
    (void)(
        try_dot_addr(&p, s, &r) ||
        try_end_addr(&p, s, &r) ||
        try_char_addr(&p, s, &r) ||
        try_mark_addr(&p, s, &r) ||
        try_forward_addr(&p, s, base, &r) ||
        try_backward_addr(&p, s, base, &r) ||
        try_zero_addr(&p, &r) ||
        try_line_addr(&p, s, &r)
    );

    *pp = p;
    return r;
}

// True if *p starts a new address expression (not an operator or comma/semi)
static int
has_next_addr(const char *p) {
    int c = (unsigned char)*p;
    return is_addr_char(c) && !is_range_separator(c) && !is_offset_op(c);
}

// True if *p starts a range endpoint (address that can follow , or ;)
static int
has_range_end(const char *p) {
    int c = (unsigned char)*p;
    return is_addr_char(c) && !is_range_separator(c);
}

static struct range
addr_plus(struct ssam *s, char **p, struct range r) {
    if (has_next_addr(*p)) {
        return parse_simple_addr(s, p, (struct range){r.p2, r.p2});
    }

    struct range r2;
    r2.p1 = line_end(&s->text, r.p2);
    r2.p2 = line_end(&s->text, r2.p1);
    return r2;
}

// Search backward from r.p1 using / pattern (minus-addr with /)
static struct range
search_minus_forward(struct ssam *s, char **p, struct range r) {
    (*p)++;
    char *re = extract_delimited_dup(p, '/');
    struct range found;

    if (search_backward(s, re, r.p1, &found)) { free(re); return found; }
    free(re);
    return (struct range){r.p1, r.p1};
}

// Search backward from r.p1 using ? pattern (minus-addr with ?)
static struct range
search_minus_backward(struct ssam *s, char **p, struct range r) {
    (*p)++;
    char *re = extract_delimited_dup(p, '?');
    struct range found;

    if (search_forward(s, re, r.p1, &found)) { free(re); return found; }
    free(re);
    return (struct range){r.p1, r.p1};
}

static struct range
addr_minus(struct ssam *s, char **p, struct range r) {
    if (has_next_addr(*p)) {
        if (is_slash(**p)) return search_minus_forward(s, p, r);
        if (is_qmark(**p)) return search_minus_backward(s, p, r);
        return parse_simple_addr(s, p, (struct range){r.p1, r.p1});
    }

    return prev_line_pos(s, r);
}

static struct range
addr_comma(struct ssam *s, char **p, struct range r) {
    struct range r2 = {r.p2, s->text.len};

    if (has_range_end(*p)) {
        r2 = eval_addr(s, p, (struct range){r.p2, r.p2});
    }
    r.p2 = r2.p2;
    return r;
}

static struct range
addr_semi(struct ssam *s, char **p, struct range r) {
    s->dot = r;
    struct range r2 = {r.p2, s->text.len};

    if (has_range_end(*p)) {
        r2 = eval_addr(s, p, r);
    }
    r.p2 = r2.p2;
    return r;
}

// Parse compound address
// Handle leading comma: `,addr` is short for `0,addr`
// Returns 1 if handled (pp advanced and result set), 0 otherwise.
static int
try_leading_comma(char **pp, struct ssam *s, struct range *out) {
    char *p = skip_space(*pp);
    if (*p != ',') { return 0; }

    p++;
    out->p1 = 0;
    p = skip_space(p);

    if (has_range_end(p)) {
        struct range r2 = eval_addr(s, &p, *out);
        out->p2 = r2.p2;
        *pp = p;
        return 1;
    }
    out->p2 = s->text.len;
    *pp = p;
    return 1;
}

// Dispatch address operator: + - , ;
static struct range
apply_addr_op(char op, struct ssam *s, char **p, struct range r) {
    if (is_plus(op))      { return addr_plus(s, p, r); }
    if (is_minus(op))     { return addr_minus(s, p, r); }
    if (is_comma(op))     { return addr_comma(s, p, r); }
    if (is_semi(op))      { return addr_semi(s, p, r); }
    return r;
}

static struct range
eval_addr(struct ssam *s, char **pp, struct range base) {
    char *p = skip_space(*pp);
    struct range r = base;

    if (try_leading_comma(&p, s, &r)) {
        *pp = p;
        return r;
    }
    if (has_next_addr(p)) {
        r = parse_simple_addr(s, &p, base);
    }
    while (is_offset_op(*p) || is_range_separator(*p)) {
        char op = *p++;

        p = skip_space(p);
        r = apply_addr_op(op, s, &p, r);
    }

    *pp = p;
    return r;
}

// ============================================================================
// Command structures
// ============================================================================

enum cmd_type {
    CMD_PRINT,      // p
    CMD_DELETE,     // d
    CMD_CHANGE,     // c/text/
    CMD_APPEND,     // a/text/
    CMD_INSERT,     // i/text/
    CMD_SUBST,      // s/old/new/[g]
    CMD_MOVE,       // m addr
    CMD_COPY,       // t addr
    CMD_MARK,       // k
    CMD_EXTRACT,    // x/pat/cmd
    CMD_YANK,       // y/pat/cmd
    CMD_GUARD,      // g/pat/cmd
    CMD_INVERSE,    // v/pat/cmd
    CMD_GROUP,      // { cmds }
    CMD_EQUALS,     // =
    CMD_EQHASH,     // =#
    CMD_PIPEIN,     // < cmd
    CMD_PIPEOUT,    // > cmd
    CMD_PIPE,       // | cmd
    CMD_READ,       // r file
    CMD_WRITE,      // w file
    CMD_WRITEA,     // W file (append)
    CMD_EDIT,       // e file
    CMD_PRINTNAME,  // P
    CMD_PRINTFNAME, // f
    CMD_SHELLRUN,   // ! cmd
    CMD_CHDIR,      // cd
    CMD_UNDO,       // u
};

struct cmd {
    enum cmd_type type;
    char *addr_str;         // unparsed address (evaluated at runtime)
    char *pattern;          // regex pattern
    char *text;             // replacement text or shell command
    int num;                // s number or g flag
    struct cmd *sub;        // subcommand or first in group
    struct cmd *next;       // next command in group
};

static struct cmd *
cmd_new(enum cmd_type type) {
    struct cmd *c = calloc(1, sizeof(*c));
    if (!c) { nomem(); }
    c->type = type;
    return c;
}

static void
cmd_free(struct cmd *c) {
    if (!c) { return; }
    free(c->addr_str);
    free(c->pattern);
    free(c->text);
    cmd_free(c->sub);
    cmd_free(c->next);
    free(c);
}

static void
trim_addr_operators(char *s) {
    if (!s || !*s) { return; }
    char *end = s + strlen(s);
    while (end > s && is_range_separator(end[-1]))
        *--end = '\0';
}

// ============================================================================
// Command constructors
// ============================================================================

static struct cmd *
new_text_cmd(enum cmd_type t, char **pp) {
    struct cmd *c = cmd_new(t);
    c->text = extract_text_dup(pp);
    return c;
}

static struct cmd *
new_shell_cmd(enum cmd_type t, char **pp) {
    struct cmd *c = cmd_new(t);
    c->text = extract_shell_dup(pp);
    return c;
}

static struct cmd *
new_addr_cmd(enum cmd_type t, char **pp) {
    struct cmd *c = cmd_new(t);
    c->text = collect_addr_string(pp);
    trim_addr_operators(c->text);
    return c;
}

static const char *
file_display_name(struct ssam *s) {
    return s->curfile ? s->curfile : "(stdin)";
}

// ============================================================================
// Command parsing
// ============================================================================

static struct cmd *parse_cmd(char **pp);

static struct cmd *
parse_pattern_cmd(enum cmd_type type, char **p, int need_cmd) {
    struct cmd *c = cmd_new(type);

    *p = skip_space(*p);
    if (**p && !isalnum(**p) && **p != '\n') {
        char delim = **p;
        (*p)++;
        c->pattern = extract_delimited_dup(p, delim);
    }
    *p = skip_space(*p);
    c->sub = parse_cmd(p);
    if (!c->sub && need_cmd) {
        c->sub = cmd_new(CMD_PRINT);
    }
    return c;
}

// Parse and link one subcommand from the group body.
// Returns 1 if a command was parsed, 0 on end of group.
static int
parse_group_member(char **pp, struct cmd ***tail) {
    char *p = skip_space(*pp);
    p = skip_separators(p);
    if (*p == '}') { *pp = p; return 0; }

    struct cmd *sub = parse_cmd(&p);
    if (sub) {
        **tail = sub;
        *tail = &sub->next;
    }
    *pp = p;
    return 1;
}

static struct cmd *
ssam_parse_group(char **pp) {
    char *p = skip_space(*pp);
    if (*p != '{') { return NULL; }
    p++;

    struct cmd *c = cmd_new(CMD_GROUP);
    struct cmd **tail = &c->sub;

    while (*p && *p != '}') {
        if (!parse_group_member(&p, &tail)) { break; }
    }
    if (*p == '}') { p++; }

    *pp = p;
    return c;
}

// Extract delimiter, pattern, replacement, and optional trailing 'g'.
static void
extract_subst_args(char **pp, struct cmd *c) {
    char *p = *pp;

    if (*p && !isalnum(*p)) {
        char delim = *p++;
        c->pattern = extract_delimited_dup(&p, delim);
        c->text = extract_delimited_dup(&p, delim);
        if (*p == 'g') {
            c->num = -1;
            p++;
        }
    }
    *pp = p;
}

// Parse s/re/t/[g] — substitute command arguments
static void
parse_subst_cmd(char **pp, struct cmd *c) {
    char *p = *pp;

    // Check for sg (global prefix) or sN (Nth match prefix)
    if (*p == 'g') {
        c->num = -1;
        p++;
    } else if (isdigit(*p)) {
        c->num = parse_uint(&p);
    }
    extract_subst_args(&p, c);
    *pp = p;
}

// Parse optional count for undo command
static void
parse_undo_count(char **pp, struct cmd *c) {
    char *p = skip_space(*pp);

    c->num = 1;
    if (isdigit(*p)) {
        c->num = parse_uint(&p);
    }
    *pp = p;
}

// Consume comment to end of line or semicolon. Returns 1 if consumed.
static int
try_skip_comment(char **pp) {
    char *p = *pp;
    if (!is_comment_start(*p, p[1])) { return 0; }
    while (*p && *p != '\n' && *p != ';') { p++; }
    *pp = p;
    return 1;
}

// Try to parse `cd` keyword at *pp. Returns cmd or NULL.
static struct cmd *
try_parse_cd(char **pp) {
    char *p = *pp;
    if (!is_cd_cmd(p)) { return NULL; }

    p += 2;
    p = skip_space(p);
    struct cmd *c = cmd_new(CMD_CHDIR);
    c->text = read_cd_dir(p);
    *pp = p + (c->text ? strlen(c->text) : 0);
    return c;
}

// True if character ends a command (end of input, group end, newline, semicolon)
static int
is_cmd_end(char c) {
    return c == '\0' || c == '}' || c == '\n' || c == ';';
}

// If address is present and next token is end of command,
// return implicit print cmd. Otherwise return NULL.
static struct cmd *
try_implicit_print(char **pp, char *addr_str) {
    if (!addr_str) { return NULL; }

    char *p = skip_space(*pp);
    if (is_cmd_end(*p)) {
        struct cmd *c = cmd_new(CMD_PRINT);
        c->addr_str = addr_str;
        *pp = p;
        return c;
    }
    return NULL;
}

static struct cmd *
parse_cmd(char **pp) {
    char *p = skip_space(*pp);
    p = skip_separators(p);
    if (!*p || *p == '}') { *pp = p; return NULL; }

    if (try_skip_comment(&p)) { *pp = p; return NULL; }

    struct cmd *c = try_parse_cd(&p);
    if (c) { *pp = p; return c; }

    char *addr_str = collect_addr_string(&p);

    p = skip_space(p);
    c = try_implicit_print(&p, addr_str);
    if (c) { *pp = p; return c; }
    if (is_cmd_end(*p)) {
        *pp = p;
        free(addr_str);
        return NULL;
    }

    char cmd_char = *p++;
    c = NULL;

    switch (cmd_char) {
    case 'p':
        c = cmd_new(CMD_PRINT);
        break;

    case 'd':
        c = cmd_new(CMD_DELETE);
        break;

    case 'k':
        c = cmd_new(CMD_MARK);
        c->num = DEFAULT_MARK;  // unnamed by default; explicit overrides
        if (is_valid_mark_char(*p)) {
            c->num = (unsigned char)*p++;
        }
        break;

    case '=':
        if (is_hash(*p)) {
            p++;
            c = cmd_new(CMD_EQHASH);
        } else {
            c = cmd_new(CMD_EQUALS);
        }
        break;

    case 'a':
        c = new_text_cmd(CMD_APPEND, &p);
        break;

    case 'i':
        c = new_text_cmd(CMD_INSERT, &p);
        break;

    case 'c':
        c = new_text_cmd(CMD_CHANGE, &p);
        break;

    case 's':
        c = cmd_new(CMD_SUBST);
        parse_subst_cmd(&p, c);
        break;

    case 'm':
        c = new_addr_cmd(CMD_MOVE, &p);
        break;

    case 't':
        c = new_addr_cmd(CMD_COPY, &p);
        break;

    case 'x':
        c = parse_pattern_cmd(CMD_EXTRACT, &p, 1);
        break;

    case 'y':
        c = parse_pattern_cmd(CMD_YANK, &p, 0);
        break;

    case 'g':
        c = parse_pattern_cmd(CMD_GUARD, &p, 0);
        break;

    case 'v':
        c = parse_pattern_cmd(CMD_INVERSE, &p, 0);
        break;

    case '{':
        p--;
        c = ssam_parse_group(&p);
        break;

    case '<':
        c = new_shell_cmd(CMD_PIPEIN, &p);
        break;

    case '>':
        c = new_shell_cmd(CMD_PIPEOUT, &p);
        break;

    case '|':
        c = new_shell_cmd(CMD_PIPE, &p);
        break;

    case 'r':
        c = new_shell_cmd(CMD_READ, &p);
        break;

    case 'w':
        c = new_shell_cmd(CMD_WRITE, &p);
        break;

    case 'W':
        c = new_shell_cmd(CMD_WRITEA, &p);
        break;

    case 'P':
        c = cmd_new(CMD_PRINTNAME);
        break;

    case 'f':
        c = cmd_new(CMD_PRINTFNAME);
        break;

    case '!':
        c = new_shell_cmd(CMD_SHELLRUN, &p);
        break;

    case 'e':
        c = new_shell_cmd(CMD_EDIT, &p);
        break;

    case 'u':
        c = cmd_new(CMD_UNDO);
        parse_undo_count(&p, c);
        break;

    default:
        free(addr_str);
        *pp = p;
        return NULL;
    }

    if (c) {
        c->addr_str = addr_str;
    } else {
        free(addr_str);
    }

    *pp = p;
    return c;
}

// ============================================================================
// Command execution
// ============================================================================

static void
exec_print(struct ssam *s) {
    fwrite(s->text.data + s->dot.p1, 1, s->dot.p2 - s->dot.p1, stdout);
}

static void
exec_delete(struct ssam *s) {
    buf_replace(&s->text, s->dot.p1, s->dot.p2, "", 0);
    s->dot.p2 = s->dot.p1;
}

static void
exec_change(struct ssam *s, const char *text) {
    size_t len = text_len(text);

    buf_replace(&s->text, s->dot.p1, s->dot.p2, str_or_empty(text), len);
    s->dot.p2 = s->dot.p1 + len;
}

static void
exec_append(struct ssam *s, const char *text) {
    size_t len = text_len(text);

    buf_replace(&s->text, s->dot.p2, s->dot.p2, str_or_empty(text), len);
    s->dot.p1 = s->dot.p2;
    s->dot.p2 = s->dot.p1 + len;
}

static void
exec_insert(struct ssam *s, const char *text) {
    size_t len = text_len(text);

    buf_replace(&s->text, s->dot.p1, s->dot.p1, str_or_empty(text), len);
    s->dot.p2 = s->dot.p1 + len;
}

// True if range ends exactly at a newline (not past it)
static int
ends_at_newline(struct ssam *s, int line1, int line2) {
    return line2 > line1 && s->dot.p2 > 0 &&
           s->text.data[s->dot.p2 - 1] == '\n';
}

static void
exec_eqhash(struct ssam *s) {
    printf("#%zu,#%zu\n", s->dot.p1, s->dot.p2);
}

static void
exec_equals(struct ssam *s) {
    int line1 = line_number(&s->text, s->dot.p1);
    int line2 = line_number(&s->text, s->dot.p2);

    if (ends_at_newline(s, line1, line2)) { line2--; }
    printf("%d,%d; #%zu,#%zu\n", line1, line2, s->dot.p1, s->dot.p2);
}

// Append escaped character from replacement string
static void
append_escape_repl(struct buf *b, char c) {
    switch (c) {
    case '&':  buf_append_char(b, '&'); break;
    case '\\': buf_append_char(b, '\\'); break;
    case 'n':  buf_append_char(b, '\n'); break;
    case 't':  buf_append_char(b, '\t'); break;
    case 'r':  buf_append_char(b, '\r'); break;
    default:   buf_append_char(b, '\\'); buf_append_char(b, c); break;
    }
}

// Append group reference (\\1-\\9) or & match from replacement
static void
append_group_ref(struct buf *b, char digit, struct rematch *m, int nm) {
    int n = digit - '0';

    if (n < nm && m[n].start) {
        buf_append(b, m[n].start, m[n].end - m[n].start);
    }
}

static char *
build_replacement(const char *repl, struct rematch *m, int nm) {
    struct buf b;
    buf_init(&b);

    for (const char *p = repl; *p; p++) {
        if (is_group_ref(p)) {
            p++;
            append_group_ref(&b, *p, m, nm);
        } else if (*p == '\\') {
            p++;
            append_escape_repl(&b, *p);
        } else if (*p == '&') {
            append_match_ref(&b, m);
        } else {
            buf_append_char(&b, *p);
        }
    }
    buf_append_char(&b, '\0');
    return b.data;
}


// Record one collected substitution item: grow array, fill fields.
// Returns 1 on success, 0 on allocation failure.
static int
record_subst_item(struct subst_item **items, int *nitems, int *cap,
                   size_t pos, size_t match_len, const char *repl,
                   struct rematch *m) {
    if (*nitems >= *cap && !subst_item_grow(items, cap)) { return 0; }

    struct subst_item *it = &(*items)[*nitems];
    it->pos = pos;
    it->match_len = match_len;
    it->replacement = build_replacement(repl ? repl : "", m, MAX_CAPTURES);
    it->repl_len = strlen(it->replacement);
    (*nitems)++;
    return 1;
}

// Convert relative regex match to absolute position range.
static struct range
match_to_range(const struct rematch *m, const char *text, size_t offset) {
    struct range r;

    r.p1 = offset + (m[0].start - (text + offset));
    r.p2 = offset + (m[0].end - (text + offset));
    return r;
}

// Advance offset past a match, handling zero-length matches correctly.
static size_t
next_offset(size_t offset, size_t ms, size_t me) {
    return (ms == me) ? offset + 1 : me;
}

// Collect substitution matches with replacement tracking.
// Handles global (num=-1) and Nth-match (num>0) modes.
static struct subst_item *
collect_subst_items(struct regex *re, const char *copy, size_t len,
                     const char *repl, int num, int *out_nitems) {
    size_t offset = 0;
    int count = 0, nitems = 0, cap = 0;
    int global = is_global_subst(num), target = nth_target(num);
    struct subst_item *items = NULL;
    struct rematch m[MAX_CAPTURES] = {0};

    while (re_search(re, copy + offset, m, MAX_CAPTURES)) {
        count++;
        struct range mr = match_to_range(m, copy, offset);

        if (global || count == target) {
            if (!record_subst_item(&items, &nitems, &cap, mr.p1, mr.p2 - mr.p1, repl, m))
                { goto fail; }
            if (!global) { break; }
        }

        offset = next_offset(offset, mr.p1, mr.p2);
        if (offset >= len) { break; }
    }

    *out_nitems = nitems;
    return items;

fail:
    subst_item_free_all(items, nitems);
    *out_nitems = 0;
    return NULL;
}

// Return pattern to use: given non-empty pattern, save and return it;
// given empty or NULL, return previously saved pattern.
// Returned pointer is owned by s->last_pattern — caller must not free it.
static const char *
use_pattern(struct ssam *s, const char *pattern) {
    if (pattern && *pattern) {
        free(s->last_pattern);
        s->last_pattern = strdup(pattern);
        return pattern;
    }

    return s->last_pattern;
}

static struct regex *
get_re_cached(struct ssam *s, const char *pattern) {
    if (!pattern || !*pattern) { return NULL; }

    if (s->cached_re && s->cached_re_pat &&
        strcmp(s->cached_re_pat, pattern) == 0) {
        return s->cached_re;
    }

    free(s->cached_re_pat);
    if (s->cached_re) { re_free(s->cached_re); }

    s->cached_re_pat = strdup(pattern);
    if (!s->cached_re_pat) { s->cached_re = NULL; return NULL; }
    s->cached_re = re_compile(pattern, NULL);
    return s->cached_re;
}

// Apply reverse substitutions end-to-start to avoid position shifts.
static void
apply_reverse_replacements(struct ssam *s,
    struct subst_item *items, int nitems, size_t base, size_t end) {
    ssize_t total_delta = 0;

    for (int i = nitems - 1; i >= 0; i--) {
        buf_replace(&s->text, base + items[i].pos,
                    base + items[i].pos + items[i].match_len,
                    items[i].replacement, items[i].repl_len);
        total_delta += (ssize_t)items[i].repl_len - (ssize_t)items[i].match_len;
    }
    s->dot.p2 = end + total_delta;
}

static void
exec_subst(struct ssam *s, const char *pattern, const char *repl, int num) {
    pattern = use_pattern(s, pattern);
    if (!pattern) { return; }

    struct regex *re = get_re_cached(s, pattern);
    if (re_report_error(re)) { return; }

    size_t base = s->dot.p1;
    size_t end = s->dot.p2;
    char *copy = buf_dup(&s->text, base, end);
    if (!copy) { return; }

    int nitems = 0;
    struct subst_item *items = collect_subst_items(re, copy, end - base, repl, num, &nitems);
    free(copy);
    if (!items && nitems == 0) { return; }

    apply_reverse_replacements(s, items, nitems, base, end);
    subst_item_free_all(items, nitems);
}

static void
move_delete_then_insert(struct buf *b, size_t p1, size_t p2,
                         size_t pos, const char *data, size_t len) {
    buf_replace(b, p1, p2, "", 0);
    buf_replace(b, pos, pos, data, len);
}

static void
move_insert_then_delete(struct buf *b, size_t p1, size_t p2,
                         size_t pos, const char *data, size_t len) {
    buf_replace(b, pos, pos, data, len);
    buf_replace(b, p1 + len, p2 + len, "", 0);
}

// Evaluate destination address string relative to current dot
static struct range
eval_dest_addr(struct ssam *s, const char *dest_str) {
    char *p = (char *)dest_str;
    return eval_addr(s, &p, s->dot);
}

static void
exec_move(struct ssam *s, const char *dest_str) {
    if (!dest_str) { return; }

    size_t src_p1 = s->dot.p1, src_p2 = s->dot.p2, len = src_p2 - src_p1;
    char *text = buf_dup(&s->text, src_p1, src_p2);
    if (!text) { return; }

    struct range dest = eval_dest_addr(s, dest_str);
    size_t pos = dest.p2;

    if (pos > src_p2) {
        move_delete_then_insert(&s->text, src_p1, src_p2, pos - len, text, len);
        pos -= len;
    } else if (pos <= src_p1) {
        move_insert_then_delete(&s->text, src_p1, src_p2, pos, text, len);
    } else {
        free(text);
        return;
    }

    s->dot.p1 = pos;
    s->dot.p2 = pos + len;
    free(text);
}

static void
exec_copy(struct ssam *s, const char *dest_str) {
    if (!dest_str) { return; }

    char *text = buf_dup(&s->text, s->dot.p1, s->dot.p2);
    if (!text) { return; }
    size_t len = s->dot.p2 - s->dot.p1;

    struct range dest = eval_dest_addr(s, dest_str);

    buf_replace(&s->text, dest.p2, dest.p2, text, len);
    s->dot.p1 = dest.p2;
    s->dot.p2 = dest.p2 + len;

    free(text);
}

// Reallocate match array, doubling capacity. Returns 1 on success.
static int
grow_match_array(void **arr, int *cap, size_t elem_size) {
    int newcap = *cap ? *cap * 2 : 256;
    void *new = realloc(*arr, newcap * elem_size);

    if (!new) { free(*arr); *arr = NULL; return 0; }
    *arr = new;
    *cap = newcap;
    return 1;
}

// Record one match position into array, growing if needed.
// Returns 0 on allocation failure.
static int
record_match_position(size_t **pos, int *nm, int *cap,
                       const char *text, const char *start, const char *end) {
    if (*nm >= *cap) {
        if (!grow_match_array((void **)pos, cap, 2 * sizeof(size_t)))
            { return 0; }
    }

    (*pos)[*nm * 2] = start - text;
    (*pos)[*nm * 2 + 1] = end - text;
    (*nm)++;
    return 1;
}

// Collect all match positions for re in text. Returns count, allocates out.
static int
collect_all_matches(struct regex *re, const char *text,
                    size_t **out_pos, int *out_nm) {
    *out_pos = NULL;
    int nm = 0, cap = 0;
    struct rematch m[MAX_CAPTURES] = {0};

    if (!re_search(re, text, m, MAX_CAPTURES)) { *out_nm = 0; return 0; }
    do {
        if (!record_match_position(out_pos, &nm, &cap, text, m[0].start, m[0].end))
            { *out_nm = nm; return 0; }
    } while (re_next(re, m, MAX_CAPTURES));

    *out_nm = nm;
    return nm;
}

static void
apply_with_delta(struct ssam *s, size_t base, ssize_t *delta,
                  size_t p1, size_t p2, struct cmd *sub) {
    size_t old_len = s->text.len;

    s->dot.p1 = base + p1 + *delta;
    s->dot.p2 = base + p2 + *delta;
    run_subcmd(s, sub);
    *delta += (ssize_t)s->text.len - (ssize_t)old_len;
}

// Resolve pattern: non-empty pattern as-is, default to ".*\n"
static const char *
default_extract_pattern(const char *pattern) {
    return (pattern && *pattern) ? pattern : ".*\n";
}

// Append &-match reference (whole match) if available
static void
append_match_ref(struct buf *b, struct rematch *m) {
    if (m[0].start) {
        buf_append(b, m[0].start, m[0].end - m[0].start);
    }
}

// Snapshot dot range, collect all match positions. Returns copied text.
// Caller must free *out_copy and *out_pos.
static char *
snapshot_and_match(struct ssam *s, struct regex *re,
                    size_t *out_base, size_t **out_pos, int *out_nm) {
    size_t end = s->dot.p2;
    *out_base = s->dot.p1;

    char *copy = buf_dup(&s->text, *out_base, end);
    if (!copy) { *out_nm = 0; return NULL; }
    collect_all_matches(re, copy, out_pos, out_nm);
    return copy;
}

static void
exec_extract(struct ssam *s, const char *pattern, struct cmd *sub) {
    const char *pat = default_extract_pattern(pattern);

    struct regex *re = get_re_cached(s, pat);
    if (re_report_error(re)) { return; }

    size_t base;
    size_t *pos = NULL;
    int nm = 0;
    char *copy = snapshot_and_match(s, re, &base, &pos, &nm);
    free(copy);
    if (nm == 0) { return; }

    ssize_t delta = 0;
    for (int i = 0; i < nm; i++) {
        apply_with_delta(s, base, &delta, pos[i * 2], pos[i * 2 + 1], sub);
    }
    free(pos);
}

// Build interleaved bounds array: [0, m1.p1, m1.p2, ..., mN.p1, mN.p2, len]
static size_t *
interleave_match_bounds(size_t *matches, int nm, size_t len, int *out_nb) {
    int nb = 2 * nm + 2;
    size_t *bounds = malloc(nb * sizeof(*bounds));
    if (!bounds) { return NULL; }

    bounds[0] = 0;
    for (int i = 0; i < nm; i++) {
        bounds[i * 2 + 1] = matches[i * 2];
        bounds[i * 2 + 2] = matches[i * 2 + 1];
    }
    bounds[nb - 1] = len;
    *out_nb = nb;
    return bounds;
}

// Collect yank boundaries: returns array [0, m1.p1, m1.p2, ..., len]
// out_nb receives element count. Returns NULL on failure.
static size_t *
collect_yank_boundaries(struct regex *re, const char *copy, size_t len,
                         int *out_nb) {
    size_t *matches = NULL;
    int nm = 0;

    collect_all_matches(re, copy, &matches, &nm);

    size_t *bounds = interleave_match_bounds(matches, nm, len, out_nb);
    free(matches);
    return bounds;
}

// Apply subcommand to each non-empty region (gap between matches).
// Returns count of regions processed.
static int
apply_yank_regions(struct ssam *s, size_t base, size_t *bounds, int nb,
                    struct cmd *sub, ssize_t *delta) {
    int nregions = 0;

    for (int i = 0; i < nb - 1; i += 2) {
        size_t p1 = bounds[i], p2 = bounds[i + 1];

        if (p1 < p2) {
            apply_with_delta(s, base, delta, p1, p2, sub);
            nregions++;
        }
    }
    return nregions;
}

// Snapshot dot into copy, collect yank boundaries for pattern.
// Returns bounds array; caller must free. Sets *out_base, *out_nb.
// Returns NULL on failure.
static size_t *
yank_copy_bounds(struct ssam *s, struct regex *re,
                   size_t *out_base, int *out_nb) {
    size_t end = s->dot.p2;
    *out_base = s->dot.p1;

    char *copy = buf_dup(&s->text, *out_base, end);
    if (!copy) { return NULL; }

    size_t *bounds = collect_yank_boundaries(re, copy, end - *out_base, out_nb);
    free(copy);
    return bounds;
}

static void
exec_yank(struct ssam *s, const char *pattern, struct cmd *sub) {
    if (!pattern || !*pattern) {
        fprintf(stderr, "ssam: y requires pattern\n");
        return;
    }

    struct regex *re = get_re_cached(s, pattern);
    if (re_report_error(re)) { return; }

    size_t base;
    int nb = 0;
    size_t *bounds = yank_copy_bounds(s, re, &base, &nb);
    if (!bounds) { return; }

    struct range saved = s->dot;
    ssize_t delta = 0;

    int nregions = apply_yank_regions(s, base, bounds, nb, sub, &delta);
    free(bounds);
    if (nregions == 0) { s->dot = saved; }
}

// True if pattern has a match in current dot (no allocation — uses slice search)
static int
matched_in_dot(struct ssam *s, const char *pattern) {
    pattern = use_pattern(s, pattern);
    if (!pattern) { return 0; }

    struct regex *re = get_re_cached(s, pattern);
    if (re_report_error(re)) { return 0; }

    struct range r;
    return search_in_slice(re, s->text.data, s->dot.p2 - s->dot.p1,
                          s->dot.p1, &r);
}

// If pattern matches dot, run subcommand
static void
exec_guard(struct ssam *s, const char *pattern, struct cmd *sub) {
    if (matched_in_dot(s, pattern)) { run_subcmd(s, sub); }
}

// If pattern does NOT match dot, run subcommand
static void
exec_invguard(struct ssam *s, const char *pattern, struct cmd *sub) {
    if (!matched_in_dot(s, pattern)) { run_subcmd(s, sub); }
}

static void
save_last_cmd(struct ssam *s, const char *cmd) {
    free(s->lastshell);
    s->lastshell = strdup(cmd);
}

static struct buf
read_to_buf(const char *path) {
    struct buf b;
    buf_init(&b);
    FILE *f = fopen(path, "r");

    if (!f) { fopen_error(path); return b; }
    read_into_buf(&b, f);
    fclose(f);
    return b;
}

// Read file into script buffer (for -f flag)
static void
read_script_file(struct buf *script, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "ssam: %s: ", path);
        perror(NULL);
        exit(1);
    }

    read_into_buf(script, f);
    fclose(f);
}

// Open command for reading, return stdout as buffer
static struct buf
read_popen(const char *cmd) {
    struct buf b;
    buf_init(&b);
    FILE *p = popen(cmd, "r");

    if (!p) {
        fprintf(stderr, "ssam: %s: ", cmd);
        perror(NULL);
        return b;
    }
    read_into_buf(&b, p);
    pclose(p);
    return b;
}

static void
exec_shell_in(struct ssam *s, const char *cmd) {
    if (is_empty_str(cmd)) { return; }

    save_last_cmd(s, cmd);
    struct buf b = read_popen(cmd);
    if (!b.data) { return; }
    dot_from_buf(s, &b);
}

static void
exec_shell_out(struct ssam *s, const char *cmd) {
    if (is_empty_str(cmd)) { return; }
    save_last_cmd(s, cmd);

    FILE *p = popen(cmd, "w");
    if (!p) {
        fprintf(stderr, "ssam: %s: ", cmd);
        perror(NULL);
        return;
    }

    fwrite(s->text.data + s->dot.p1, 1, s->dot.p2 - s->dot.p1, p);
    pclose(p);
}

static int
dot_to_temp(const char *data, size_t len, char *tmpout) {
    int fd = mkstemp(tmpout);
    if (fd < 0) { perror("mkstemp"); return 0; }

    if (!write_all(fd, data, len)) { close(fd); unlink(tmpout); return 0; }
    close(fd);
    return 1;
}

static char *
build_pipe_cmd(const char *cmd, const char *tmpin) {
    size_t len = strlen(cmd) + strlen(tmpin) + 4;
    char *fullcmd = malloc(len);
    if (!fullcmd) { return NULL; }
    sprintf(fullcmd, "%s < %s", cmd, tmpin);
    return fullcmd;
}

static void
exec_shell_pipe(struct ssam *s, const char *cmd) {
    if (is_empty_str(cmd)) { return; }

    save_last_cmd(s, cmd);
    char tmpin[] = TEMP_TEMPLATE;

    if (!dot_to_temp(s->text.data + s->dot.p1, s->dot.p2 - s->dot.p1, tmpin)) { return; }

    char *fullcmd = build_pipe_cmd(cmd, tmpin);
    if (!fullcmd) { unlink(tmpin); return; }
    struct buf b = read_popen(fullcmd);
    free(fullcmd);
    unlink(tmpin);

    if (!b.data) { return; }
    dot_from_buf(s, &b);
}

static void
exec_read(struct ssam *s, const char *path) {
    if (is_empty_str(path)) { return; }

    struct buf b = read_to_buf(path);
    if (!b.data) { return; }
    dot_from_buf(s, &b);
}

static void
exec_write_file(struct ssam *s, const char *path) {
    if (is_empty_str(path)) { return; }

    FILE *f = fopen(path, "w");
    if (!f) { fopen_error(path); return; }
    fwrite(s->text.data + s->dot.p1, 1, s->dot.p2 - s->dot.p1, f);
    fclose(f);
}

static void
exec_write_append(struct ssam *s, const char *path) {
    if (is_empty_str(path)) { return; }

    FILE *f = fopen(path, "a");
    if (!f) { fopen_error(path); return; }
    fwrite(s->text.data + s->dot.p1, 1, s->dot.p2 - s->dot.p1, f);
    fclose(f);
}

static void
exec_edit(struct ssam *s, const char *path) {
    if (is_empty_str(path)) { return; }

    buf_free(&s->text);
    buf_init(&s->text);
    read_path(&s->text, path);

    s->dot.p1 = 0;
    s->dot.p2 = s->text.len;
}

static void
exec_shell_run(struct ssam *s, const char *cmd) {
    if (is_empty_str(cmd)) { return; }

    save_last_cmd(s, cmd);
    if (system(cmd) != 0) {
        fprintf(stderr, "ssam: !: %s\n", cmd);
    }
}

// HOME directory fallback for empty cd target
static const char *
home_or_target(const char *dir) {
    return is_empty_str(dir) ? getenv("HOME") : dir;
}

static void
exec_chdir(struct ssam *s, const char *dir) {
    (void)s;

    const char *target = home_or_target(dir);
    if (!target) { return; }

    if (chdir(target) != 0) {
        fprintf(stderr, "ssam: cd %s: ", target);
        perror(NULL);
    }
}

static void
drop_oldest_undo(struct ssam *s) {
    free(s->undo[0].data);
    memmove(s->undo, s->undo + 1, (MAX_UNDO - 1) * sizeof(s->undo[0]));
    s->undolen--;
}

static void
undo_save(struct ssam *s) {
    if (is_inside_subcmd(s)) { return; }

    if (s->undolen >= MAX_UNDO) { drop_oldest_undo(s); }

    struct undo_snap *sn = &s->undo[s->undolen++];
    sn->data = buf_dup(&s->text, 0, s->text.len);
    sn->len = s->text.len;
    sn->dot = s->dot;
}

// Restore buffer from snapshot, free snapshot data
static void
restore_snapshot(struct ssam *s, struct undo_snap *sn) {
    buf_replace(&s->text, 0, s->text.len, sn->data, sn->len);
    s->dot = sn->dot;
    free(sn->data);
    sn->data = NULL;
}

static void
exec_undo(struct ssam *s, int n) {
    if (n <= 0) { n = 1; }

    for (int i = 0; i < n && s->undolen > 0; i++) {
        struct undo_snap *sn = &s->undo[--s->undolen];
        restore_snapshot(s, sn);
    }
}

// True for commands that modify the buffer
static int
is_modifying(enum cmd_type t) {
    switch (t) {
    case CMD_DELETE: case CMD_CHANGE: case CMD_APPEND:
    case CMD_INSERT: case CMD_SUBST: case CMD_MOVE:
    case CMD_COPY: case CMD_EXTRACT: case CMD_YANK:
    case CMD_PIPEIN: case CMD_PIPE: case CMD_READ:
    case CMD_EDIT: case CMD_GUARD: case CMD_INVERSE:
    case CMD_GROUP:
        return 1;
    default: return 0;
    }
}

static void
save_undo_if_needed(struct ssam *s, struct cmd *c) {
    if (is_top_level(s) && is_modifying(c->type)) {
        undo_save(s);
    }
}

static void
execute(struct ssam *s, struct cmd *c) {
    while (c) {
        save_undo_if_needed(s, c);

        if (c->addr_str) {
            char *p = c->addr_str;
            s->dot = eval_addr(s, &p, s->dot);
        }

        switch (c->type) {
        case CMD_PRINT:  exec_print(s); break;
        case CMD_DELETE: exec_delete(s); break;
        case CMD_CHANGE: exec_change(s, c->text); break;
        case CMD_APPEND: exec_append(s, c->text); break;
        case CMD_INSERT: exec_insert(s, c->text); break;
        case CMD_SUBST:  exec_subst(s, c->pattern, c->text, c->num); break;
        case CMD_MOVE:   exec_move(s, c->text); break;
        case CMD_COPY:   exec_copy(s, c->text); break;
        case CMD_MARK:   s->mark[c->num] = s->dot; break;
        case CMD_EXTRACT: exec_extract(s, c->pattern, c->sub); break;
        case CMD_YANK:   exec_yank(s, c->pattern, c->sub); break;
        case CMD_GUARD:  exec_guard(s, c->pattern, c->sub); break;
        case CMD_INVERSE: exec_invguard(s, c->pattern, c->sub); break;
        case CMD_GROUP:  execute(s, c->sub); break;
        case CMD_EQUALS: exec_equals(s); break;
        case CMD_EQHASH: exec_eqhash(s); break;
        case CMD_PIPEIN: exec_shell_in(s, c->text); break;
        case CMD_PIPEOUT: exec_shell_out(s, c->text); break;
        case CMD_PIPE:   exec_shell_pipe(s, c->text); break;
        case CMD_READ:   exec_read(s, c->text); break;
        case CMD_WRITE:  exec_write_file(s, c->text); break;
        case CMD_WRITEA: exec_write_append(s, c->text); break;
        case CMD_PRINTNAME:
        case CMD_PRINTFNAME: printf("%s\n", file_display_name(s)); break;
        case CMD_EDIT:   exec_edit(s, c->text); break;
        case CMD_SHELLRUN: exec_shell_run(s, c->text); break;
        case CMD_CHDIR:  exec_chdir(s, c->text); break;
        case CMD_UNDO:   exec_undo(s, c->num); break;
        }
        c = c->next;
    }
}

// ============================================================================
// I/O helpers
// ============================================================================

static void
read_into_buf(struct buf *b, FILE *f) {
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        buf_append(b, chunk, n);
    }
}

// Replace dot with buf content and free buf
static void
dot_from_buf(struct ssam *s, struct buf *b) {
    buf_replace(&s->text, s->dot.p1, s->dot.p2, b->data, b->len);
    s->dot.p2 = s->dot.p1 + b->len;
    buf_free(b);
}

static int
fopen_error(const char *path) {
    fprintf(stderr, "ssam: %s: ", path);
    perror(NULL);
    return 0;
}

// Iterate lines from before/after, counting and optionally printing diffs.
// When print=1, writes color diff to out.  Returns change count.
static int
line_diff(FILE *out, const char *before, const char *after,
          const char *file, int print) {
    const char *bp = before ? before : "";
    const char *ap = after ? after : "";
    int line = 1, changes = 0;

    while (*bp || *ap) {
        const char *bend = strchr(bp, '\n');
        const char *aend = strchr(ap, '\n');
        size_t blen = bend ? (size_t)(bend - bp) : strlen(bp);
        size_t alen = aend ? (size_t)(aend - ap) : strlen(ap);
        int same = (blen == alen) && memcmp(bp, ap, blen) == 0;

        if (!same) {
            if (print) {
                if (changes == 0 && file)
                    fprintf(out, "%s---%s %s\n", DIM, RST, file);
                fprintf(out, "  %s%d%s %s-%.*s%s\n", DIM, line, RST, RED, (int)blen, bp, RST);
                fprintf(out, "  %s%d%s %s+%.*s%s\n", DIM, line, RST, GRN, (int)alen, ap, RST);
            }
            changes++;
        }

        bp = bend ? bend + 1 : bp + blen;
        ap = aend ? aend + 1 : ap + alen;
        line++;
    }

    // Trailing newline mismatch
    if (changes == 0) {
        size_t bl = before ? strlen(before) : 0;
        size_t al = after ? strlen(after) : 0;
        int bnl = bl > 0 && before[bl - 1] == '\n';
        int anl = al > 0 && after[al - 1] == '\n';

        if (bnl != anl) {
            if (print) {
                if (file)
                    fprintf(out, "%s---%s %s\n", DIM, RST, file);
                fprintf(out, "  %s%d%s %s-%s%s\n", DIM, line, RST, RED, "", RST);
                fprintf(out, "  %s%d%s %s+%s%s\n", DIM, line, RST, GRN, "", RST);
            }
            changes++;
        }
    }

    return changes;
}

// Count changed lines between before/after. Handles NULL or empty for either.
static int
count_changes(const char *before, const char *after) {
    return line_diff(NULL, before, after, NULL, 0);
}

// Print diff with ANSI color.  --- file header and line numbers in dim,
// - lines in red, + lines in green.  file is optional.
static int
diff_print(FILE *out, const char *before, const char *after, const char *file) {
    return line_diff(out, before, after, file, 1);
}

static void
read_path(struct buf *b, const char *path) {
    FILE *f = fopen(path, "r");

    if (!f) { fopen_error(path); return; }
    read_into_buf(b, f);
    fclose(f);
}

// ============================================================================
// Main
// ============================================================================

static void
usage(int code) {
    FILE *out = code ? stderr : stdout;
    fprintf(out, "usage: ssam [-n] [-d] [-s] [-i[.bak]] [-e cmd] [-f file] [file ...]\n");
    if (code == 0) {
        fprintf(out, "\n"
            "Structural regular expression stream editor (Plan 9 sam subset).\n"
            "\n"
            "Options:\n"
            "  -e cmd    Execute cmd\n"
            "  -f file   Read commands from file\n"
            "  -n        Suppress final implicit print\n"
            "  -d        Show diff of changes on stderr\n"
            "  -s        Show change summary on stderr\n"
            "  -i[.bak]  Edit files in-place (optional backup suffix)\n"
            "  -h        Show this help\n"
            "\n"
            "Addresses:\n"
            "  #n        Character position n\n"
            "  n         Line n\n"
            "  .         Current selection (dot)\n"
            "  $         End of file\n"
            "  /re/      Forward search\n"
            "  ?re?      Backward search\n"
            "  '         Mark (set by k)\n"
            "  a1,a2     Range from a1 to a2\n"
            "  a1;a2     Range, a2 evaluated with dot=a1\n"
            "  a1+a2     a2 after a1 (default +1 line)\n"
            "  a1-a2     a2 before a1 (default -1 line)\n"
            "  ,         Whole file (0,$)\n"
            "\n"
            "Commands:\n"
            "  p         Print dot\n"
            "  d         Delete dot\n"
            "  a/text/   Append after dot\n"
            "  i/text/   Insert before dot\n"
            "  c/text/   Change dot to text\n"
            "  s/re/t/   Substitute (s/re/t/g for all)\n"
            "  m addr    Move dot to after addr\n"
            "  t addr    Copy dot to after addr\n"
            "  k[x]      Set mark x (default ')\n"
            "  'x        Recall mark x\n"
            "  =         Print addresses\n"
            "  P         Print file name\n"
            "  f         Print file name\n"
            "  e file    Replace buffer with file\n"
            "  r file    Read file, insert after dot\n"
            "  w file    Write dot to file\n"
            "  W file    Append dot to file\n"
            "  ! cmd     Run shell command\n"
            "  < cmd     Replace dot with cmd output\n"
            "  > cmd     Send dot to cmd\n"
            "  | cmd     Pipe dot through cmd\n"
            "  cd [dir]  Change directory\n"
            "\n"
            "Structural:\n"
            "  x/re/cmd  For each match, run cmd (default re: .*\\n)\n"
            "  y/re/cmd  For text between matches, run cmd\n"
            "  g/re/cmd  If dot matches, run cmd\n"
            "  v/re/cmd  If dot doesn't match, run cmd\n"
            "  { c1; c2 } Group commands\n"
            "\n"
            "Examples:\n"
            "  ssam 's/foo/bar/g' file             Replace all\n"
            "  ssam -n ',x g/error/ p'             Print error lines\n"
            "  ssam ',x/  +/ c/ /'                 Collapse spaces\n"
            "  ssam '1,10d' file                   Delete lines 1-10\n"
            "  ssam '/start/,/end/p'               Print range\n"
        );
    }
    exit(code);
}

// True if arg is -i (in-place) flag with optional suffix
static int
is_inplace_flag(const char *arg) {
    return arg[0] == '-' && arg[1] == 'i';
}

// Extract -i[.bak] from argv before getopt runs (we own argv at this
// point and getopt hasn't run yet). Returns backup suffix or "" for bare -i.
// SIDEFF: removes the -i arg from argv/argc so getopt never sees it.
static const char *
extract_inplace_argv(int *argc, char **argv) {
    for (int i = 1; i < *argc; i++) {
        char *arg = argv[i];
        if (!is_inplace_flag(arg)) continue;
        const char *suffix = arg[2] ? arg + 2 : "";
        memmove(&argv[i], &argv[i+1], (*argc - i - 1) * sizeof(argv[0]));
        (*argc)--;
        argv[*argc] = NULL;
        return suffix;
    }
    return NULL;
}

enum out_mode { MODE_TEXT, MODE_NONE, MODE_DIFF, MODE_SUMMARY };

static void
read_script(struct buf *script, int argc, char **argv, enum out_mode *mode) {
    int opt;
    while ((opt = getopt(argc, argv, "hndse:f:")) != -1) {
        switch (opt) {
        case 'h': usage(0); break;
        case 'n': *mode = MODE_NONE; break;
        case 'd': *mode = MODE_DIFF; break;
        case 's': *mode = MODE_SUMMARY; break;
        case 'e':
            buf_append_str(script, optarg);
            buf_append_char(script, '\n');
            break;
        case 'f':
            read_script_file(script, optarg);
            break;
        default: usage(1);
        }
    }

    if (script->len == 0) {
        if (optind >= argc) { usage(1); }
        buf_append_str(script, argv[optind++]);
    }

    buf_append_char(script, '\0');
}

static struct cmd *
parse_commands(struct buf *script) {
    char *p = script->data;
    struct cmd *cmd = NULL;
    struct cmd **tail = &cmd;

    while (*p) {
        p = skip_space(p);
        p = skip_separators(p);

        if (*p == '}') { p++; continue; }
        if (!*p) { break; }

        struct cmd *c = parse_cmd(&p);
        if (c) { *tail = c; tail = &c->next; }
    }
    return cmd;
}

static void
init_ssam(struct ssam *s, int suppress) {
    memset(s, 0, sizeof(*s));

    buf_init(&s->text);
    s->suppress_print = suppress;
}

static void
cleanup_ssam(struct ssam *s) {
    for (int j = 0; j < s->undolen; j++) { free(s->undo[j].data); }

    buf_free(&s->text);
    free(s->lastshell);
    free(s->last_pattern);
    free(s->cached_re_pat);
    if (s->cached_re) { re_free(s->cached_re); }
}

// Create temporary file for atomic write, return fd and path.
// Caller must close(fd) and free *tmpl on success.
static int
create_temp_file(const char *path, int *out_fd, char **out_tmpl) {
    char *tmpl = malloc(strlen(path) + TMPL_SUFFIX_LEN);
    if (!tmpl) { return 0; }

    sprintf(tmpl, "%s%s", path, TMPL_SUFFIX);
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        fprintf(stderr, "ssam: %s: ", path);
        perror(NULL);
        free(tmpl);
        return 0;
    }
    *out_fd = fd;
    *out_tmpl = tmpl;
    return 1;
}

// Report error and clean up temp file
static void
cleanup_temp(int fd, char *tmpl) {
    close(fd);
    unlink(tmpl);
    free(tmpl);
}

static int
write_to_temp(const char *path, const char *data, size_t len,
               char **out_tmpl) {
    int fd;
    char *tmpl;

    if (!create_temp_file(path, &fd, &tmpl)) { return 0; }

    if (!write_all(fd, data, len)) {
        fprintf(stderr, "ssam: %s: ", path);
        perror(NULL);
        cleanup_temp(fd, tmpl);
        return 0;
    }

    close(fd);
    *out_tmpl = tmpl;
    return 1;
}

static void
atomic_replace(const char *path, char *tmpl, const char *inplace) {
    // Create backup if suffix provided (rename original before replacing)
    if (*inplace) {
        char *bak = malloc(strlen(path) + strlen(inplace) + 1);
        if (bak) {
            sprintf(bak, "%s%s", path, inplace);
            rename(path, bak);  // best-effort
            free(bak);
        }
    }

    if (rename(tmpl, path) < 0) {
        fprintf(stderr, "ssam: %s: rename: ", path);
        perror(NULL);
        unlink(tmpl);
    }
    free(tmpl);
}

// Report changes: summary line to stderr or diff to stdout
static void
report_edits(const char *before, const char *after,
              enum out_mode mode, const char *path) {
    if (mode == MODE_SUMMARY) {
        int n = count_changes(before, after);
        if (n > 0)
            fprintf(stderr, "%d line(s) changed\n", n);
    } else if (mode == MODE_DIFF) {
        diff_print(stdout, before, after, path);
    }
}

// Edit each file in-place, using cmd for transformation
static void
edit_file(const char *path, struct cmd *cmd, const char *inplace, enum out_mode mode) {
    struct ssam s;
    init_ssam(&s, 1);

    read_path(&s.text, path);
    s.curfile = path;
    s.dot.p1 = 0;
    s.dot.p2 = s.text.len;
    s.mark[DEFAULT_MARK] = s.dot;

    char *before = buf_dup(&s.text, 0, s.text.len);

    execute(&s, cmd);

    char *tmpl = NULL;
    if (!write_to_temp(path, s.text.data, s.text.len, &tmpl)) {
        cleanup_ssam(&s);
        free(before);
        return;
    }

    atomic_replace(path, tmpl, inplace);
    report_edits(before, s.text.data, mode, path);
    free(before);
    cleanup_ssam(&s);
}

static void
run_pipeline(struct cmd *cmd, int argc, char **argv, enum out_mode mode) {
    struct ssam s;
    init_ssam(&s, mode != MODE_TEXT);

    if (optind >= argc) {
        read_into_buf(&s.text, stdin);
    } else {
        for (int i = optind; i < argc; i++) {
            read_path(&s.text, argv[i]);
        }
        s.curfile = argv[argc - 1];
    }
    s.dot.p1 = 0;
    s.dot.p2 = s.text.len;
    s.mark[DEFAULT_MARK] = s.dot;

    char *before = (mode != MODE_TEXT) ? buf_dup(&s.text, 0, s.text.len) : NULL;

    execute(&s, cmd);

    if (mode == MODE_TEXT) {
        fwrite(s.text.data, 1, s.text.len, stdout);
    } else {
        report_edits(before, s.text.data, mode, NULL);
    }

    free(before);
    cleanup_ssam(&s);
}

int
main(int argc, char **argv) {
    const char *inplace = extract_inplace_argv(&argc, argv);

    struct buf script;
    buf_init(&script);
    enum out_mode mode = MODE_TEXT;
    read_script(&script, argc, argv, &mode);

    struct cmd *cmd = parse_commands(&script);

    if (inplace) {
        if (optind >= argc) {
            fprintf(stderr, "ssam: -i requires file arguments\n");
            exit(1);
        }
        for (int i = optind; i < argc; i++) {
            edit_file(argv[i], cmd, inplace, mode);
        }
    } else {
        run_pipeline(cmd, argc, argv, mode);
    }

    cmd_free(cmd);
    buf_free(&script);
    return 0;
}
