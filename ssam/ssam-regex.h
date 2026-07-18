#if 0
#!/usr/bin/env -S ccraft -DTEST_REGEX
#endif  /* 0 */
// ssam-regex.h — regex engine for ssam
//
// WHAT IT DOES
//   Thompson NFA regex engine.  O(nm) time, no catastrophic backtracking.
//   Multiline by default, dot never matches newline.  ERE syntax,
//   backreferences, lookaround, Boyer-Moore-Horspool fast path.
//
// QUICK START
//   #include "regex.h"
//   struct rematch m[10];
//   if (re_find("hello", "hello world", m, 10, NULL))
//       printf("Found: %.*s\n", (int)(m[0].end - m[0].start), m[0].start);
//
// ============================================================================
// TUTORIAL: THOMPSON NFA
// ============================================================================
//
// A regex compiles to an NFA — a graph of states and transitions.
// The engine keeps a list of all active states, advancing each on
// every character.  Branching (|) forks the list; quantifiers add
// loops.  All paths run in parallel — no backtracking, no exponential
// blowup.
//
// Pattern "a(b|c)d" — the NFA branches at b/c, both paths simulated
// at once.  Thread list never exceeds pattern size → O(nm) always.
//
// Steps:
//   1. Parse pattern to NFA fragment graph (Thompson construction)
//   2. Simulate via double-buffered thread list (O(nm))
//   3. On match, populate capture groups from saved thread state
//   4. Literal patterns skip NFA (Boyer-Moore-Horspool)
//
// SYNTAX
// ------
// Most characters match themselves.  Metacharacters:
//
//   .     Any char except newline
//   ^ $   Start/end of line (multiline always on)
//   |     Alternation: cat|dog
//   \     Escape: \. matches literal dot
//   ()    Grouping and capture
//   []    Character class
//   *+?{} Quantifiers (lazy: *?, possessive: *+)

// QUANTIFIERS - HOW MANY TIMES TO MATCH
// ============================================================================
//
//   *         zero or more:     "ab*c"    matches "ac", "abc", "abbc"
//   +         one or more:      "ab+c"    matches "abc", "abbc" (not "ac")
//   ?         zero or one:      "colou?r" matches "color", "colour"
//   {n}       exactly n:        "a{3}"    matches "aaa"
//   {n,}      at least n:       "a{2,}"   matches "aa", "aaa", "aaaa"...
//   {n,m}     between n and m:  "a{2,4}"  matches "aa", "aaa", "aaaa"
//
// Quantifier modifiers:
//
//   *?  +?  ??  {n,m}?    Lazy: match as few as possible
//                         "<.*?>" on "<a><b>" matches "<a>" not "<a><b>"
//
//   *+  ++  ?+  {n,m}+    Possessive: match greedily, never backtrack
//                         "a*+a" FAILS on "aaa" (a*+ consumes all)
//
// ============================================================================
// CHARACTER CLASSES - MATCH ONE FROM A SET
// ============================================================================
//
//   [abc]     any of a, b, c:           "[aeiou]" matches vowels
//   [a-z]     range a through z:        "[0-9]" matches digits
//   [^abc]    any character NOT in set: "[^0-9]" matches non-digits
//   [a-zA-Z]  multiple ranges combined
//   []abc]    literal ] if first
//
// Unicode ranges (with "u" flag or \u{} escapes):
//
//   [\u{0400}-\u{04FF}]   Cyrillic block
//   [ä-ö]                 if input is UTF-8
//
// Shorthand classes:
//
//   \d  digit [0-9]                \D  non-digit [^0-9]
//   \w  word [a-zA-Z0-9_]          \W  non-word [^a-zA-Z0-9_]
//   \s  whitespace [ \t\n\r\f\v]   \S  non-whitespace
//
// ============================================================================
// ANCHORS AND BOUNDARIES
// ============================================================================
//
//   ^         start of line (or string with "m" flag)
//   $         end of line (or string with "m" flag)
//   \A        start of string (ignores multiline flag)
//   \z        end of string (ignores multiline flag)
//   \Z        end of string or before final newline
//   \b        word boundary: "\bword\b" matches "word" not "sword"
//   \B        non-word boundary: "\Bword" matches "sword" not "word"
//
// ============================================================================
// GROUPS
// ============================================================================
//
//   (...)     capturing group: saves matched text, accessible via m[1], m[2]...
//   (?:...)   non-capturing group: just for grouping, no capture
//
// ============================================================================
// ESCAPE SEQUENCES
// ============================================================================
//
//   \n  newline    \r  carriage return    \t  tab
//   \f  form feed  \v  vertical tab       \\  literal backslash
//   \.  literal dot (escape any metacharacter)
//
//   \xNN        hex byte (exactly 2 digits): \x41 = 'A'
//   \u{NNNN}    Unicode codepoint (1-6 hex digits): \u{00E9} = 'é'
//
// ============================================================================
// BRE vs ERE SYNTAX
// ============================================================================
//
// BRE (Basic) is the default. ERE (Extended) uses flag "e".
//
//   Feature        BRE          ERE
//   -------        ---          ---
//   Groups         \( \)        ( )
//   Alternation    \|           |
//   Repetition     \{ \}        { }
//   + and ?        literal      quantifiers
//
// ============================================================================
// API REFERENCE
// ============================================================================
//
// Compile a pattern:
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Configuration
// ============================================================================

// Sam-compatible: enable extensions by default
#ifndef RE_LOOKAROUND
#define RE_LOOKAROUND   1
#endif  /* RE_LOOKAROUND */
#ifndef RE_BACKREF
#define RE_BACKREF      1
#endif  /* RE_BACKREF */

#ifndef MAX_CAPTURES
#define MAX_CAPTURES    10
#endif  /* MAX_CAPTURES */
#ifndef MAX_STATES
#define MAX_STATES      1024
#endif  /* MAX_STATES */
#ifndef MAX_THREADS
#define MAX_THREADS     512
#endif  /* MAX_THREADS */
#ifndef MAX_LOOK_DEPTH
#define MAX_LOOK_DEPTH  16
#endif  /* MAX_LOOK_DEPTH */
#ifndef MAX_LITERAL
#define MAX_LITERAL     256
#endif  /* MAX_LITERAL */
#ifndef MAX_CP_RANGES
#define MAX_CP_RANGES   32      // Unicode codepoint ranges per class
#endif  /* MAX_CP_RANGES */
#define CLASS_BYTES     32      // 256 bits for ASCII charset

// ============================================================================
// Public Types
// ============================================================================

struct rematch {
    const char *start;
    const char *end;
};

// ============================================================================
// Internal Types
// ============================================================================

enum {
    ST_CHAR,        // match literal character (val = codepoint)
    ST_ANY,         // match any character (.)
    ST_CLASS,       // match character in class [abc]
    ST_NCLASS,      // match character NOT in class [^abc]
    ST_SPLIT,       // branch point (quantifiers, alternation)
    ST_SAVE,        // record capture position
    ST_BOL,         // ^ anchor (or \A in non-multiline)
    ST_EOL,         // $ anchor (or \z in non-multiline)
    ST_BOS,         // \A - beginning of string (ignores multiline)
    ST_EOS,         // \z - end of string (ignores multiline)
    ST_EOSB,        // \Z - end of string or before final \n
    ST_WBOUND,      // \b word boundary
    ST_NWBOUND,     // \B non-word boundary
    ST_MATCH,       // accept state
    ST_NOP,         // pass-through (merge point for multi-output fragments)
#ifdef RE_LOOKAROUND
    ST_LAHEAD_POS,  // (?=...)
    ST_LAHEAD_NEG,  // (?!...)
    ST_LBEHIND_POS, // (?<=...)
    ST_LBEHIND_NEG, // (?<!...)
#endif  /* RE_LOOKAROUND */
#ifdef RE_BACKREF
    ST_BACKREF,     // \1-\9
#endif  /* RE_BACKREF */
};

// Unicode codepoint range for character classes
struct cprange {
    int lo;
    int hi;
};

enum {
    Q_GREEDY,       // match more, allow backtrack
    Q_LAZY,         // match less first
    Q_POSSESSIVE,   // match more, no backtrack
};

struct state {
    int type;
    int val;            // codepoint for ST_CHAR, slot for ST_SAVE, etc.
    int quant;
    unsigned char cclass[CLASS_BYTES];      // ASCII bitmap
    struct cprange cpranges[MAX_CP_RANGES]; // Unicode ranges
    int ncpranges;
    struct state *out1;
    struct state *out2;
#ifdef RE_LOOKAROUND
    struct state *look;
    int look_width;
#endif  /* RE_LOOKAROUND */
};

struct thread {
    struct state *st;
    const char *cap[MAX_CAPTURES * 2];
};

struct regex {
    struct state *start;
    struct state *pool;     // dynamically allocated
    int nstates;
    int maxstates;          // allocated capacity
    int ncaptures;

    int ere;        // extended syntax
    int icase;      // case insensitive
    int multiline;  // ^ $ match at \n
    int dotall;     // . matches \n
    int utf8;       // UTF-8 mode
    int has_lazy;   // optimization flag

    // Literal optimization
    int is_literal;
    char literal[MAX_LITERAL];
    size_t literal_len;
    size_t literal_skip[256];

    const char *text;
    const char *text_end;
    const char *next_search;

    int err;
    const char *errpos;
    char errmsg[128];
    int overflow;
    int look_depth;
};

// ============================================================================
// UTF-8 Support
// ============================================================================

// Decode UTF-8 codepoint, return bytes consumed (0 on error)
static int
utf8_decode(const char *s, const char *end, int *cp) {
    if (s >= end) return 0;

    unsigned char c = (unsigned char)*s;

    if (c < 0x80) {
        *cp = c;
        return 1;
    }

    if ((c & 0xE0) == 0xC0 && s + 1 < end) {
        *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }

    if ((c & 0xF0) == 0xE0 && s + 2 < end) {
        *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }

    if ((c & 0xF8) == 0xF0 && s + 3 < end) {
        *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
              ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }

    *cp = c;  // invalid, treat as byte
    return 1;
}

// Encode UTF-8 codepoint, return bytes written
static int
utf8_encode(int cp, char *buf) {
    if (cp < 0x80) {
        buf[0] = cp;
        return 1;
    }

    if (cp < 0x800) {
        buf[0] = 0xC0 | (cp >> 6);
        buf[1] = 0x80 | (cp & 0x3F);
        return 2;
    }

    if (cp < 0x10000) {
        buf[0] = 0xE0 | (cp >> 12);
        buf[1] = 0x80 | ((cp >> 6) & 0x3F);
        buf[2] = 0x80 | (cp & 0x3F);
        return 3;
    }

    buf[0] = 0xF0 | (cp >> 18);
    buf[1] = 0x80 | ((cp >> 12) & 0x3F);
    buf[2] = 0x80 | ((cp >> 6) & 0x3F);
    buf[3] = 0x80 | (cp & 0x3F);
    return 4;
}

// Check if codepoint is word character (basic Unicode awareness)
static int
is_word_cp(int cp) {
    if (cp < 128) {
        return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
               (cp >= '0' && cp <= '9') || cp == '_';
    }
    if (cp >= 0x00C0 && cp <= 0x024F) return 1;  // Latin Extended
    if (cp >= 0x0370 && cp <= 0x03FF) return 1;  // Greek
    if (cp >= 0x0400 && cp <= 0x04FF) return 1;  // Cyrillic
    return 0;
}

// ============================================================================
// Character Predicates
// ============================================================================

static int is_nul(int c)      { return c == '\0'; }
static int is_newline(int c)  { return c == '\n'; }
static int is_escape(int c)   { return c == '\\'; }
static int is_caret(int c)    { return c == '^'; }
static int is_dollar(int c)   { return c == '$'; }
static int is_dot(int c)      { return c == '.'; }
static int is_pipe(int c)     { return c == '|'; }
static int is_star(int c)     { return c == '*'; }
static int is_plus(int c)     { return c == '+'; }
static int is_question(int c) { return c == '?'; }
static int is_lparen(int c)   { return c == '('; }
static int is_rparen(int c)   { return c == ')'; }
static int is_lbracket(int c) { return c == '['; }
static int is_rbracket(int c) { return c == ']'; }
static int is_lbrace(int c)   { return c == '{'; }
static int is_rbrace(int c)   { return c == '}'; }
static int is_hyphen(int c)   { return c == '-'; }
static int is_colon(int c)    { return c == ':'; }
static int is_equals(int c)   { return c == '='; }
static int is_bang(int c)     { return c == '!'; }
static int is_less(int c)     { return c == '<'; }
static int is_digit(int c)    { return c >= '0' && c <= '9'; }

static int
is_word(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int
is_meta(int c) {
    return c && strchr("\\^$.|*+?()[]{}", c);
}

// ============================================================================
// Domain Predicates - Questions About State
// ============================================================================

static int has_error(struct regex *re)     { return re->err; }
static int has_overflow(struct regex *re)  { return re->overflow; }
static int states_full(struct regex *re)   { return re->nstates >= re->maxstates; }
static int threads_full(int n)             { return n >= MAX_THREADS; }
static int no_threads(int n)               { return n == 0; }
static int captures_full(int g)            { return g >= MAX_CAPTURES; }

static int is_match(struct state *s)       { return s->type == ST_MATCH; }
static int is_split(struct state *s)       { return s->type == ST_SPLIT; }
static int is_save(struct state *s)        { return s->type == ST_SAVE; }
static int is_bol(struct state *s)         { return s->type == ST_BOL; }
static int is_eol(struct state *s)         { return s->type == ST_EOL; }
static int is_bos(struct state *s)         { return s->type == ST_BOS; }
static int is_eos(struct state *s)         { return s->type == ST_EOS; }
static int is_eosb(struct state *s)        { return s->type == ST_EOSB; }
static int is_wbound(struct state *s)      { return s->type == ST_WBOUND; }
static int is_nwbound(struct state *s)     { return s->type == ST_NWBOUND; }
static int is_nop(struct state *s)         { return s->type == ST_NOP; }

static int
consumes_char(struct state *s) {
    return s->type == ST_CHAR || s->type == ST_ANY ||
           s->type == ST_CLASS || s->type == ST_NCLASS;
}

static int
is_anchor(struct state *s) {
    return is_bol(s) || is_eol(s) || is_bos(s) || is_eos(s) || is_eosb(s);
}

static int
is_boundary(struct state *s) {
    return is_wbound(s) || is_nwbound(s);
}

#ifdef RE_LOOKAROUND
static int is_lahead_pos(struct state *s)  { return s->type == ST_LAHEAD_POS; }
static int is_lahead_neg(struct state *s)  { return s->type == ST_LAHEAD_NEG; }
static int is_lbehind_pos(struct state *s) { return s->type == ST_LBEHIND_POS; }
static int is_lbehind_neg(struct state *s) { return s->type == ST_LBEHIND_NEG; }

static int
is_lookaround(struct state *s) {
    return is_lahead_pos(s) || is_lahead_neg(s) ||
           is_lbehind_pos(s) || is_lbehind_neg(s);
}
#endif  /* RE_LOOKAROUND */

#ifdef RE_BACKREF
static int is_backref(struct state *s)     { return s->type == ST_BACKREF; }
#endif  /* RE_BACKREF */

static int is_lazy(int q)       { return q == Q_LAZY; }
static int is_possessive(int q) { return q == Q_POSSESSIVE; }

static int uses_ere(struct regex *re)       { return re->ere; }
static int uses_icase(struct regex *re)     { return re->icase; }
static int uses_multiline(struct regex *re) { return re->multiline; }
static int uses_dotall(struct regex *re)    { return re->dotall; }
static int uses_utf8(struct regex *re)      { return re->utf8; }

static int at_text_start(const char *p, struct regex *re) { return p == re->text; }
static int at_text_end(const char *p, struct regex *re)   { return p == re->text_end; }
static int past_text_end(const char *p, struct regex *re) { return p > re->text_end; }
static int within_text(const char *p, struct regex *re)   { return p < re->text_end; }

static int
after_newline(const char *p, struct regex *re) {
    return p > re->text && is_newline(p[-1]);
}

static int
at_newline(const char *p, struct regex *re) {
    return within_text(p, re) && is_newline(*p);
}

// ============================================================================
// Case Conversion
// ============================================================================

static int
to_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int
to_upper(int c) {
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

// ============================================================================
// Character Class Bit Vector
// ============================================================================

static int
class_has(const unsigned char *cc, int c) {
    if (c < 0 || c > 255) return 0;
    return (cc[(unsigned char)c >> 3] >> ((unsigned char)c & 7)) & 1;
}

static void
class_set(unsigned char *cc, int c) {
    if (c >= 0 && c <= 255)
        cc[(unsigned char)c >> 3] |= 1 << ((unsigned char)c & 7);
}

static void
class_set_range(unsigned char *cc, int lo, int hi) {
    for (int c = lo; c <= hi && c <= 255; c++) class_set(cc, c);
}

static void
class_set_digits(unsigned char *cc) {
    class_set_range(cc, '0', '9');
}

static void
class_set_word(unsigned char *cc) {
    class_set_range(cc, 'a', 'z');
    class_set_range(cc, 'A', 'Z');
    class_set_range(cc, '0', '9');
    class_set(cc, '_');
}

static void
class_set_space(unsigned char *cc) {
    class_set(cc, ' ');
    class_set(cc, '\t');
    class_set(cc, '\n');
    class_set(cc, '\r');
    class_set(cc, '\f');
    class_set(cc, '\v');
}

// Add codepoint range to state (for Unicode character classes)
static int
class_add_cprange(struct state *s, int lo, int hi) {
    if (s->ncpranges >= MAX_CP_RANGES) return 0;
    s->cpranges[s->ncpranges].lo = lo;
    s->cpranges[s->ncpranges].hi = hi;
    s->ncpranges++;
    return 1;
}

// Check if codepoint is in any range
static int
class_has_cp(struct state *s, int cp) {
    // ASCII: check bitmap
    if (cp < 256 && class_has(s->cclass, cp)) return 1;
    // Unicode: check ranges
    for (int i = 0; i < s->ncpranges; i++) {
        if (cp >= s->cpranges[i].lo && cp <= s->cpranges[i].hi)
            return 1;
    }
    return 0;
}

static void
class_add_word_ranges(struct state *s) {
    class_add_cprange(s, 0x00C0, 0x024F);  // Latin Extended
    class_add_cprange(s, 0x0370, 0x03FF);  // Greek
    class_add_cprange(s, 0x0400, 0x04FF);  // Cyrillic
}

// ============================================================================
// Error Handling
// ============================================================================

static void
fail(struct regex *re, const char *msg, const char *pos) {
    if (has_error(re)) return;
    re->err = 1;
    re->errpos = pos;
    strncpy(re->errmsg, msg, sizeof re->errmsg - 1);
    re->errmsg[sizeof re->errmsg - 1] = '\0';
}

// ============================================================================
// NFA State Constructors
// ============================================================================

static struct state *
alloc_state(struct regex *re, int type) {
    if (states_full(re)) {
        fail(re, "too many states", NULL);
        return NULL;
    }
    struct state *s = &re->pool[re->nstates++];
    memset(s, 0, sizeof *s);
    s->type = type;
    return s;
}

static struct state *
make_char(struct regex *re, int c) {
    struct state *s = alloc_state(re, ST_CHAR);
    if (s) s->val = c;
    return s;
}

static struct state *
make_any(struct regex *re) {
    return alloc_state(re, ST_ANY);
}

static struct state *
make_class(struct regex *re, int negated) {
    return alloc_state(re, negated ? ST_NCLASS : ST_CLASS);
}

static struct state *
make_split(struct regex *re, struct state *a, struct state *b) {
    struct state *s = alloc_state(re, ST_SPLIT);
    if (s) { s->out1 = a; s->out2 = b; }
    return s;
}

static struct state *
make_save(struct regex *re, int slot) {
    struct state *s = alloc_state(re, ST_SAVE);
    if (s) s->val = slot;
    return s;
}

static struct state *make_bol(struct regex *re)     { return alloc_state(re, ST_BOL); }
static struct state *make_eol(struct regex *re)     { return alloc_state(re, ST_EOL); }
static struct state *make_bos(struct regex *re)     { return alloc_state(re, ST_BOS); }
static struct state *make_eos(struct regex *re)     { return alloc_state(re, ST_EOS); }
static struct state *make_eosb(struct regex *re)    { return alloc_state(re, ST_EOSB); }
static struct state *make_wbound(struct regex *re)  { return alloc_state(re, ST_WBOUND); }
static struct state *make_nwbound(struct regex *re) { return alloc_state(re, ST_NWBOUND); }
static struct state *make_match(struct regex *re)   { return alloc_state(re, ST_MATCH); }

// Create split state with quantifier and lazy wiring.
// Returns split state; sets *exit to the exit pointer.
static struct state *
make_quantified_split(struct regex *re, struct state *loop, int q,
                      struct state ***exit) {
    struct state *sp = make_split(re, loop, NULL);
    if (!sp) return NULL;
    sp->quant = q;
    if (is_lazy(q)) { sp->out1 = NULL; sp->out2 = loop; }
    *exit = is_lazy(q) ? &sp->out1 : &sp->out2;
    return sp;
}

#ifdef RE_BACKREF
static struct state *
make_backref(struct regex *re, int n) {
    struct state *s = alloc_state(re, ST_BACKREF);
    if (s) s->val = n;
    return s;
}
#endif  /* RE_BACKREF */

// ============================================================================
// NFA Fragment - Partial NFA With Dangling Outputs
// ============================================================================

struct frag {
    struct state *start;
    struct state **out;
    struct state **out2;
};

static struct frag
make_frag(struct state *start, struct state **out) {
    return (struct frag){ start, out, NULL };
}

static struct frag
no_frag(void) {
    return (struct frag){ NULL, NULL, NULL };
}

static int
frag_empty(struct frag f) {
    return f.start == NULL;
}

static void
frag_connect(struct frag *f, struct state *target) {
    if (f->out) *f->out = target;
    if (f->out2) *f->out2 = target;
}

// ============================================================================
// Parser State
// ============================================================================

struct parser {
    struct regex *re;
    const char *p;
    int group;
};

static int  peek(struct parser *ps)      { return (unsigned char)*ps->p; }
static int  peek2(struct parser *ps)     { return (unsigned char)ps->p[1]; }
static int  at_end(struct parser *ps)    { return is_nul(*ps->p); }
static int  take(struct parser *ps)      { return (unsigned char)*ps->p++; }
static void skip(struct parser *ps)      { ps->p++; }
static void skip_n(struct parser *ps, int n) { ps->p += n; }

static int
accept(struct parser *ps, int c) {
    if (*ps->p != c) return 0;
    ps->p++;
    return 1;
}

// ============================================================================
// Parse Helpers
// ============================================================================

static int
hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int
is_hex(int c) {
    return hex_digit(c) >= 0;
}

// Parse \xNN (exactly 2 hex digits)
static int
parse_hex2(struct parser *ps) {
    if (at_end(ps) || !is_hex(peek(ps))) {
        fail(ps->re, "invalid \\x escape", ps->p);
        return -1;
    }
    int h1 = hex_digit(take(ps));
    if (at_end(ps) || !is_hex(peek(ps))) {
        fail(ps->re, "invalid \\x escape", ps->p);
        return -1;
    }
    int h2 = hex_digit(take(ps));
    return (h1 << 4) | h2;
}

// Parse \u{NNNN} or \u{NNNNNN} (1-6 hex digits in braces)
static int
parse_unicode(struct parser *ps) {
    if (!accept(ps, '{')) {
        fail(ps->re, "expected { after \\u", ps->p);
        return -1;
    }
    int cp = 0;
    int digits = 0;
    while (!at_end(ps) && is_hex(peek(ps)) && digits < 6) {
        cp = (cp << 4) | hex_digit(take(ps));
        digits++;
    }
    if (digits == 0) {
        fail(ps->re, "empty \\u{} escape", ps->p);
        return -1;
    }
    if (!accept(ps, '}')) {
        fail(ps->re, "unterminated \\u{} escape", ps->p);
        return -1;
    }
    if (cp > 0x10FFFF) {
        fail(ps->re, "codepoint out of range", ps->p);
        return -1;
    }
    return cp;
}

static int
parse_escape_char(struct parser *ps) {
    if (at_end(ps)) {
        fail(ps->re, "trailing backslash", ps->p);
        return -1;
    }
    int c = take(ps);
    switch (c) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return '\0';
    case 'x': return parse_hex2(ps);
    case 'u': return parse_unicode(ps);
    default:  return c;
    }
}

// ============================================================================
// Parse Character Class [...]
// ============================================================================

static int
looking_at_range(struct parser *ps) {
    return is_hyphen(peek(ps)) && peek2(ps) && !is_rbracket(peek2(ps));
}

// Add character/range to class, handling both ASCII and Unicode
static void
class_add_char_or_range(struct state *s, int lo, int hi, int icase) {
    if (hi < 0) hi = lo;  // single char

    // ASCII range: use bitmap
    if (lo < 256 && hi < 256) {
        class_set_range(s->cclass, lo, hi);
        if (icase) {
            for (int c = lo; c <= hi; c++) {
                class_set(s->cclass, to_lower(c));
                class_set(s->cclass, to_upper(c));
            }
        }
        return;
    }

    // Mixed or Unicode range: split at boundary
    if (lo < 256) {
        class_set_range(s->cclass, lo, 255);
        if (icase) {
            for (int c = lo; c < 256; c++) {
                class_set(s->cclass, to_lower(c));
                class_set(s->cclass, to_upper(c));
            }
        }
        lo = 256;
    }

    // Pure Unicode range
    class_add_cprange(s, lo, hi);
}

static struct frag
parse_class(struct parser *ps) {
    int neg = accept(ps, '^');
    struct state *s = make_class(ps->re, neg);
    if (!s) return no_frag();

    if (accept(ps, ']')) class_set(s->cclass, ']');

    while (!at_end(ps) && !is_rbracket(peek(ps))) {
        int lo = is_escape(peek(ps)) ? (skip(ps), parse_escape_char(ps)) : take(ps);
        if (lo < 0) return no_frag();

        if (looking_at_range(ps)) {
            skip(ps);
            int hi = is_escape(peek(ps)) ? (skip(ps), parse_escape_char(ps)) : take(ps);
            if (hi < 0) return no_frag();
            if (hi < lo) { fail(ps->re, "invalid range", ps->p); return no_frag(); }
            class_add_char_or_range(s, lo, hi, uses_icase(ps->re));
        } else {
            class_add_char_or_range(s, lo, lo, uses_icase(ps->re));
        }
    }

    if (!accept(ps, ']')) { fail(ps->re, "unterminated class", ps->p); return no_frag(); }
    return make_frag(s, &s->out1);
}

// ============================================================================
// Parse Escape Sequences
// ============================================================================

static struct frag
parse_escape(struct parser *ps) {
    if (at_end(ps)) { fail(ps->re, "trailing backslash", ps->p); return no_frag(); }

    int c = take(ps);
    struct state *s;

    switch (c) {
    case 'n': s = make_char(ps->re, '\n'); break;
    case 'r': s = make_char(ps->re, '\r'); break;
    case 't': s = make_char(ps->re, '\t'); break;
    case 'f': s = make_char(ps->re, '\f'); break;
    case 'v': s = make_char(ps->re, '\v'); break;

    case 'x': {
        int cp = parse_hex2(ps);
        if (cp < 0) return no_frag();
        s = make_char(ps->re, cp);
        break;
    }
    case 'u': {
        int cp = parse_unicode(ps);
        if (cp < 0) return no_frag();
        s = make_char(ps->re, cp);
        break;
    }

    case 'd': s = make_class(ps->re, 0); if (s) class_set_digits(s->cclass); break;
    case 'D': s = make_class(ps->re, 1); if (s) class_set_digits(s->cclass); break;
    case 'w':
        s = make_class(ps->re, 0);
        if (s) { class_set_word(s->cclass); class_add_word_ranges(s); }
        break;
    case 'W':
        s = make_class(ps->re, 1);
        if (s) { class_set_word(s->cclass); class_add_word_ranges(s); }
        break;
    case 's': s = make_class(ps->re, 0); if (s) class_set_space(s->cclass); break;
    case 'S': s = make_class(ps->re, 1); if (s) class_set_space(s->cclass); break;

    case 'b': s = make_wbound(ps->re);  return s ? make_frag(s, &s->out1) : no_frag();
    case 'B': s = make_nwbound(ps->re); return s ? make_frag(s, &s->out1) : no_frag();

    case 'A': s = make_bos(ps->re);  return s ? make_frag(s, &s->out1) : no_frag();
    case 'z': s = make_eos(ps->re);  return s ? make_frag(s, &s->out1) : no_frag();
    case 'Z': s = make_eosb(ps->re); return s ? make_frag(s, &s->out1) : no_frag();

#ifdef RE_BACKREF
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9':
        s = make_backref(ps->re, c - '0');
        break;
#endif  /* RE_BACKREF */

    case '(': case ')': case '{':
        if (!uses_ere(ps->re)) { ps->p--; return no_frag(); }
        s = make_char(ps->re, c);
        break;

    default:
        s = make_char(ps->re, c);
        break;
    }

    return s ? make_frag(s, &s->out1) : no_frag();
}

// ============================================================================
// Parse Group
// ============================================================================

static struct frag parse_regex(struct parser *ps);

#ifdef RE_LOOKAROUND
static int
calc_fixed_width(struct state *s) {
    if (!s) return 0;
    int w = 0;
    while (s && !is_match(s)) {
        if (consumes_char(s))                    { w++; s = s->out1; continue; }
        if (is_save(s) || is_bol(s) || is_eol(s) ||
            is_wbound(s) || is_nwbound(s))       { s = s->out1; continue; }
        if (is_split(s)) {
            int w1 = calc_fixed_width(s->out1);
            int w2 = calc_fixed_width(s->out2);
            return (w1 < 0 || w2 < 0 || w1 != w2) ? -1 : w + w1;
        }
#ifdef RE_BACKREF
        if (is_backref(s)) return -1;
#endif  /* RE_BACKREF */
        return -1;
    }
    return w;
}

static struct frag
parse_lookaround(struct parser *ps, int type) {
    struct state *look = alloc_state(ps->re, type);
    if (!look) return no_frag();

    struct frag inner = parse_regex(ps);
    if (has_error(ps->re)) return no_frag();

    struct state *end = make_match(ps->re);
    if (!end) return no_frag();
    frag_connect(&inner, end);
    look->look = inner.start;

    if (type == ST_LBEHIND_POS || type == ST_LBEHIND_NEG) {
        int w = calc_fixed_width(inner.start);
        if (w < 0) { fail(ps->re, "lookbehind requires fixed width", ps->p); return no_frag(); }
        look->look_width = w;
    }

    if (!accept(ps, ')')) { fail(ps->re, "unterminated lookaround", ps->p); return no_frag(); }
    return make_frag(look, &look->out1);
}
#endif  /* RE_LOOKAROUND */

static int
at_bre_rparen(struct parser *ps) {
    return is_escape(peek(ps)) && is_rparen(peek2(ps));
}

// Allocate capture-start save state, assign group number.
// Returns NULL for non-capturing groups. Caller checks capturing flag.
static struct state *
alloc_capture_start(struct parser *ps, int capturing, int *gnum) {
    if (!capturing) return NULL;

    *gnum = ps->group++;
    if (captures_full(*gnum)) {
        fail(ps->re, "too many captures", ps->p);
        return NULL;
    }
    return make_save(ps->re, *gnum * 2);
}

// Consume closing paren. Handles BRE (\) and ERE ( ) syntax.
// Returns 0 on error.
static int
expect_group_close(struct parser *ps) {
    if (uses_ere(ps->re)) {
        if (!accept(ps, ')')) { fail(ps->re, "unterminated group", ps->p); return 0; }
        return 1;
    }
    if (!at_bre_rparen(ps)) { fail(ps->re, "unterminated group", ps->p); return 0; }
    skip_n(ps, 2);
    return 1;
}

// Wrap inner fragment with capture-end save state.
static struct frag
wrap_capture(struct state *cap_start, struct frag inner, struct regex *re, int gnum) {
    struct state *cap_end = make_save(re, gnum * 2 + 1);

    if (!cap_end) return no_frag();
    cap_start->out1 = inner.start ? inner.start : cap_end;
    if (inner.start) frag_connect(&inner, cap_end);
    return make_frag(cap_start, &cap_end->out1);
}

static struct frag
parse_group(struct parser *ps) {
    int capturing = 1;
    int gnum = 0;

    // Phase 1: detect group type — lookaround, non-capturing, or capturing
    if (is_question(peek(ps))) {
        skip(ps);
        int c = peek(ps);
        if (is_colon(c))            { skip(ps); capturing = 0; }
#ifdef RE_LOOKAROUND
        else if (is_equals(c))      { skip(ps); return parse_lookaround(ps, ST_LAHEAD_POS); }
        else if (is_bang(c))        { skip(ps); return parse_lookaround(ps, ST_LAHEAD_NEG); }
        else if (is_less(c)) {
            skip(ps);
            if (is_equals(peek(ps))) { skip(ps); return parse_lookaround(ps, ST_LBEHIND_POS); }
            if (is_bang(peek(ps)))   { skip(ps); return parse_lookaround(ps, ST_LBEHIND_NEG); }
            fail(ps->re, "invalid group", ps->p);
            return no_frag();
        }
#endif  /* RE_LOOKAROUND */
        else { fail(ps->re, "invalid group", ps->p); return no_frag(); }
    }

    // Phase 2: set up capture start state
    struct state *cap_start = alloc_capture_start(ps, capturing, &gnum);
    if (capturing && !cap_start) return no_frag();

    // Phase 3: parse inner expression
    struct frag inner = parse_regex(ps);
    if (has_error(ps->re)) return no_frag();

    // Phase 4: close group
    if (!expect_group_close(ps)) return no_frag();

    // Phase 5: wrap captures or return bare inner
    if (!capturing) return inner;
    return wrap_capture(cap_start, inner, ps->re, gnum);
}

// ============================================================================
// Parse Atom
// ============================================================================

static int
at_alt_or_close(struct parser *ps) {
    int c = peek(ps);
    return is_pipe(c) || is_rparen(c);
}

static struct frag
parse_atom(struct parser *ps) {
    int c = peek(ps);

    if (is_nul(c)) return no_frag();

    if (is_caret(c)) {
        skip(ps);
        struct state *s = make_bol(ps->re);
        return s ? make_frag(s, &s->out1) : no_frag();
    }
    if (is_dollar(c)) {
        skip(ps);
        struct state *s = make_eol(ps->re);
        return s ? make_frag(s, &s->out1) : no_frag();
    }
    if (is_dot(c)) {
        skip(ps);
        struct state *s = make_any(ps->re);
        return s ? make_frag(s, &s->out1) : no_frag();
    }
    if (is_lbracket(c)) {
        skip(ps);
        return parse_class(ps);
    }
    if (is_escape(c)) {
        skip(ps);
        if (!uses_ere(ps->re) && is_lparen(peek(ps))) { skip(ps); return parse_group(ps); }
        return parse_escape(ps);
    }
    if (is_lparen(c) && uses_ere(ps->re)) {
        skip(ps);
        return parse_group(ps);
    }
    if (uses_ere(ps->re) && at_alt_or_close(ps)) {
        return no_frag();
    }
    if (!is_meta(c)) {
        skip(ps);
        int ch = uses_icase(ps->re) ? to_lower(c) : c;
        struct state *s = make_char(ps->re, ch);
        return s ? make_frag(s, &s->out1) : no_frag();
    }
    return no_frag();
}

// ============================================================================
// Fragment Cloning (for bounded repetition)
// ============================================================================

#define MAX_CLONE 128

struct clone_map {
    struct state *old[MAX_CLONE];
    struct state *new[MAX_CLONE];
    int n;
};

static struct state *
find_clone(struct clone_map *m, struct state *s) {
    for (int i = 0; i < m->n; i++)
        if (m->old[i] == s) return m->new[i];
    return NULL;
}

static struct state *
clone_deep(struct regex *re, struct state *s, struct clone_map *m) {
    if (!s) return NULL;
    struct state *found = find_clone(m, s);
    if (found) return found;
    if (m->n >= MAX_CLONE) { fail(re, "pattern too complex", NULL); return NULL; }

    struct state *c = alloc_state(re, s->type);
    if (!c) return NULL;
    c->val = s->val;
    c->quant = s->quant;
    memcpy(c->cclass, s->cclass, CLASS_BYTES);
    memcpy(c->cpranges, s->cpranges, sizeof(s->cpranges));
    c->ncpranges = s->ncpranges;
#ifdef RE_LOOKAROUND
    c->look = s->look;
    c->look_width = s->look_width;
#endif  /* RE_LOOKAROUND */
    m->old[m->n] = s;
    m->new[m->n] = c;
    m->n++;

    c->out1 = clone_deep(re, s->out1, m);
    c->out2 = clone_deep(re, s->out2, m);
    return c;
}

static struct state **
find_clone_ptr(struct clone_map *m, struct state **p) {
    if (!p) return NULL;
    for (int i = 0; i < m->n; i++) {
        if (p == &m->old[i]->out1) return &m->new[i]->out1;
        if (p == &m->old[i]->out2) return &m->new[i]->out2;
    }
    return NULL;
}

static struct frag
clone_frag(struct regex *re, struct frag f) {
    struct clone_map m = {0};
    struct state *start = clone_deep(re, f.start, &m);
    if (!start) return no_frag();
    struct frag r = make_frag(start, find_clone_ptr(&m, f.out));
    r.out2 = find_clone_ptr(&m, f.out2);
    return r;
}

// ============================================================================
// Parse Bounded Repetition {n,m}
// ============================================================================

static int
parse_int(struct parser *ps) {
    int n = 0;
    while (is_digit(peek(ps))) {
        n = n * 10 + (take(ps) - '0');
        if (n > 1000) { fail(ps->re, "repetition too large", ps->p); return -1; }
    }
    return n;
}

// Clone first occurrence of atom, keep original on repeat use
static struct frag
repeat_atom(struct regex *re, struct frag atom, int *used) {
    struct frag f = *used ? clone_frag(re, atom) : atom;

    *used = 1;
    return f;
}

static struct frag
build_repeat(struct parser *ps, struct frag atom, int min, int max, int q) {
    if (min == 0 && max == 0) return no_frag();

    struct regex *re = ps->re;
    struct frag result = no_frag();
    int used = 0;

    // Phase 1: mandatory repeats
    for (int i = 0; i < min; i++) {
        struct frag copy = repeat_atom(re, atom, &used);

        if (frag_empty(copy)) return no_frag();
        if (frag_empty(result)) { result = copy; }
        else { frag_connect(&result, copy.start); result.out = copy.out; }
    }

    // Phase 2: infinite loop (max == -1)
    if (max == -1) {
        struct frag loop = repeat_atom(re, atom, &used);

        if (frag_empty(loop)) return no_frag();
        struct state **exit;
        struct state *sp = make_quantified_split(re, loop.start, q, &exit);
        if (!sp) return no_frag();
        frag_connect(&loop, sp);
        if (frag_empty(result)) return make_frag(sp, exit);
        frag_connect(&result, sp);
        return make_frag(result.start, exit);
    }

    // Phase 3: bounded optional repeats
    for (int i = min; i < max; i++) {
        struct frag opt = repeat_atom(re, atom, &used);

        if (frag_empty(opt)) return no_frag();
        struct state **exit;
        struct state *sp = make_quantified_split(re, opt.start, q, &exit);
        if (!sp) return no_frag();
        struct state *nop = alloc_state(re, ST_NOP);
        if (!nop) return no_frag();
        *exit = nop;
        *opt.out = nop;
        if (frag_empty(result)) {
            result.start = sp;
            result.out = &nop->out1;
        } else {
            frag_connect(&result, sp);
            result.out = &nop->out1;
        }
    }
    return result;
}

static int
at_bre_rbrace(struct parser *ps) {
    return is_escape(peek(ps)) && is_rbrace(peek2(ps));
}

static struct frag
parse_bounded(struct parser *ps, struct frag atom) {
    int min = parse_int(ps);
    if (min < 0) return no_frag();
    int max = min;

    if (accept(ps, ',')) {
        if (is_digit(peek(ps))) {
            max = parse_int(ps);
            if (max < 0) return no_frag();
            if (max < min) { fail(ps->re, "invalid range", ps->p); return no_frag(); }
        } else {
            max = -1;
        }
    }

    if (uses_ere(ps->re)) {
        if (!accept(ps, '}')) { fail(ps->re, "unterminated repetition", ps->p); return no_frag(); }
    } else {
        if (!at_bre_rbrace(ps)) { fail(ps->re, "unterminated repetition", ps->p); return no_frag(); }
        skip_n(ps, 2);
    }

    int q = Q_GREEDY;
    if (accept(ps, '?'))      { q = Q_LAZY; ps->re->has_lazy = 1; }
    else if (accept(ps, '+')) { q = Q_POSSESSIVE; }

    return build_repeat(ps, atom, min, max, q);
}

// ============================================================================
// Parse Quantifiers
// ============================================================================

static int
parse_quant_mod(struct parser *ps) {
    if (accept(ps, '?')) { ps->re->has_lazy = 1; return Q_LAZY; }
    if (accept(ps, '+')) { return Q_POSSESSIVE; }
    return Q_GREEDY;
}

static struct frag
apply_star(struct parser *ps, struct frag atom, int q) {
    struct state **exit;
    struct state *sp = make_quantified_split(ps->re, atom.start, q, &exit);
    if (!sp) return no_frag();
    frag_connect(&atom, sp);
    return make_frag(sp, exit);
}

static struct frag
apply_plus(struct parser *ps, struct frag atom, int q) {
    struct state **exit;
    struct state *sp = make_quantified_split(ps->re, atom.start, q, &exit);
    if (!sp) return no_frag();
    frag_connect(&atom, sp);
    return make_frag(atom.start, exit);
}

static struct frag
apply_opt(struct parser *ps, struct frag atom, int q) {
    struct state **exit;
    struct state *sp = make_quantified_split(ps->re, atom.start, q, &exit);
    if (!sp) return no_frag();
    // Merge both paths (skip vs match) into a single output via NOP
    struct state *nop = alloc_state(ps->re, ST_NOP);
    if (!nop) return no_frag();
    *exit = nop;
    if (atom.out) *atom.out = nop;
    return make_frag(sp, &nop->out1);
}

static int
at_bre_lbrace(struct parser *ps) {
    return is_escape(peek(ps)) && is_lbrace(peek2(ps));
}

static struct frag
parse_piece(struct parser *ps) {
    struct frag atom = parse_atom(ps);
    if (frag_empty(atom) || has_error(ps->re)) return atom;

    int c = peek(ps);
    if (is_star(c))     { skip(ps); return apply_star(ps, atom, parse_quant_mod(ps)); }
    if (is_plus(c))     { skip(ps); return apply_plus(ps, atom, parse_quant_mod(ps)); }
    if (is_question(c)) { skip(ps); return apply_opt(ps, atom, parse_quant_mod(ps)); }
    if (is_lbrace(c) && uses_ere(ps->re)) { skip(ps); return parse_bounded(ps, atom); }
    if (!uses_ere(ps->re) && at_bre_lbrace(ps)) { skip_n(ps, 2); return parse_bounded(ps, atom); }
    return atom;
}

// ============================================================================
// Parse Concatenation
// ============================================================================

static int
at_bre_alt_or_close(struct parser *ps) {
    return is_escape(peek(ps)) && (is_rparen(peek2(ps)) || is_pipe(peek2(ps)));
}

static int
at_concat_end(struct parser *ps) {
    if (is_nul(peek(ps))) return 1;
    if (uses_ere(ps->re) && at_alt_or_close(ps)) return 1;
    if (!uses_ere(ps->re) && at_bre_alt_or_close(ps)) return 1;
    return 0;
}

static struct frag
parse_concat(struct parser *ps) {
    struct frag result = no_frag();
    while (!at_concat_end(ps)) {
        struct frag piece = parse_piece(ps);
        if (has_error(ps->re)) return no_frag();
        if (frag_empty(piece)) break;
        if (frag_empty(result)) { result = piece; }
        else { frag_connect(&result, piece.start); result.out = piece.out; result.out2 = piece.out2; }
    }
    return result;
}

// ============================================================================
// Parse Alternation
// ============================================================================

static int at_ere_alt(struct parser *ps) { return uses_ere(ps->re) && is_pipe(peek(ps)); }
static int at_bre_alt(struct parser *ps) { return !uses_ere(ps->re) && is_escape(peek(ps)) && is_pipe(peek2(ps)); }

static struct frag
parse_regex(struct parser *ps) {
    struct frag left = parse_concat(ps);
    if (has_error(ps->re)) return no_frag();

    while (at_ere_alt(ps) || at_bre_alt(ps)) {
        at_ere_alt(ps) ? skip(ps) : skip_n(ps, 2);
        struct frag right = parse_concat(ps);
        if (has_error(ps->re)) return no_frag();
        struct state *sp = make_split(ps->re, left.start, right.start);
        if (!sp) return no_frag();
        left = (struct frag){ sp, left.out, right.out };
    }
    return left;
}

// ============================================================================
// Literal Pattern Detection & Boyer-Moore
// ============================================================================

static void
build_bmh_table(struct regex *re, size_t len) {
    for (int i = 0; i < 256; i++) re->literal_skip[i] = len;
    for (size_t i = 0; i + 1 < len; i++) {
        unsigned char c = (unsigned char)re->literal[i];
        re->literal_skip[c] = len - 1 - i;
        if (re->icase) {
            re->literal_skip[to_lower(c)] = len - 1 - i;
            re->literal_skip[to_upper(c)] = len - 1 - i;
        }
    }
}

// Encode codepoint and append to literal buffer. Returns bytes written.
static int
append_literal_cp(struct regex *re, int cp, size_t *len) {
    if (*len >= MAX_LITERAL - 4) return 0;

    if (cp > 127) {
        char buf[4];
        int n = utf8_encode(cp, buf);

        for (int i = 0; i < n; i++)
            re->literal[(*len)++] = buf[i];
        return n;
    }
    re->literal[(*len)++] = cp;
    return 1;
}

static int
extract_literal(struct regex *re) {
    // Don't use literal fast path when there are capture groups —
    // the fast path only populates m[0], leaving m[1..9] unset.
    if (re->ncaptures > 1) return 0;

    struct state *s = re->start;
    size_t len = 0;

    // Skip initial save state
    if (is_save(s)) s = s->out1;

    // Walk state chain, collecting literal characters
    while (s && !is_match(s) && len < MAX_LITERAL - 4) {
        if (s->type == ST_CHAR) {
            if (!append_literal_cp(re, s->val, &len)) break;
            s = s->out1;
        } else if (is_save(s)) {
            s = s->out1;
        } else {
            break;
        }
    }

    // Must end at save->match or match
    if (is_save(s)) s = s->out1;
    if (!is_match(s)) return 0;

    re->literal[len] = '\0';
    re->literal_len = len;
    re->is_literal = (len > 0);

    if (re->is_literal && len > 1) build_bmh_table(re, len);

    return re->is_literal;
}

// Boyer-Moore-Horspool: compare a char against pattern char, return 1 if match.
// Used as callback below — one for exact match, one for case-insensitive.
static int
bmh_eq(unsigned char a, unsigned char b)          { return a == b; }
static int
bmh_eq_icase(unsigned char a, unsigned char b)    { return to_lower(a) == to_lower(b); }

static const char *
bmh_search(const char *text, size_t n, const char *pat, size_t m,
           const size_t *skip, int (*eq)(unsigned char, unsigned char)) {
    size_t i = 0;
    size_t last = m - 1;
    while (i <= n - m) {
        size_t j = last;
        while (j < m && eq(text[i + j], pat[j])) {
            if (j == 0) return text + i;
            j--;
        }
        i += skip[(unsigned char)text[i + last]];
    }
    return NULL;
}

static const char *
literal_search(struct regex *re, const char *text, size_t n) {
    size_t m = re->literal_len;
    if (m == 0) return text;
    if (n < m) return NULL;

    const char *pat = re->literal;

    // Single char: use memchr (or linear scan for icase)
    if (m == 1) {
        if (re->icase) {
            int lo = to_lower((unsigned char)pat[0]);
            for (size_t i = 0; i < n; i++)
                if (to_lower((unsigned char)text[i]) == lo) return text + i;
            return NULL;
        }
        return memchr(text, pat[0], n);
    }

    // Boyer-Moore-Horspool with appropriate comparison
    int (*cmp)(unsigned char, unsigned char) = re->icase ? bmh_eq_icase : bmh_eq;
    const size_t *skip = re->literal_skip;
    return bmh_search(text, n, pat, m, skip, cmp);
}

// ============================================================================
// Compile
// ============================================================================

// Flags string parser: combine as needed (e, i, m, s, u)
static void
parse_flags(struct regex *re, const char *flags) {
    if (!flags) return;
    for (; *flags; flags++) {
        switch (*flags) {
        case 'e': re->ere = 1; break;
        case 'i': re->icase = 1; break;
        case 'm': re->multiline = 1; break;
        case 's': re->dotall = 1; break;
        case 'u': re->utf8 = 1; break;
        }
    }
}

// Auto-enable UTF-8 if pattern contains high codepoints
static void
detect_utf8_needed(struct regex *re) {
    for (int i = 0; i < re->nstates; i++) {
        struct state *s = &re->pool[i];
        if (s->type == ST_CHAR && s->val > 127) { re->utf8 = 1; return; }
        if ((s->type == ST_CLASS || s->type == ST_NCLASS) && s->ncpranges > 0) { re->utf8 = 1; return; }
    }
}

struct regex *
re_compile(const char *pattern, const char *flags) {
    if (!pattern || !*pattern) {
        struct regex *re = calloc(1, sizeof *re);
        if (re) {
            re->err = 1;
            snprintf(re->errmsg, sizeof(re->errmsg), "empty pattern");
        }
        return re;
    }

    struct regex *re = calloc(1, sizeof *re);
    if (!re) return NULL;

    // Allocate state pool dynamically
    re->pool = calloc(MAX_STATES, sizeof(struct state));
    if (!re->pool) { free(re); return NULL; }
    re->maxstates = MAX_STATES;

    // Sam-compatible defaults: ERE + multiline
    re->ere = 1;
    re->multiline = 1;

    parse_flags(re, flags);

    struct state *cap0 = make_save(re, 0);
    if (!cap0) { free(re->pool); free(re); return NULL; }

    struct parser ps = { .re = re, .p = pattern, .group = 1 };
    struct frag body = parse_regex(&ps);

    if (has_error(re)) return re;
    if (!at_end(&ps)) { fail(re, "unexpected character", ps.p); return re; }

    struct state *cap1 = make_save(re, 1);
    struct state *accept = make_match(re);
    if (!cap1 || !accept) return re;

    cap0->out1 = body.start ? body.start : cap1;
    if (body.start) frag_connect(&body, cap1);
    cap1->out1 = accept;

    re->start = cap0;
    re->ncaptures = ps.group;

    // Auto-enable UTF-8 if pattern uses high codepoints
    detect_utf8_needed(re);

    // Try to extract literal for fast path
    extract_literal(re);

    return re;
}

// ============================================================================
// NFA Execution - Character Matching
// ============================================================================

static int
char_matches(struct state *s, int cp, struct regex *re) {
    switch (s->type) {
    case ST_CHAR:   return uses_icase(re) ? to_lower(cp) == to_lower(s->val) : cp == s->val;
    case ST_ANY:    return uses_dotall(re) || !is_newline(cp);
    case ST_CLASS:  return class_has_cp(s, cp);
    case ST_NCLASS: return !class_has_cp(s, cp);
    default:        return 0;
    }
}

static int
can_match_char(struct state *s, int c, struct regex *re) {
    if (!s) return 0;
    switch (s->type) {
    case ST_CHAR: case ST_ANY: case ST_CLASS: case ST_NCLASS:
        return char_matches(s, c, re);
    case ST_SPLIT:
        return can_match_char(s->out1, c, re) || can_match_char(s->out2, c, re);
    case ST_SAVE: case ST_BOL: case ST_EOL: case ST_WBOUND: case ST_NWBOUND:
        return can_match_char(s->out1, c, re);
    default:
        return 0;
    }
}

static int
at_word_boundary(const char *p, struct regex *re) {
    int before, after;
    if (uses_utf8(re)) {
        int cp;
        if (p > re->text) {
            // Find start of previous codepoint
            const char *prev = p - 1;
            while (prev > re->text && (*prev & 0xC0) == 0x80) prev--;
            utf8_decode(prev, p, &cp);
            before = is_word_cp(cp);
        } else {
            before = 0;
        }
        if (within_text(p, re)) {
            utf8_decode(p, re->text_end, &cp);
            after = is_word_cp(cp);
        } else {
            after = 0;
        }
    } else {
        before = (p > re->text) && is_word((unsigned char)p[-1]);
        after = within_text(p, re) && is_word((unsigned char)*p);
    }
    return before != after;
}

#ifdef RE_BACKREF
static int
backref_matches(struct state *s, const char *p, struct regex *re, const char **cap) {
    int n = s->val;
    const char *start = cap[n * 2];
    const char *end = cap[n * 2 + 1];
    if (!start || !end) return 0;
    size_t len = end - start;
    if (p + len > re->text_end) return 0;
    if (uses_icase(re)) {
        for (size_t i = 0; i < len; i++)
            if (to_lower((unsigned char)p[i]) != to_lower((unsigned char)start[i])) return 0;
    } else {
        if (memcmp(p, start, len) != 0) return 0;
    }
    return len;
}
#endif  /* RE_BACKREF */

// ============================================================================
// NFA Execution - Thread Management
// ============================================================================

static const char *advance_cp(const char *p, const char *end, int utf8);

#ifdef RE_LOOKAROUND
static int run_subnfa(struct regex *re, struct state *start, const char *p);
#endif  /* RE_LOOKAROUND */

static void spawn(struct thread *, int *, struct state *,
                  const char **, const char *, struct regex *);

static void
spawn_split(struct thread *list, int *n, struct state *s,
            const char **cap, const char *p, struct regex *re) {
    if (is_possessive(s->quant)) {
        int c = within_text(p, re) ? (unsigned char)*p : -1;
        int can = (c >= 0) && can_match_char(s->out1, c, re);
        spawn(list, n, can ? s->out1 : s->out2, cap, p, re);
    } else {
        spawn(list, n, s->out1, cap, p, re);
        spawn(list, n, s->out2, cap, p, re);
    }
}

static void
spawn_capture(struct thread *list, int *n, struct state *s,
              const char **cap, const char *p, struct regex *re) {
    const char *newcap[MAX_CAPTURES * 2];
    memcpy(newcap, cap, sizeof newcap);
    newcap[s->val] = p;
    spawn(list, n, s->out1, newcap, p, re);
}

static void
spawn_anchor(struct thread *list, int *n, struct state *s,
             const char **cap, const char *p, struct regex *re) {
    switch (s->type) {
    case ST_BOL:
        if (at_text_start(p, re) || (uses_multiline(re) && after_newline(p, re)))
            spawn(list, n, s->out1, cap, p, re);
        break;

    case ST_EOL:
        if (at_text_end(p, re) || (uses_multiline(re) && at_newline(p, re)))
            spawn(list, n, s->out1, cap, p, re);
        break;

    case ST_BOS:
        if (at_text_start(p, re)) spawn(list, n, s->out1, cap, p, re);
        break;

    case ST_EOS:
        if (at_text_end(p, re)) spawn(list, n, s->out1, cap, p, re);
        break;

    case ST_EOSB:
        if (at_text_end(p, re) || (at_newline(p, re) && p + 1 == re->text_end))
            spawn(list, n, s->out1, cap, p, re);
        break;
    }
}

static void
spawn_boundary(struct thread *list, int *n, struct state *s,
               const char **cap, const char *p, struct regex *re) {
    if (is_wbound(s) && at_word_boundary(p, re)) {
        spawn(list, n, s->out1, cap, p, re);
    } else if (is_nwbound(s) && !at_word_boundary(p, re)) {
        spawn(list, n, s->out1, cap, p, re);
    }
}

static void
spawn_nop(struct thread *list, int *n, struct state *s,
          const char **cap, const char *p, struct regex *re) {
    spawn(list, n, s->out1, cap, p, re);
}

#ifdef RE_LOOKAROUND
static void
spawn_lookaround(struct thread *list, int *n, struct state *s,
                 const char **cap, const char *p, struct regex *re) {
    if (is_lahead_pos(s)) {
        if (run_subnfa(re, s->look, p)) spawn(list, n, s->out1, cap, p, re);
    } else if (is_lahead_neg(s)) {
        if (!run_subnfa(re, s->look, p)) spawn(list, n, s->out1, cap, p, re);
    } else if (is_lbehind_pos(s)) {
        const char *back = p - s->look_width;
        if (back >= re->text && run_subnfa(re, s->look, back))
            spawn(list, n, s->out1, cap, p, re);
    } else if (is_lbehind_neg(s)) {
        const char *back = p - s->look_width;
        if (!(back >= re->text && run_subnfa(re, s->look, back)))
            spawn(list, n, s->out1, cap, p, re);
    }
}
#endif  /* RE_LOOKAROUND */

static void
spawn(struct thread *list, int *n, struct state *s,
      const char **cap, const char *p, struct regex *re) {
    if (!s) return;
    if (threads_full(*n)) { re->overflow = 1; return; }

    if (is_split(s))          { spawn_split(list, n, s, cap, p, re); return; }
    if (is_save(s))           { spawn_capture(list, n, s, cap, p, re); return; }
    if (is_anchor(s))         { spawn_anchor(list, n, s, cap, p, re); return; }
    if (is_boundary(s))       { spawn_boundary(list, n, s, cap, p, re); return; }
    if (is_nop(s))            { spawn_nop(list, n, s, cap, p, re); return; }
#ifdef RE_LOOKAROUND
    if (is_lookaround(s))     { spawn_lookaround(list, n, s, cap, p, re); return; }
#endif

    // Default: add thread to active list
    struct thread *t = &list[*n];
    t->st = s;
    memcpy(t->cap, cap, sizeof t->cap);
    (*n)++;
}

// Consume one character: decode codepoint, spawn next-threads from curr into next.
// Returns number of threads spawned (0 = no survivors). Does NOT swap or advance p.
static void
decode_current_char(const char *p, struct regex *re, int *cp, int *cplen) {
    *cp = 0;
    *cplen = 1;
    if (!within_text(p, re)) return;
    if (uses_utf8(re)) {
        *cplen = utf8_decode(p, re->text_end, cp);
    } else {
        *cp = (unsigned char)*p;
    }
}

static int
step_threads(struct thread *curr, int ncurr,
            struct thread *next, const char *p, struct regex *re) {
    int nnext = 0;

    int cp, cplen;
    decode_current_char(p, re, &cp, &cplen);

    for (int i = 0; i < ncurr; i++) {
        struct thread *t = &curr[i];
        struct state *s = t->st;

        if (is_match(s)) continue;

#ifdef RE_BACKREF
        if (is_backref(s) && within_text(p, re)) {
            int len = backref_matches(s, p, re, t->cap);
            if (len > 0) spawn(next, &nnext, s->out1, t->cap, p + len, re);
            continue;
        }
#endif  /* RE_BACKREF */

        if (consumes_char(s) && within_text(p, re)) {
            if (char_matches(s, cp, re)) {
                spawn(next, &nnext, s->out1, t->cap, p + cplen, re);
            }
        }
    }

    return nnext;
}

// ============================================================================
// NFA Execution - Main Loop (with double-buffering)
// ============================================================================

// Advance pointer by one codepoint (UTF-8 aware)
static const char *
advance_cp(const char *p, const char *end, int utf8) {
    if (p >= end) return p;

    if (!utf8) return p + 1;

    int cp;
    int len = utf8_decode(p, end, &cp);
    return p + len;
}

// Advance one step: consume a character, swap buffers, advance position.
// Returns 1 if more threads exist and position is valid.
static int
nfa_step(struct thread **curr, int *ncurr, struct thread **next,
         const char **p, struct regex *re) {
    int nnext = step_threads(*curr, *ncurr, *next, *p, re);

    if (no_threads(nnext)) return 0;

    struct thread *tmp = *curr;
    *curr = *next;
    *next = tmp;
    *ncurr = nnext;
    *p = advance_cp(*p, re->text_end, re->utf8);

    return !past_text_end(*p, re);
}

// Run threads from start state at position p.
// On match, populate match_cap with capture positions.
// If stop_on_first, return immediately on first match.
// Returns 1 if match found, 0 otherwise.
static int
run_nfa_loop(struct regex *re, struct state *start, const char *p,
             const char **match_cap, int stop_on_first) {
    struct thread buf1[MAX_THREADS], buf2[MAX_THREADS];
    struct thread *curr = buf1, *next = buf2;
    int ncurr = 0;

    const char *cap[MAX_CAPTURES * 2] = {0};
    spawn(curr, &ncurr, start, cap, p, re);

    while (!no_threads(ncurr)) {
        for (int i = 0; i < ncurr; i++) {
            if (is_match(curr[i].st)) {
                memcpy(match_cap, curr[i].cap,
                       sizeof(const char *) * MAX_CAPTURES * 2);
                if (stop_on_first) return 1;
                break;
            }
        }

        if (!nfa_step(&curr, &ncurr, &next, &p, re)) break;
    }

    for (int i = 0; i < ncurr; i++) {
        if (is_match(curr[i].st)) {
            memcpy(match_cap, curr[i].cap,
                   sizeof(const char *) * MAX_CAPTURES * 2);
            return 1;
        }
    }
    return 0;
}

static int
run_nfa(struct regex *re, const char *start, struct rematch *m, int nm) {
    const char *match_cap[MAX_CAPTURES * 2] = {0};

    int found = run_nfa_loop(re, re->start, start, match_cap, re->has_lazy);

    if (found && m) {
        int n = nm < re->ncaptures ? nm : re->ncaptures;

        for (int i = 0; i < n; i++) {
            m[i].start = match_cap[i * 2];
            m[i].end = match_cap[i * 2 + 1];
        }
    }
    return found;
}

#ifdef RE_LOOKAROUND
static int
run_subnfa(struct regex *re, struct state *start, const char *p) {
    if (re->look_depth >= MAX_LOOK_DEPTH) { re->overflow = 1; return 0; }
    re->look_depth++;

    const char *match_cap[MAX_CAPTURES * 2] = {0};
    int found = run_nfa_loop(re, start, p, match_cap, 1);

    re->look_depth--;
    return found;
}
#endif  /* RE_LOOKAROUND */

// ============================================================================
// Public API
// ============================================================================

int
re_match(struct regex *re, const char *input, struct rematch *m, int nm) {
    if (!re || has_error(re)) return 0;

    re->overflow = 0;
    re->text = input;
    re->text_end = input + strlen(input);

    int r = run_nfa(re, input, m, nm);
    return has_overflow(re) ? 0 : r;
}

// Search for a match starting from `from`, iterating forward codepoint by codepoint.
// If `advance_zero`, prevent infinite loop when match is at same position as start.
static int
nfa_search_from(struct regex *re, const char *from, struct rematch *m, int nm,
                int advance_zero) {
    for (const char *p = from; p <= re->text_end;
         p = advance_cp(p, re->text_end, re->utf8)) {
        if (run_nfa(re, p, m, nm)) {
            if (has_overflow(re)) return 0;

            re->next_search = (m && m[0].end)
                ? m[0].end
                : advance_cp(p, re->text_end, re->utf8);

            if (advance_zero && re->next_search == from)
                re->next_search = advance_cp(from, re->text_end, re->utf8);

            return 1;
        }

        if (has_overflow(re)) return 0;
        if (p == re->text_end) break;
    }
    return 0;
}

// Fast path: literal pattern uses Boyer-Moore. Sets next_search, returns found flag.
// Always handles no-zero-advance internally.
static int
literal_fastpath(struct regex *re, const char *input, size_t len,
                 struct rematch *m, int nm, int advance_zero) {
    const char *found = literal_search(re, input, len);
    if (!found) return 0;

    if (m && nm > 0) {
        m[0].start = found;
        m[0].end = found + re->literal_len;
    }

    re->next_search = found + re->literal_len;

    if (advance_zero && re->next_search == input)
        re->next_search = advance_cp(input, re->text_end, re->utf8);

    return 1;
}

int
re_search(struct regex *re, const char *input, struct rematch *m, int nm) {
    if (!re || has_error(re)) return 0;
    re->overflow = 0;
    re->text = input;
    re->text_end = input + strlen(input);

    if (re->is_literal)
        return literal_fastpath(re, input, re->text_end - input, m, nm, 0);

    return nfa_search_from(re, input, m, nm, 0);
}

int
re_next(struct regex *re, struct rematch *m, int nm) {
    if (!re || !re->next_search || has_overflow(re)) return 0;
    const char *start = re->next_search;
    if (past_text_end(start, re)) return 0;

    if (re->is_literal)
        return literal_fastpath(re, start, re->text_end - start, m, nm, 1);

    return nfa_search_from(re, start, m, nm, 1);
}

void
re_free(struct regex *re) {
    if (re) {
        free(re->pool);
        free(re);
    }
}

const char *
re_error(struct regex *re) {
    if (!re) return "out of memory";
    if (has_error(re)) return re->errmsg;
    if (has_overflow(re)) return "thread limit exceeded (input too complex for pattern)";
    return NULL;
}

int
re_find(const char *pattern, const char *input, struct rematch *m, int nm, const char *flags) {
    struct regex *re = re_compile(pattern, flags);
    if (!re) return 0;
    int found = re_search(re, input, m, nm);
    re_free(re);
    return found;
}

// ============================================================================
// Test Suite
// ============================================================================

#ifdef TEST_REGEX
#include <stdio.h>

static int tests = 0, passed = 0;

static void
test(const char *pat, const char *input, const char *flags, int expect, const char *want) {
    tests++;
    struct rematch m[10] = {0};
    int found = re_find(pat, input, m, 10, flags);

    char got[256] = "";
    if (found && m[0].start && m[0].end) {
        int len = m[0].end - m[0].start;
        snprintf(got, sizeof got, "%.*s", len, m[0].start);
    }

    int ok = (found == expect) && (!expect || !want || strcmp(got, want) == 0);
    passed += ok;

    printf("%s: /%s/ =~ \"%s\"", ok ? "PASS" : "FAIL", pat, input);
    if (found && m[0].start) printf(" -> \"%s\"", got);
    if (!ok && want) printf(" (expected \"%s\")", want);
    printf("\n");
}

int
main(void) {
    printf("=== Literals (fast path) ===\n");
    test("hello", "hello world", NULL, 1, "hello");
    test("world", "hello world", NULL, 1, "world");
    test("xyz", "hello world", NULL, 0, NULL);
    test("needle", "haystack with needle inside", NULL, 1, "needle");

    printf("\n=== Any char ===\n");
    test("h.llo", "hello", NULL, 1, "hello");

    printf("\n=== Anchors ===\n");
    test("^hello", "hello world", NULL, 1, "hello");
    test("^world", "hello world", NULL, 0, NULL);
    test("world$", "hello world", NULL, 1, "world");
    test("hello$", "hello world", NULL, 0, NULL);

    printf("\n=== Quantifiers ===\n");
    test("ab*c", "ac", NULL, 1, "ac");
    test("ab*c", "abc", NULL, 1, "abc");
    test("ab*c", "abbbc", NULL, 1, "abbbc");
    test("ab+c", "ac", NULL, 0, NULL);
    test("ab+c", "abc", NULL, 1, "abc");
    test("ab?c", "ac", NULL, 1, "ac");
    test("ab?c", "abc", NULL, 1, "abc");

    printf("\n=== Character Classes ===\n");
    test("[abc]", "b", NULL, 1, "b");
    test("[abc]", "d", NULL, 0, NULL);
    test("[a-z]", "m", NULL, 1, "m");
    test("[^abc]", "d", NULL, 1, "d");

    printf("\n=== Shorthand Classes ===\n");
    test("\\d+", "abc123def", NULL, 1, "123");
    test("\\w+", "hello world", NULL, 1, "hello");
    test("\\s+", "   x", NULL, 1, "   ");

    printf("\n=== Word Boundary ===\n");
    test("\\bword\\b", "a word here", NULL, 1, "word");
    test("\\bword\\b", "awordhere", NULL, 0, NULL);

    printf("\n=== Groups ===\n");
    test("(ab)+", "abab", "e", 1, "abab");
    test("cat|dog", "dog", "e", 1, "dog");
    test("cat|dog", "cat", "e", 1, "cat");

    printf("\n=== Bounded Repetition ===\n");
    test("a{3}", "aaa", "e", 1, "aaa");
    test("a{3}", "aa", "e", 0, NULL);
    test("a{2,4}", "aaaaa", "e", 1, "aaaa");
    test("a{2,}", "aaaaa", "e", 1, "aaaaa");

    printf("\n=== Flags ===\n");
    test("hello", "HELLO", "i", 1, "HELLO");
    test("^foo", "bar\nfoo", "m", 1, "foo");
    test("a.b", "a\nb", "s", 1, "a\nb");

    printf("\n=== Lazy Quantifiers ===\n");
    test("a+?", "aaa", "e", 1, "a");
    test("<.*?>", "<a><b>", "e", 1, "<a>");

    printf("\n=== Possessive Quantifiers ===\n");
    test("a*+", "aaa", "e", 1, "aaa");
    test("a*+a", "aaa", "e", 0, NULL);
    test("a++a", "aaa", "e", 0, NULL);
    test("\"[^\"]*+\"", "\"hello\"", "e", 1, "\"hello\"");

    printf("\n=== Hex Escapes ===\n");
    test("\\x41\\x42\\x43", "ABC", "e", 1, "ABC");
    test("\\x61", "a", "e", 1, "a");

    printf("\n=== Unicode Escapes ===\n");
    test("\\u{0041}", "A", "e", 1, "A");
    test("caf\\u{00E9}", "café", "e", 1, "café");

    printf("\n=== String Anchors ===\n");
    test("\\Afoo", "foo bar", "e", 1, "foo");
    test("\\Afoo", "bar foo", "e", 0, NULL);
    test("bar\\z", "foo bar", "e", 1, "bar");
    test("foo\\z", "foo bar", "e", 0, NULL);
    test("bar\\Z", "foo bar", "e", 1, "bar");
    test("bar\\Z", "foo bar\n", "e", 1, "bar");

    printf("\n=== UTF-8 Mode ===\n");
    test(".", "é", "u", 1, "é");
    test("..", "éà", "u", 1, "éà");
    test("\\bfoo\\b", "foo bar", "u", 1, "foo");

    printf("\n=== Unicode Character Classes ===\n");
    test("[\\u{00E0}-\\u{00FF}]", "é", "eu", 1, "é");
    test("[\\u{0400}-\\u{04FF}]+", "привет", "eu", 1, "привет");

    printf("\n=== Unicode \\w ===\n");
    test("\\w+", "café", "eu", 1, "café");
    test("\\w+", "naïve", "eu", 1, "naïve");
    test("\\w+", "Ωmega", "eu", 1, "Ωmega");
    test("\\w+", "Москва", "eu", 1, "Москва");

#ifdef RE_LOOKAROUND
    printf("\n=== Lookahead ===\n");
    test("foo(?=bar)", "foobar", "e", 1, "foo");
    test("foo(?!bar)", "foobaz", "e", 1, "foo");

    printf("\n=== Lookbehind ===\n");
    test("(?<=foo)bar", "foobar", "e", 1, "bar");
    test("(?<!foo)bar", "bazbar", "e", 1, "bar");
#endif  /* RE_LOOKAROUND */

#ifdef RE_BACKREF
    printf("\n=== Backreferences ===\n");
    test("(\\w+) \\1", "hello hello", "e", 1, "hello hello");
    test("(\\w+) \\1", "hello world", "e", 0, NULL);
#endif  /* RE_BACKREF */

    printf("\n=== Alternation (covers 2-output frag fix) ===\n");
    test("a?|b", "a", "e", 1, "a");
    test("a?|b", "b", "e", 1, "b");
    test("a?|b", "", "e", 1, "");
    test("a?|b", "c", "e", 1, "");
    test("a*|b", "aaa", "e", 1, "aaa");
    test("a*|b", "", "e", 1, "");
    test("(foo)?|bar", "foo", "e", 1, "foo");
    test("(foo)?|bar", "bar", "e", 1, "bar");
    test("a?b?|c", "a", "e", 1, "a");
    test("a?b?|c", "b", "e", 1, "b");
    test("a?b?|c", "ab", "e", 1, "ab");
    test("a{0,2}|b", "aa", "e", 1, "aa");
    test("a{0,2}|b", "b", "e", 1, "b");

    printf("\n=== re_next (iterative search) ===\n");
    {
        struct regex *re = re_compile("a", "e");
        struct rematch m[1];
        int n = 0;
        const char *s = "aba";
        if (re_search(re, s, m, 1)) {
            n++;
            while (re_next(re, m, 1)) { n++; }
        }
        printf("%s: re_next on \"aba\" found %d matches (expected 2)\n",
               n == 2 ? "PASS" : "FAIL", n);
        if (n == 2) passed++;
        tests++;
        re_free(re);
    }
    {
        struct regex *re = re_compile(".", "e");
        struct rematch m[1];
        int n = 0;
        const char *s = "abc";
        if (re_search(re, s, m, 1)) {
            n++;
            while (re_next(re, m, 1)) { n++; }
        }
        printf("%s: re_next on \"abc\" found %d matches (expected 3)\n",
               n == 3 ? "PASS" : "FAIL", n);
        if (n == 3) passed++;
        tests++;
        re_free(re);
    }
    {
        struct regex *re = re_compile("\\d+", "e");
        struct rematch m[1];
        int n = 0;
        const char *s = "ab12cd34ef";
        if (re_search(re, s, m, 1)) {
            n++;
            while (re_next(re, m, 1)) { n++; }
        }
        printf("%s: re_next on \"ab12cd34ef\" found %d matches (expected 2)\n",
               n == 2 ? "PASS" : "FAIL", n);
        if (n == 2) passed++;
        tests++;
        re_free(re);
    }
    {
        struct regex *re = re_compile("a", "e");
        struct rematch m[1];
        int n = 0;
        const char *s = "xyz";
        if (re_search(re, s, m, 1)) { n++; while (re_next(re, m, 1)) { n++; } }
        printf("%s: re_next on \"xyz\" found %d matches (expected 0)\n",
               n == 0 ? "PASS" : "FAIL", n);
        if (n == 0) passed++;
        tests++;
        re_free(re);
    }

    printf("\n=== Results: %d/%d passed ===\n", passed, tests);
    return passed == tests ? 0 : 1;
}
#endif  /* TEST_REGEX */
