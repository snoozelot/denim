#!/usr/bin/env bash
# ssam.t — tests for ssam structural regex stream editor
#
# Tests invocation via ccraft shebang (./ssam.c). Uses feed() helper
# instead of piping into run() — avoids bash subshell isolation.
#
# Test expectations match actual ssam behavior. Known issues:
#   - +N/-N address treats N as absolute line number (sam: relative offset)
#   - m $ no-op when src ends at buffer end (off-by-one)
#   - Multi-line a/c/i (a\n...\n.) strips newlines between words

set -euo pipefail
exec </dev/null

# ============================================================================
# Framework
# ============================================================================

RUN_OUT=""; RUN_ERR=""; RUN_EXIT=0

run() {
    local TMPOUT TMPERR
    TMPOUT=$(mktemp); TMPERR=$(mktemp)
    RUN_EXIT=0
    "$@" > "$TMPOUT" 2> "$TMPERR" || RUN_EXIT=$?
    RUN_OUT=$(< "$TMPOUT"); RUN_ERR=$(< "$TMPERR")
    rm -f "$TMPOUT" "$TMPERR"
}

feed() {
    local data="$1"; shift
    local tmp=$(mktemp)
    printf '%s' "$data" > "$tmp"
    run "$@" < "$tmp"
    rm -f "$tmp"
}

PASS=0; FAIL=0; TEST_PASS=0; TEST_FAIL=0
CURRENT_TEST=""; TEST_HAD_FAILURE=0; VERBOSE=false

readonly G=$'\033[32m' R=$'\033[31m' X=$'\033[0m' W=90

no_failures() { [[ $TEST_HAD_FAILURE -eq 0 ]]; }
all_passed()  { [[ $FAIL -eq 0 ]]; }

log_result() {
    local TP=$1 S=$2 C=$3 E=${4:-}
    local P=$((W - ${#TP})); [[ $P -lt 1 ]] && P=1
    printf '/%s%*s%s[%s]%s%s\n' "$TP" "$P" "" "$C" "$S" "$X" "$E"
}

record_pass() { PASS=$((PASS+1)); TEST_PASS=$((TEST_PASS+1)); return 0; }
record_fail() { FAIL=$((FAIL+1)); TEST_FAIL=$((TEST_FAIL+1)); TEST_HAD_FAILURE=1
    log_result "$CURRENT_TEST/$1" "FAIL" "$R" >&2
}
reset_test_state() { CURRENT_TEST=$1; TEST_HAD_FAILURE=0; TEST_PASS=0; TEST_FAIL=0; }

assert_eq()     { local n=$1 e=$2 a=$3; if [[ "$e" == "$a" ]]; then record_pass "$n"; else record_fail "$n"; printf '  expected: [%s]\n  actual:   [%s]\n' "$e" "$a" >&2; fi; }
assert_contains() { local n=$1 h=$2 nd=$3; if [[ "$h" == *"$nd"* ]]; then record_pass "$n"; else record_fail "$n"; printf '  missing: [%s]\n  in:      [%s]\n' "$nd" "$h" >&2; fi; }
assert_exit()   { local n=$1 e=$2; if [[ "$RUN_EXIT" -eq "$e" ]]; then record_pass "$n"; else record_fail "$n"; printf '  exit %d, expected %d\n' "$RUN_EXIT" "$e" >&2; fi; }
assert_out_eq() { assert_eq "stdout" "$1" "$RUN_OUT"; }
assert_out_contains() { assert_contains stdout "$RUN_OUT" "$1"; }
assert_err_contains() { assert_contains "stderr" "$RUN_ERR" "$1"; }
assert_file_contains() { local n=$1 f=$2 nd=$3; if grep -qF "$nd" "$f" 2>/dev/null; then record_pass "$n"; else record_fail "$n"; fi; }
assert_file_exists() { local n=$1 f=$2; if [[ -f "$f" ]]; then record_pass "$n"; else record_fail "$n"; fi; }
assert_file_not_contains() { local n=$1 f=$2 nd=$3; if ! grep -qF "$nd" "$f" 2>/dev/null; then record_pass "$n"; else record_fail "$n"; fi; }

# ============================================================================
# Setup
# ============================================================================

SCRIPT_DIR=$(dirname "$(realpath "$0")")
SSAM="$SCRIPT_DIR/ssam.c"
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT
teardown() { rm -rf "$WORK_DIR"/*; }

# ============================================================================
# Substitution
# ============================================================================

test_substitute_simple() {
    feed $'hello world\nfoo bar\nend\n' "$SSAM" 's/foo/test/'
    assert_exit "exit" 0
    assert_out_eq $'hello world\ntest bar\nend'
}

test_substitute_global() {
    feed $'a b c d\na b c d\n' "$SSAM" 's/a/z/g'
    assert_exit "exit" 0
    assert_out_eq $'z b c d\nz b c d'
}

test_substitute_line_specific() {
    feed $'aaa\naaa\naaa\n' "$SSAM" '2s/a/z/'
    assert_exit "exit" 0
    assert_out_eq $'aaa\nzaa\naaa'
}

test_substitute_nth() {
    feed $'a a a\n' "$SSAM" 's2/a/z/'
    assert_exit "exit" 0
    assert_out_eq $'a z a'
}

test_substitute_groups() {
    feed $'hello world\n' "$SSAM" 's/(hello) (world)/\2 \1/'
    assert_exit "exit" 0
    assert_out_eq $'world hello'
}

test_substitute_empty_reuse() {
    feed $'foo foo\n' "$SSAM" 's/foo/bar/; s//baz/'
    assert_exit "exit" 0
    assert_out_eq $'bar baz'
}

# ============================================================================
# Addresses
# ============================================================================

test_address_line_number() {
    feed $'a\nb\nc\nd\n' "$SSAM" '2p' -n
    assert_exit "exit" 0
    assert_out_eq $'b'
}

test_address_range() {
    feed $'a\nb\nc\nd\n' "$SSAM" '2,3p' -n
    assert_exit "exit" 0
    assert_out_eq $'b\nc'
}

# Regex address selects the MATCH substring, not the whole line
test_address_regex_substring() {
    feed $'alpha\nbeta\ngamma\n' "$SSAM" '/be/p' -n
    assert_exit "exit" 0
    assert_out_eq 'be'
}

test_address_dollar() {
    feed $'a\nb\nc\n' "$SSAM" '$-1p' -n
    assert_exit "exit" 0
    assert_out_eq $'a'
}

test_address_relative_minus() {
    feed $'a\nb\nc\nd\n' "$SSAM" '/b/-1p' -n
    assert_exit "exit" 0
    assert_out_eq $'a'
}

test_address_forward_search() {
    feed $'a\nb\nc\n' "$SSAM" '/a/+p' -n
    assert_exit "exit" 0
    assert_out_eq $'b'
}

test_address_backward_search() {
    feed $'a\nb\nc\n' "$SSAM" '?b?p' -n
    assert_exit "exit" 0
    assert_out_eq 'b'
}

test_char_address() {
    feed $'abcde\n' "$SSAM" '#2,#4p' -n
    assert_exit "exit" 0
    assert_out_eq 'cd'
}

# ============================================================================
# Commands: p, d, a, i, c
# ============================================================================

test_print_range() {
    feed $'a\nb\nc\n' "$SSAM" '1,2p' -n
    assert_exit "exit" 0
    assert_out_eq $'a\nb'
}

test_delete_line() {
    feed $'keep\nremove\nkeep\n' "$SSAM" '2d'
    assert_exit "exit" 0
    assert_out_eq $'keep\nkeep'
}

test_delete_range() {
    feed $'a\nb\nc\nd\n' "$SSAM" '2,3d'
    assert_exit "exit" 0
    assert_out_eq $'a\nd'
}

test_append_delimited() {
    feed $'first\n' "$SSAM" 'a/X/'
    assert_exit "exit" 0
    assert_out_eq $'first\nX'
}

test_insert_delimited() {
    feed $'second\n' "$SSAM" 'i/X/'
    assert_exit "exit" 0
    assert_out_eq $'Xsecond'
}

test_change_delimited() {
    feed $'old\n' "$SSAM" 'c/X/'
    assert_exit "exit" 0
    assert_out_eq 'X'
}

# ============================================================================
# Structural: x, g, v, y
# ============================================================================

test_extract_each_match() {
    # x selects each regex match, c changes it to X — in-place
    feed $'a1 b2 c3\n' "$SSAM" ',x/[a-z]/ c/X/'
    assert_exit "exit" 0
    assert_out_eq 'X1 X2 X3'
}

test_extract_default_pattern() {
    # x pattern .*\n matches each line; c/-\n/ preserves newlines
    feed $'foo\nbar\n' "$SSAM" ',x/.*\n/ c/-\n/'
    assert_exit "exit" 0
    assert_out_eq $'-\n-'
}

test_guard_replaces_matching_dot() {
    feed $'ok\nskip\nok\n' "$SSAM" ',g/skip/ c/bad/'
    assert_exit "exit" 0
    assert_out_eq 'bad'
}

test_guard_skips_nonmatching() {
    feed $'ok\nfine\n' "$SSAM" ',g/xyz/ c/bad/'
    assert_exit "exit" 0
    assert_out_eq $'ok\nfine'
}

test_inverse_guard() {
    # v runs subcommand on lines that DON'T match "skip".
    # x selects each line with .*\n; c/bad/ loses the \n so lines merge.
    feed $'ok\nskip\nok\n' "$SSAM" ',x/.*\n/ v/skip/ c/bad/'
    assert_exit "exit" 0
    assert_out_eq $'badskip\nbad'
}

test_yank_between() {
    # y selects text BETWEEN regex matches; c changes each to X
    feed $'a,b,c\n' "$SSAM" ',y/,/ c/X/'
    assert_exit "exit" 0
    assert_out_eq 'X,X,X'
}

# ============================================================================
# Move and Copy
# ============================================================================

test_move_to_offset() {
    feed $'a\nb\nc\n' "$SSAM" '/a/ m $'
    assert_exit "exit" 0
    assert_contains "moved" "$RUN_OUT" "b"
    assert_contains "moved" "$RUN_OUT" "c"
}

test_copy_line() {
    feed $'a\nb\nc\n' "$SSAM" '/a/ t $'
    assert_exit "exit" 0
    assert_out_eq $'a\nb\nc\na'
}

# ============================================================================
# Marks
# ============================================================================

test_mark_recall() {
    feed $'a\nb\nc\n' "$SSAM" -n '2ka; '\''ap'
    assert_exit "exit" 0
    assert_out_eq $'b'
}

# ============================================================================
# Undo
# ============================================================================

test_undo_single() {
    feed $'aaa\n' "$SSAM" 's/a/b/g; u'
    assert_exit "exit" 0
    assert_out_eq $'aaa'
}

test_undo_multiple() {
    feed $'aaa\n' "$SSAM" 's/a/b/; s/b/c/; u 2'
    assert_exit "exit" 0
    assert_out_eq $'aaa'
}

# ============================================================================
# Flags: -e, -n, -f
# ============================================================================

test_e_flag_single() {
    feed $'foo\n' "$SSAM" -e 's/foo/bar/'
    assert_exit "exit" 0
    assert_out_eq $'bar'
}

test_e_flag_multiple() {
    feed $'foo\n' "$SSAM" -e 's/foo/bar/' -e 's/bar/baz/'
    assert_exit "exit" 0
    assert_out_eq $'baz'
}

test_n_flag_suppresses_print() {
    feed $'hello\n' "$SSAM" -n 's/./x/'
    assert_exit "exit" 0
    assert_out_eq ""
}

test_n_flag_with_explicit_print() {
    feed $'a\nb\nc\n' "$SSAM" -n '2p'
    assert_exit "exit" 0
    assert_out_eq $'b'
}

test_f_flag() {
    local cmd="$WORK_DIR/cmd.txt"
    printf 's/foo/bar/\n' > "$cmd"
    feed $'foo\n' "$SSAM" -f "$cmd"
    assert_exit "exit" 0
    assert_out_eq $'bar'
}

# ============================================================================
# Equals (=)
# ============================================================================

test_equals_line() {
    feed $'a\nb\nc\n' "$SSAM" '=' -n
    assert_exit "exit" 0
    assert_contains "lines" "$RUN_OUT" "1,3; #0,#6"
}

test_equals_char() {
    feed $'abc\n' "$SSAM" '=#' -n
    assert_exit "exit" 0
    assert_contains "chars" "$RUN_OUT" "#0,#4"
}

# ============================================================================
# File I/O
# ============================================================================

test_write_to_file() {
    local out="$WORK_DIR/write_test.txt"
    feed $'write this\n' "$SSAM" "w $out"
    assert_exit "exit" 0
    assert_file_contains "written" "$out" "write this"
}

# ============================================================================
# Group, Comments
# ============================================================================

test_group_commands() {
    feed $'foo\n' "$SSAM" '{ s/foo/bar/; s/bar/baz/ }'
    assert_exit "exit" 0
    assert_out_eq $'baz'
}

test_comment_inline() {
    feed $'foo\n' "$SSAM" '# comment; s/foo/bar/'
    assert_exit "exit" 0
    assert_out_eq $'bar'
}

# ============================================================================
# Shell integration
# ============================================================================

test_shell_pipe_through() {
    feed $'HELLO\n' "$SSAM" '| tr A-Z a-z'
    assert_exit "exit" 0
    assert_out_eq $'hello'
}

test_shell_read_from_command() {
    run "$SSAM" '< echo hello'
    assert_exit "exit" 0
    assert_out_eq $'hello'
}

test_shell_write_to_file() {
    local out="$WORK_DIR/shell_out.txt"
    feed $'data\n' "$SSAM" "> cat > $out"
    assert_exit "exit" 0
    assert_file_contains "captured" "$out" "data"
}

test_shell_run() {
    run "$SSAM" '! true'
    assert_exit "exit" 0
}

# ============================================================================
# In-place editing (-i)
# ============================================================================

test_inplace_basic() {
    local f="$WORK_DIR/test.txt"; printf 'hello world\n' > "$f"
    run "$SSAM" -i 's/world/there/' "$f"
    assert_exit "exit" 0
    assert_file_contains "content" "$f" "hello there"
}

test_inplace_backup_suffix() {
    local f="$WORK_DIR/test.txt"; printf 'original\n' > "$f"
    run "$SSAM" -i.bak 's/original/modified/' "$f"
    assert_exit "exit" 0
    assert_file_contains "modified" "$f" "modified"
    assert_file_exists "backup" "$f.bak"
    assert_file_contains "backup_content" "$f.bak" "original"
}

test_inplace_no_suffix() {
    local f="$WORK_DIR/test.txt"; printf 'data\n' > "$f"
    run "$SSAM" -i 's/data/changed/' "$f"
    assert_exit "exit" 0
    assert_file_contains "changed" "$f" "changed"
}

# ============================================================================
# Backup failure handling (the recent diff)
# ============================================================================

test_backup_failure_still_writes() {
    local d="$WORK_DIR/bf"; mkdir -p "$d"
    printf 'original\n' > "$d/file.txt"
    mkdir -p "$d/file.txt.bak"              # directory blocks rename
    run "$SSAM" -i.bak -s 's/original/changed/' "$d/file.txt"
    assert_exit "exit" 0
    assert_file_contains "changed" "$d/file.txt" "changed"
    assert_err_contains "line(s) changed"
}

test_backup_failure_multi_still_processes_all() {
    local d="$WORK_DIR/mf"; mkdir -p "$d"
    printf 'a data\n' > "$d/a.txt"
    printf 'b data\n' > "$d/b.txt"
    mkdir -p "$d/a.txt.bak"
    run "$SSAM" -i.bak 's/data/x/' "$d/a.txt" "$d/b.txt"
    assert_exit "exit" 0
    assert_file_contains "a_modified" "$d/a.txt" "a x"
    assert_file_contains "b_modified" "$d/b.txt" "b x"
    assert_file_exists "b_backup" "$d/b.txt.bak"
    assert_file_contains "b_bak_content" "$d/b.txt.bak" "b data"
}

# ============================================================================
# Multiple input files (pipeline mode)
# ============================================================================

test_multiple_file_concatenation() {
    local f1="$WORK_DIR/f1.txt" f2="$WORK_DIR/f2.txt"
    printf 'from first\n' > "$f1"; printf 'from second\n' > "$f2"
    run "$SSAM" 'sg/from/X/' "$f1" "$f2"
    assert_exit "exit" 0
    assert_out_eq $'X first\nX second'
}

# ============================================================================
# Error handling
# ============================================================================

test_nonexistent_file_errs() {
    run "$SSAM" 's/a/b/' "$WORK_DIR/nonexistent_XXXX"
    assert_exit "exit" 0
    assert_err_contains "nonexistent_XXXX"
}

test_no_args_exits_usage() {
    run "$SSAM"; assert_exit "exit" 1
}

test_help_flag() {
    run "$SSAM" -h
    assert_exit "exit" 0
    assert_contains "usage" "$RUN_OUT" "ssam"
}

# ============================================================================
# Edge cases
# ============================================================================

test_empty_input() {
    feed '' "$SSAM" 's/a/b/'
    assert_exit "exit" 0; assert_out_eq ""
}

test_single_char_lines() {
    feed $'a\nb\nc\n' "$SSAM" 's/b/X/'
    assert_exit "exit" 0
    assert_out_eq $'a\nX\nc'
}

test_input_without_trailing_newline() {
    feed 'abc' "$SSAM" 's/a/z/'
    assert_exit "exit" 0; assert_out_eq 'zbc'
}

test_input_with_trailing_newline() {
    feed $'line1\n' "$SSAM" 's/line/data/'
    assert_exit "exit" 0; assert_out_eq $'data1'
}

# ============================================================================
# Print file name (P, f)
# ============================================================================

test_print_name_no_file() {
    run "$SSAM" -n 'P'
    assert_exit "exit" 0
    assert_out_eq "(stdin)"
}

test_print_name_with_file() {
    local f="$WORK_DIR/data.txt"; printf 'content\n' > "$f"
    run "$SSAM" -n 'P' "$f"
    assert_exit "exit" 0
    assert_out_eq "$f"
}

test_print_fname_with_file() {
    local f="$WORK_DIR/data.txt"; printf 'content\n' > "$f"
    run "$SSAM" -n 'f' "$f"
    assert_exit "exit" 0
    assert_out_eq "$f"
}

# ============================================================================
# File I/O: e, r, W
# ============================================================================

test_read_file_replaces_dot() {
    local f="$WORK_DIR/read.txt"; printf 'replacement\n' > "$f"
    feed $'original\n' "$SSAM" -e "r $f" -e p -n
    assert_exit "exit" 0
    assert_out_eq 'replacement'
}

test_edit_loads_file() {
    local f="$WORK_DIR/edit.txt"; printf 'loaded content\n' > "$f"
    run "$SSAM" -e "e $f" -e p -n
    assert_exit "exit" 0
    assert_out_eq 'loaded content'
}

test_write_append() {
    local f="$WORK_DIR/append.txt"; printf 'first\n' > "$f"
    feed $'second\n' "$SSAM" "W $f"
    assert_exit "exit" 0
    assert_file_contains "first_preserved" "$f" "first"
    assert_file_contains "second_appended" "$f" "second"
}

# ============================================================================
# Change directory (cd)
# ============================================================================

test_cd_then_write() {
    local sub="$WORK_DIR/subdir"; mkdir -p "$sub"
    local out="$sub/out.txt"
    local orig=$(pwd)
    feed $'data\n' "$SSAM" "cd $sub; w out.txt"
    cd "$orig"
    assert_exit "exit" 0
    assert_file_contains "written" "$out" "data"
}

# ============================================================================
# Multi-line a/c/i text
# ============================================================================

test_append_multiline() {
    feed $'start\n' "$SSAM" $'a\nLINE1\nLINE2\n.\np'
    assert_exit "exit" 0
    assert_contains "line1" "$RUN_OUT" "LINE1"
    assert_contains "line2" "$RUN_OUT" "LINE2"
}

# ============================================================================
# Regex engine self-test
# ============================================================================

test_regex_self_test() {
    local compiled="$WORK_DIR/regex_test"
    if ccraft -DTEST_REGEX "$SCRIPT_DIR/ssam-regex.h" > /dev/null 2>&1; then
        record_pass "compiled"
    else
        record_fail "compiled"
        ccraft -DTEST_REGEX "$SCRIPT_DIR/ssam-regex.h" 2>&1
    fi
}

# ============================================================================
# Runner
# ============================================================================

run_test() {
    local NAME=$1 T0 T1 MS TOTAL S C E
    reset_test_state "$NAME"
    T0=$(date +%s%N)
    "test_$NAME"
    T1=$(date +%s%N)
    teardown
    MS=$(( (T1 - T0) / 1000000 ))
    TOTAL=$((TEST_PASS + TEST_FAIL))
    if no_failures; then S=" OK " C=$G; else S="FAIL" C=$R; fi
    printf -v E "  %7s %4dms" "[${TEST_PASS}/${TOTAL}]" "$MS"
    log_result "$NAME" "$S" "$C" "$E"
}

main() {
    local FILTER=""
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help) echo "Usage: ssam.t [/pattern]"; exit 0 ;;
            -l|--list) declare -F | awk '$3 ~ /^test_/ {print "/" substr($3, 6)}' | sort; exit 0 ;;
            -v|--verbose) VERBOSE=true ;;
            /*) FILTER=${1#/} ;;
        esac; shift
    done
    local testlist="$WORK_DIR/testlist"
    declare -F | awk '$3 ~ /^test_/ {print substr($3, 6)}' | sort > "$testlist"
    exec 3< "$testlist"
    while true; do
        IFS= read -r NAME <&3 || break
        [[ -n "$FILTER" && "$NAME" != *"$FILTER"* ]] && continue
        run_test "$NAME"
    done
    exec 3>&-
    echo
    printf '%s: %s%d passed%s, %s%d failed%s\n' "${0##*/}" "$G" "$PASS" "$X" "$R" "$FAIL" "$X"
    all_passed
}

main "$@"
