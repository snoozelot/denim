#!/usr/bin/env bash
# z - Jump to frecent directories
#
# A directory jumper that learns from your cd habits. It tracks directories you
# visit, ranking them by "frecency" - a combination of frequency and recency.
# Type a few characters and jump to the best matching directory instantly.
#
# WHAT IS FRECENCY?
#
# Frecency combines how often you visit a directory with how recently you visited.
# A directory visited 10 times this week scores higher than one visited 100 times
# last year. The algorithm uses exponential decay:
#
#   score = 10000 * visits * (3.75 / ((0.0001 * seconds_since_visit + 1) + 0.25))
#
# This means recent visits boost scores dramatically, but frequently visited
# directories maintain high scores even as they age.
#
# HOW IT WORKS
#
# 1. z hooks into your shell prompt (PROMPT_COMMAND)
# 2. Every time you cd, z records the directory in ~/.z with:
#    - Full path
#    - Visit count (rank)
#    - Unix timestamp of last visit
# 3. When you type "z foo", it searches the database for paths containing "foo"
# 4. It jumps to the highest-scoring match
#
# INSTALLATION
#
# Add to your ~/.bashrc:
#
#   source /path/to/z.sh
#
# Then restart your shell or run: source ~/.bashrc
#
# Use your shell normally for a while. After visiting some directories, z will
# have enough data to be useful. You can also import from other tools:
#
#   z --import ~/.z.old              # Import z/fasd/zsh-z format
#   z --import-autojump ~/.local/share/autojump/autojump.txt
#
# BASIC USAGE
#
#   z foo         Jump to most frecent directory matching "foo"
#   z foo bar     Match must contain both "foo" AND "bar" (in order)
#   z -l          List all tracked directories with scores
#   z -l foo      List directories matching "foo"
#
# PATTERN MATCHING
#
# Patterns are regular expressions joined with ".*":
#   - "z foo bar" becomes "foo.*bar" (foo followed eventually by bar)
#   - Case-insensitive if pattern is all lowercase
#   - Case-sensitive if pattern contains uppercase
#
# Examples:
#   z proj        Matches ~/projects, ~/work/project-x, /var/projections
#   z proj api    Matches ~/projects/api, ~/work/project-api
#   z Proj        Case-sensitive: ~/Projects but not ~/projects
#
# COMMON ROOT BEHAVIOR
#
# When multiple paths match, z prefers the "common root" - the shortest path
# that is a prefix of all matches. Given:
#
#   ~/projects/myapp
#   ~/projects/myapp-api
#   ~/projects/myapp-web
#
# Typing "z myapp" goes to ~/projects/myapp (the common root), not whichever
# subdirectory has highest frecency. Use "z myapp-api" to be specific.
#
# PATH PASSTHROUGH
#
# These bypass the database and go directly to cd:
#
#   z -           Previous directory (cd -)
#   z ..          Parent directory
#   z /foo        Absolute paths
#   z ./foo       Relative paths starting with . or ..
#
# SUBDIR DESCENT
#
#   z proj / src  Jump to "proj" match, then descend into src/ or src-*/
#
# The "/" separator tells z to first find "proj", then look for a subdirectory
# matching "src". Useful when "z src" would match too many directories.
#
# INTERACTIVE MODE
#
#   z -i          Pick from all directories with fzy/fzf
#   z -i foo      Pick from directories matching "foo"
#   zi foo        Shorthand (alias for z -i)
#
# Requires fzy or fzf to be installed. fzy is preferred if both exist.
#
# OPTIONS
#
#   -c    Restrict to subdirectories of current directory
#   -e    Echo path instead of cd'ing (for scripts: dir=$(z -e foo))
#   -h    Show help
#   -i    Interactive picker mode (requires fzy or fzf)
#   -l    List matches sorted by score
#   -r    Sort by rank only (visit count, ignore recency)
#   -t    Sort by time only (most recent first)
#   -x    Remove current directory from database
#
# SUBCOMMANDS
#
#   z --add PATH            Manually add path to database
#   z --remove PATH         Remove path from database
#   z --list                Show raw database contents
#   z --clean               Remove entries for deleted directories
#   z --import FILE         Import z/fasd/zsh-z format database
#   z --import-autojump F   Import autojump format database
#
# TAB COMPLETION
#
# Type "z foo<TAB>" to see matching paths. Completions strip the common prefix
# so you see distinctive suffixes only.
#
# CONFIGURATION
#
# Set these environment variables BEFORE sourcing z.sh:
#
#   _Z_DATA=/path/to/db       Database location (default: ~/.z)
#   _Z_CMD=j                  Rename command (default: z)
#   _Z_MAX_SCORE=9000         Trigger rank decay when total exceeds this
#   _Z_EXCLUDE_DIRS=(...)     Array of glob patterns to never track:
#                             _Z_EXCLUDE_DIRS=('/tmp/*' '*/node_modules/*')
#   _Z_PICKER="fzf"           Override picker command
#   _Z_HOOK=pwd               Only record on actual directory change
#   _Z_ECHO=1                 Print path before cd'ing
#   _Z_OWNER=user             For sudo with preserved $HOME
#   _Z_NO_RESOLVE_SYMLINKS    Disable symlink resolution
#   _Z_NO_PROMPT_COMMAND      Disable automatic tracking (manual --add only)
#
# DATABASE FORMAT
#
# The database (~/.z) is pipe-delimited text:
#
#   /path/to/dir|rank|timestamp
#   /home/user/projects|45|1706789012
#   /var/log|12|1706654321
#
# Ranks decay by 1% when total exceeds _Z_MAX_SCORE, preventing unbounded growth.
#
# ALSO DEFINES
#
#   zi       Alias for z -i
#   ..       cd ..
#   ...      cd ../..
#   ....     cd ../../..
#   .....    cd ../../../..
#
# Fork of rupa deadwyler's z.sh (WTFPL v2).

# =============================================================================
# Constants
# =============================================================================

DB_DEFAULT="$HOME/.z"
DECAY_THRESHOLD=9000
DECAY_FACTOR=0.99
FREC_SCALE=10000
FREC_BOOST=3.75
FREC_SMOOTH=0.0001
FREC_BASE=0.25
SCORE_MIN=-9999999999

# =============================================================================
# Predicates: File System
# =============================================================================

is_symlink()    { [ -h "$1" ]; }
is_file()       { [ -f "$1" ]; }
is_dir()        { [ -d "$1" ]; }
is_owned()      { [ -O "$1" ]; }
is_nonempty()   { [ -s "$1" ]; }

# =============================================================================
# Predicates: Strings
# =============================================================================

is_set()          { [ -n "$1" ]; }
is_empty()        { [ -z "$1" ]; }
is_root_path()    { [ "$1" = "/" ]; }
is_home_path()    { [ "$1" = "$HOME" ]; }
is_dash()         { [ "$1" = "-" ]; }
is_dotdot()       { [ "$1" = ".." ]; }
is_absolute()     { case "$1" in /*) true ;; *) false ;; esac; }
is_relative()     { case "$1" in ./* | ../*) true ;; *) false ;; esac; }

looks_like_path() { is_dash "$1" || is_dotdot "$1" || is_absolute "$1" || is_relative "$1"; }

# =============================================================================
# Predicates: Configuration
# =============================================================================

has_excludes()      { declare -p _Z_EXCLUDE_DIRS &>/dev/null && [ "${#_Z_EXCLUDE_DIRS[@]}" -gt 0 ]; }
has_owner()         { is_set "$_Z_OWNER"; }
has_fzy()           { command -v fzy >/dev/null 2>&1; }
has_fzf()           { command -v fzf >/dev/null 2>&1; }
has_complete()      { type complete >/dev/null 2>&1; }
has_path_shorten()  { command -v path-shorten >/dev/null 2>&1; }
has_path_expand()   { command -v path-expand.c >/dev/null 2>&1; }
is_shortened_path() { [[ "$1" == /* ]] && ! is_dir "$1" && has_path_expand; }
wants_echo()        { is_set "$_Z_ECHO"; }

# =============================================================================
# Predicates: Exclusion
# =============================================================================

matches_glob() { case "$1" in $2) true ;; *) false ;; esac; }

is_excluded() {
    local DIR="$1" GLOB
    for GLOB in "${_Z_EXCLUDE_DIRS[@]}"; do
        matches_glob "$DIR" "$GLOB" && return 0
    done
    return 1
}

should_skip() { is_home_path "$1" || is_root_path "$1" || { has_excludes && is_excluded "$1"; }; }

# =============================================================================
# Database: File Access
# =============================================================================

datafile() {
    local FILE="${_Z_DATA:-$DB_DEFAULT}"
    is_empty "$FILE" && FILE="$DB_DEFAULT"
    is_symlink "$FILE" && FILE=$(readlink "$FILE")
    echo "$FILE"
}

can_access_db() {
    has_owner && return 0
    is_file "$1" || return 0
    is_owned "$1"
}

emit_valid_entries() {
    local FILE="$1" LINE DIR
    is_file "$FILE" || return 0
    while read -r LINE; do
        DIR="${LINE%%|*}"
        is_dir "$DIR" && echo "$LINE"
    done < "$FILE"
}

write_data() {
    local FILE="$1" DATA="$2"
    is_empty "$DATA" && return
    printf '%s\n' "$DATA" > "$FILE"
    has_owner && chown "$_Z_OWNER:$(id -ng "$_Z_OWNER")" "$FILE"
}

remove_entry() {
    local FILE="$1" PATH="$2" DATA
    DATA=$(grep -v "^${PATH//[[\.*^$+?{}()|\\]/\\&}|" "$FILE" 2>/dev/null)
    write_data "$FILE" "$DATA"
}

# =============================================================================
# Picker: Interactive Selection
# =============================================================================

picker_cmd() {
    is_set "$_Z_PICKER" && { echo "$_Z_PICKER"; return; }
    has_fzy             && { echo "fzy"; return; }
    has_fzf             && { echo "fzf --height=40% --reverse"; return; }
}

extract_path_from_score_line() {
    echo "${1#*  }"
}

maybe_shorten_path() {
    has_path_shorten && { path-shorten; return; }
    cat
}

# =============================================================================
# AWK: Frecency Scoring
# =============================================================================

AWK_FRECENT='
function age(time, now)              { return now - time }
function frecent(rank, time, now,    a) {
    a = age(time, now)
    return int(SCALE * rank * (BOOST / ((SMOOTH * a + 1) + BASE)))
}'

# =============================================================================
# AWK: Database Update
# =============================================================================

AWK_UPDATE='
function is_target(p)                { return p == path }
function needs_decay()               { return total > threshold }
function apply_decay(r)              { return decay * r }
function store(p, r, t)              { rank[p] = r; time[p] = t }
function emit_entry(p)               { print p "|" rank[p] "|" time[p] }

BEGIN                                { store(path, 1, now) }

$2 >= 1 {
    if (is_target($1))                 store($1, $2 + 1, now)
    else                               store($1, $2, $3)
    total += $2
}

END {
    for (p in rank) {
        if (needs_decay())             rank[p] = apply_decay(rank[p])
        emit_entry(p)
    }
}'

# =============================================================================
# AWK: Tab Completion
# =============================================================================

AWK_COMPLETE='
function escape_for_shell(s) {
    gsub(/\\/, "\\\\", s); gsub(/ /, "\\ ", s); gsub(/\t/, "\\\t", s)
    gsub(/"/, "\\\"", s); gsub(/\(/, "\\(", s); gsub(/\)/, "\\)", s)
    gsub(/\[/, "\\[", s); gsub(/\]/, "\\]", s); gsub(/&/, "\\&", s)
    gsub(/;/, "\\;", s); gsub(/\|/, "\\|", s); gsub(/</, "\\<", s)
    gsub(/>/, "\\>", s); gsub(/\$/, "\\$", s); gsub(/`/, "\\`", s)
    gsub(/!/, "\\!", s); gsub(/\*/, "\\*", s); gsub(/\?/, "\\?", s)
    return s
}

function no_paths()                  { return count == 0 }
function one_path()                  { return count == 1 }
function too_shallow(depth)          { return depth <= 3 }
function too_short(depth)            { return depth < 2 }
function is_empty_string(s)          { return s == "" }
function starts_with(str, prefix)    { return substr(str, 1, length(prefix)) == prefix }
function is_nonempty_part(part)      { return part != "" }

function remove_prefix(path, prefix) {
    if (is_empty_string(prefix))     return path
    if (starts_with(path, prefix))   return substr(path, length(prefix) + 2)
    return path
}

function join_path_parts(parts, depth,    j, result) {
    for (j = 1; j <= depth; j++)
        if (is_nonempty_part(parts[j])) result = result "/" parts[j]
    return result
}

function find_common_prefix(    i, j, n, depth, parts, ref, shortest) {
    if (no_paths())                  return ""
    if (one_path()) {
        n = split(paths[1], parts, "/")
        if (too_shallow(n))          return ""
        return join_path_parts(parts, n - 2)
    }
    depth = split(paths[1], ref, "/")
    shortest = depth
    for (i = 2; i <= count; i++) {
        n = split(paths[i], parts, "/")
        if (n < shortest)            shortest = n
        for (j = 1; j <= depth; j++)
            if (j > n || parts[j] != ref[j]) { depth = j - 1; break }
    }
    if (depth >= shortest)           depth = shortest - 1
    if (too_short(depth))            return ""
    return join_path_parts(ref, depth)
}

function matches_query(path)         { return tolower(path) ~ query_lower }

BEGIN {
    gsub(/\\/, "", q)
    q = substr(q, 3)
    gsub(/ /, ".*", q)
    query_lower = tolower(q)
}

{ if (matches_query($1)) paths[++count] = $1 }

END {
    prefix = find_common_prefix()
    for (i = 1; i <= count; i++)
        print escape_for_shell(remove_prefix(paths[i], prefix))
}'

# =============================================================================
# AWK: Score and List
# =============================================================================

AWK_SCORE="$AWK_FRECENT"'
function score_by_rank(rank, time)   { return rank }
function score_by_time(rank, time)   { return time - now }
function compute_score(rank, time) {
    if (typ == "rank")               return score_by_rank(rank, time)
    if (typ == "recent")             return score_by_time(rank, time)
    return frecent(rank, time, now)
}

function no_query()                  { return q == "" }
function matches_exact(path)         { return path ~ q }
function matches_nocase(path)        { return tolower(path) ~ query_lower }
function matches_query(path)         { return no_query() || matches_exact(path) || matches_nocase(path) }

BEGIN {
    gsub(/ /, ".*", q)
    query_lower = tolower(q)
}

{ if (matches_query($1)) printf "%10d  %s\n", compute_score($2, $3), $1 }'

# =============================================================================
# AWK: Find Best Match
# =============================================================================

AWK_BEST="$AWK_FRECENT"'
function score_by_rank(rank, time)   { return rank }
function score_by_time(rank, time)   { return time - now }
function compute_score(rank, time) {
    if (typ == "rank")               return score_by_rank(rank, time)
    if (typ == "recent")             return score_by_time(rank, time)
    return frecent(rank, time, now)
}

function is_filesystem_root(path)    { return path == "/" }
function is_shorter_path(a, b)       { return length(a) < length(b) }
function is_subpath(child, parent)   { return index(child, parent) == 1 }

function find_shortest(arr,    path, shortest) {
    for (path in arr)
        if (arr[path] && (!shortest || is_shorter_path(path, shortest)))
            shortest = path
    return shortest
}

function find_common_root(arr,    path, shortest, prefix) {
    shortest = find_shortest(arr)
    if (is_filesystem_root(shortest)) return ""
    prefix = shortest "/"
    for (path in arr)
        if (arr[path] && path != shortest && !is_subpath(path, prefix))
            return ""
    return shortest
}

function matches_exact(path)         { return path ~ q }
function matches_nocase(path)        { return tolower(path) ~ query_lower }
function prefer_common_root()        { return !typ }

BEGIN {
    gsub(/ /, ".*", q)
    query_lower = tolower(q)
    best_score = MIN
    ibest_score = MIN
}

{
    score = compute_score($2, $3)
    if (matches_exact($1)) {
        exact_matches[$1] = score
        if (score > best_score) { best = $1; best_score = score }
    }
    else if (matches_nocase($1)) {
        nocase_matches[$1] = score
        if (score > ibest_score) { ibest = $1; ibest_score = score }
    }
}

END {
    if (best) {
        root = find_common_root(exact_matches)
        print (root && prefer_common_root()) ? root : best
        exit 0
    }
    if (ibest) {
        root = find_common_root(nocase_matches)
        print (root && prefer_common_root()) ? root : ibest
        exit 0
    }
    exit 1
}'

# =============================================================================
# Database Operations
# =============================================================================

db_add() {
    local FILE="$1" DIR="$2" DATA
    DATA=$(emit_valid_entries "$FILE" \
        | awk -F"|" \
            -v path="$DIR" \
            -v now="$(date +%s)" \
            -v threshold="${_Z_MAX_SCORE:-$DECAY_THRESHOLD}" \
            -v decay="$DECAY_FACTOR" \
            "$AWK_UPDATE")
    write_data "$FILE" "$DATA"
}

db_complete() {
    local FILE="$1" QUERY="$2"
    emit_valid_entries "$FILE" | awk -F"|" -v q="$QUERY" "$AWK_COMPLETE"
}

db_score() {
    local FILE="$1" TYPE="$2" QUERY="$3"
    emit_valid_entries "$FILE" \
        | awk -F"|" \
            -v now="$(date +%s)" \
            -v typ="$TYPE" \
            -v q="$QUERY" \
            -v SCALE="$FREC_SCALE" \
            -v BOOST="$FREC_BOOST" \
            -v SMOOTH="$FREC_SMOOTH" \
            -v BASE="$FREC_BASE" \
            "$AWK_SCORE"
}

db_best() {
    local FILE="$1" TYPE="$2" QUERY="$3"
    emit_valid_entries "$FILE" \
        | awk -F"|" \
            -v now="$(date +%s)" \
            -v typ="$TYPE" \
            -v q="$QUERY" \
            -v SCALE="$FREC_SCALE" \
            -v BOOST="$FREC_BOOST" \
            -v SMOOTH="$FREC_SMOOTH" \
            -v BASE="$FREC_BASE" \
            -v MIN="$SCORE_MIN" \
            "$AWK_BEST"
}

db_clean() {
    local FILE="$1" DATA
    DATA=$(emit_valid_entries "$FILE")
    write_data "$FILE" "$DATA"
}

db_import() {
    local SRC="$1" DST="$2" DATA
    DATA=$({ is_file "$DST" && cat "$DST"
             awk -F"|" '$2 >= 1 && $1 != "" { print }' "$SRC"
           } | sort -t'|' -k1,1 -k2,2rn \
             | awk -F"|" '!seen[$1]++')
    write_data "$DST" "$DATA"
}

db_import_autojump() {
    local SRC="$1" DST="$2" DATA
    DATA=$({ is_file "$DST" && cat "$DST"
             awk -F'\t' '$1 >= 1 && $2 != "" { print $2 "|" $1 "|" systime() }' "$SRC"
           } | sort -t'|' -k1,1 -k2,2rn \
             | awk -F"|" '!seen[$1]++')
    write_data "$DST" "$DATA"
}

# =============================================================================
# Actions
# =============================================================================

go_to() {
    wants_echo && echo "$1"
    builtin cd "$1"
}

glob_unexpanded() { [ "$1" = "$2*/" ]; }

find_subdir() {
    local BASE="$1" PATTERN="$2" MATCH GLOB="$BASE/$PATTERN"
    is_dir "$GLOB" && { echo "$GLOB"; return 0; }
    for MATCH in "$GLOB"*/; do
        glob_unexpanded "$MATCH" "$GLOB" && continue
        is_dir "$MATCH" && { echo "${MATCH%/}"; return 0; }
    done
    return 1
}

escape_regex() {
    printf '%s' "$1" | sed 's/[.[\*^$+?{}()|\\]/\\&/g'
}

# =============================================================================
# Commands
# =============================================================================

cmd_add() {
    local FILE="$1" DIR="$2"
    should_skip "$DIR" && return
    db_add "$FILE" "$DIR"
}

cmd_remove() {
    local FILE="$1" DIR="$2"
    remove_entry "$FILE" "$DIR"
    echo "z: removed $DIR"
}

cmd_list_raw() {
    local FILE="$1"
    is_file "$FILE" && cat "$FILE"
}

cmd_list_scored() {
    local FILE="$1" TYPE="$2" QUERY="$3" SCORE DIR
    is_file "$FILE" || return
    db_score "$FILE" "$TYPE" "$QUERY" | sort -rn \
        | while read -r SCORE DIR; do
            printf "%-10s %s\n" "$SCORE" "$(maybe_shorten_path <<< "$DIR")"
          done >&2
}

cmd_clean() {
    local FILE="$1"
    is_file "$FILE" || return
    db_clean "$FILE"
    echo "z: cleaned stale entries"
}

cmd_import() {
    local SRC="$1" DST="$2"
    is_file "$SRC" || { echo "z: not found: $SRC" >&2; return 1; }
    db_import "$SRC" "$DST"
    echo "z: imported $SRC"
}

cmd_import_autojump() {
    local SRC="$1" DST="$2"
    is_file "$SRC" || { echo "z: not found: $SRC" >&2; return 1; }
    db_import_autojump "$SRC" "$DST"
    echo "z: imported $SRC"
}

cmd_pick() {
    local FILE="$1" TYPE="$2" QUERY="$3" ECHO="$4" PICKER SELECTION
    PICKER=$(picker_cmd)
    is_empty "$PICKER" && { echo "z: install fzy or fzf" >&2; return 1; }
    is_file "$FILE"    || return
    SELECTION=$(db_score "$FILE" "$TYPE" "$QUERY" | sort -rn | $PICKER)
    is_empty "$SELECTION" && return 1
    SELECTION=$(extract_path_from_score_line "$SELECTION")
    is_set "$ECHO" && { echo "$SELECTION"; return; }
    go_to "$SELECTION"
}

cmd_jump() {
    local FILE="$1" TYPE="$2" QUERY="$3" SUBDIR="$4" ECHO="$5" RESULT
    is_file "$FILE" || return
    RESULT=$(db_best "$FILE" "$TYPE" "$QUERY") || return 1
    is_empty "$RESULT" && return 1
    if is_set "$SUBDIR"; then
        RESULT=$(find_subdir "$RESULT" "$SUBDIR") \
            || { echo "z: no subdir '$SUBDIR'" >&2; return 1; }
    fi
    is_set "$ECHO" && { echo "$RESULT"; return; }
    go_to "$RESULT"
}

show_help() {
    cat >&2 <<'EOF'
z [-cehilrtx] [pattern...] [/ subdir]

Jump:
  z foo         Jump to most frecent match
  z foo bar     Match both patterns
  z -           Previous directory
  z ..          Parent directory

Options:
  -c            Restrict to subdirs of $PWD
  -e            Echo path (don't cd)
  -h            Show this help
  -i            Interactive picker
  -l            List matches
  -r            Sort by rank only
  -t            Sort by time only
  -x            Remove $PWD from database

Subcommands:
  z --add PATH           Add to database
  z --remove PATH        Remove from database
  z --list               Show raw database
  z --clean              Remove stale entries
  z --import FILE        Import z/fasd format
  z --import-autojump F  Import autojump format
EOF
}

# =============================================================================
# Main Entry Point
# =============================================================================

_z() {
    local FILE ECHO QUERY TYPE LIST INTERACTIVE SUBDIR OPT

    FILE=$(datafile)
    can_access_db "$FILE" || return

    # Subcommands
    case "$1" in
    --help|-h)
        show_help
        return ;;
    --add)
        is_set "$2" || { echo "z: --add needs path" >&2; return 1; }
        cmd_add "$FILE" "$2"
        return ;;
    --remove)
        is_set "$2" || { echo "z: --remove needs path" >&2; return 1; }
        cmd_remove "$FILE" "$2"
        return ;;
    --list)
        cmd_list_raw "$FILE"
        return ;;
    --clean)
        cmd_clean "$FILE"
        return ;;
    --import)
        cmd_import "$2" "$FILE"
        return ;;
    --import-autojump)
        cmd_import_autojump "$2" "$FILE"
        return ;;
    --complete)
        is_nonempty "$FILE" && db_complete "$FILE" "$2"
        return ;;
    esac

    # Path passthrough (only if path exists or is special like "-" or "..")
    if [ $# -eq 1 ] && looks_like_path "$1"; then
        { is_dir "$1" || is_dash "$1"; } && { go_to "$1"; return; }
        # Try shortened path expansion
        if is_shortened_path "$1"; then
            local EXPANDED
            EXPANDED=$(path-expand.c "$1") && { go_to "$EXPANDED"; return; }
        fi
        # Fall through to database lookup
    fi

    # Parse options and arguments
    while is_set "$1"; do
        case "$1" in
        --)
            shift
            while is_set "$1"; do
                QUERY="$QUERY${QUERY:+ }$1"
                shift
            done ;;
        -*)
            OPT="${1:1}"
            while is_set "$OPT"; do
                case "${OPT:0:1}" in
                c) QUERY="^$(escape_regex "$PWD") $QUERY" ;;
                e) ECHO=1 ;;
                h) show_help; return ;;
                i) INTERACTIVE=1 ;;
                l) LIST=1 ;;
                r) TYPE="rank" ;;
                t) TYPE="recent" ;;
                x) remove_entry "$FILE" "$PWD" ;;
                esac
                OPT="${OPT:1}"
            done ;;
        /)
            is_set "$2" || { echo "z: / needs subdir" >&2; return 1; }
            SUBDIR="$2"
            shift ;;
        *)
            QUERY="$QUERY${QUERY:+ }$1" ;;
        esac
        shift
    done

    # Execute: interactive > list > jump
    is_set "$INTERACTIVE" && { cmd_pick "$FILE" "$TYPE" "$QUERY" "$ECHO"; return; }
    is_set "$LIST"        && { cmd_list_scored "$FILE" "$TYPE" "$QUERY"; return; }
    is_empty "$QUERY"     && { cmd_list_scored "$FILE" "$TYPE" ""; return; }
    cmd_jump "$FILE" "$TYPE" "$QUERY" "$SUBDIR" "$ECHO"
}

# =============================================================================
# Shell Integration
# =============================================================================

if is_dir "${_Z_DATA:-$DB_DEFAULT}"; then
    echo "ERROR: z datafile is a directory" >&2
    return 1 2>/dev/null || true
fi

alias ${_Z_CMD:-z}='_z'
alias ${_Z_CMD:-z}i='_z -i'
alias ${_Z_CMD:-z}s='_zs'

_zs() {
    local OLD="$1" NEW="$2" TARGET
    is_empty "$OLD" && { echo "zs: usage: zs OLD NEW" >&2; return 1; }
    [[ "$PWD" != *"$OLD"* ]] && { echo "zs: '$OLD' not in $PWD" >&2; return 1; }
    TARGET="${PWD/$OLD/$NEW}"
    is_dir "$TARGET" || { echo "zs: $TARGET not found" >&2; return 1; }
    go_to "$TARGET"
}
alias ..='builtin cd ..'
alias ...='builtin cd ../..'
alias ....='builtin cd ../../..'
alias .....='builtin cd ../../../..'

[ "$_Z_NO_RESOLVE_SYMLINKS" ] || _Z_RESOLVE_SYMLINKS="-P"

if has_complete; then
    complete -o nospace -C '_z --complete "$COMP_LINE"' ${_Z_CMD:-z}

    [ "$_Z_NO_PROMPT_COMMAND" ] || {
        same_dir_as_before() { [ "$_Z_PREV" = "$PWD" ]; }
        record_current_dir()  { ( _z --add "$(command pwd $_Z_RESOLVE_SYMLINKS 2>/dev/null)" & ); }

        case "${_Z_HOOK:-prompt}" in
        pwd)
            _z_hook() {
                same_dir_as_before && return
                _Z_PREV="$PWD"
                record_current_dir
            } ;;
        *)
            _z_hook() { record_current_dir; } ;;
        esac

        [[ "$PROMPT_COMMAND" == *_z_hook* ]] || \
            PROMPT_COMMAND="${PROMPT_COMMAND:+$PROMPT_COMMAND;}_z_hook"
    }
fi
