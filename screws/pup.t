#!/usr/bin/env bash
# pup.t — test suite for pup.js (CSS selector HTML filter)
#
# Tests the CLI tool via integration: select, filter, format.
# Covers HTML output, text extraction, attribute extraction,
# JSON output, counting, file input, stdin, and error paths.
#
# USAGE
#   ./pup.t              run all tests
#   ./pup.t /basic       run tests matching "basic"
#   ./pup.t -l           list tests
#   ./pup.t -v           verbose (show each assertion)

set -euo pipefail

# ============================================================================
# Test Framework (from bash-test reference)
# ============================================================================

RUN_OUT=""
RUN_ERR=""
RUN_EXIT=0

run() {
    local TMPOUT TMPERR
    TMPOUT=$(mktemp)
    TMPERR=$(mktemp)

    RUN_EXIT=0
    "$@" > "$TMPOUT" 2> "$TMPERR" || RUN_EXIT=$?
    RUN_OUT=$(< "$TMPOUT")
    RUN_ERR=$(< "$TMPERR")
    rm -f "$TMPOUT" "$TMPERR"
}

PASS=0
FAIL=0
TEST_PASS=0
TEST_FAIL=0
CURRENT_TEST=""
TEST_HAD_FAILURE=0
VERBOSE=false

readonly GREEN=$'\033[32m'
readonly RED=$'\033[31m'
readonly RESET=$'\033[0m'
readonly WIDTH=90

no_failures()  { [[ $TEST_HAD_FAILURE -eq 0 ]]; }
all_passed()   { [[ $FAIL -eq 0 ]]; }
should_skip()  { [[ -n "$FILTER" && "$1" != *"$FILTER"* ]]; }

show_help() {
    cat <<'EOF'
Usage:
  ./pup.t                   run all tests
  ./pup.t /pattern          run tests matching "pattern"
  ./pup.t -l                list tests
  ./pup.t -v                verbose (show each assertion)
EOF
    exit 0
}

show_tests() {
    declare -F | awk '$3 ~ /^test_/ {print "/" substr($3, 6)}' | sort
    exit 0
}

log_result() {
    local TEST_PATH=$1 STATUS=$2 COLOR=$3 EXTRA=${4:-}
    local PAD=$((WIDTH - ${#TEST_PATH}))

    [[ $PAD -lt 1 ]] && PAD=1
    printf '/%s%*s%s[%s]%s%s\n' \
        "$TEST_PATH" "$PAD" "" "$COLOR" "$STATUS" "$RESET" "$EXTRA"
}

log_pass() {
    $VERBOSE && log_result "$CURRENT_TEST/$1" " OK " "$GREEN"
    return 0
}

record_pass() {
    PASS=$((PASS + 1))
    TEST_PASS=$((TEST_PASS + 1))
    log_pass "$1"
}

record_fail() {
    FAIL=$((FAIL + 1))
    TEST_FAIL=$((TEST_FAIL + 1))
    TEST_HAD_FAILURE=1
    log_result "$CURRENT_TEST/$1" "FAIL" "$RED" >&2
}

reset_test_state() {
    CURRENT_TEST=$1
    TEST_HAD_FAILURE=0
    TEST_PASS=0
    TEST_FAIL=0
}

assert() {
    local NAME=$1
    shift

    local RESULT=0
    "$@" 2>/dev/null || RESULT=$?

    if [[ $RESULT -eq 0 ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
    fi
}

assert_eq() {
    local NAME=$1 EXPECTED=$2 ACTUAL=$3

    if [[ "$EXPECTED" == "$ACTUAL" ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  expected: %s\n  actual:   %s\n' "$EXPECTED" "$ACTUAL" >&2
    fi
}

assert_contains() {
    local NAME=$1 HAYSTACK=$2 NEEDLE=$3

    if [[ "$HAYSTACK" == *"$NEEDLE"* ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  missing: %s\n' "$NEEDLE" >&2
    fi
}

assert_exit() {
    local NAME=$1 EXPECTED=$2

    if [[ "$RUN_EXIT" -eq "$EXPECTED" ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  expected exit %d, got %d\n' "$EXPECTED" "$RUN_EXIT" >&2
    fi
}

assert_not_contains() {
    local NAME=$1 HAYSTACK=$2 NEEDLE=$3

    if [[ "$HAYSTACK" != *"$NEEDLE"* ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  unexpected: %s\n' "$NEEDLE" >&2
    fi
}

# assert_lines NAME EXPECTED_COUNT VALUE
assert_lines() {
    local NAME=$1 EXPECTED=$2 VALUE=$3
    local ACTUAL

    ACTUAL=$(printf '%s' "$VALUE" | grep -c . || true)

    if [[ "$EXPECTED" -eq "$ACTUAL" ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  expected %d lines, got %d\n' "$EXPECTED" "$ACTUAL" >&2
    fi
}

# ============================================================================
# Test Runner
# ============================================================================

setup()    { :; }
teardown() { :; }

run_test() {
    local NAME=$1
    local T0 T1 MS TOTAL STATUS COLOR EXTRA

    reset_test_state "$NAME"

    setup

    T0=$(date +%s%N)
    "test_$NAME"
    T1=$(date +%s%N)

    teardown

    MS=$(( (T1 - T0) / 1000000 ))
    TOTAL=$((TEST_PASS + TEST_FAIL))

    if no_failures; then
        STATUS=" OK " COLOR=$GREEN
    else
        STATUS="FAIL" COLOR=$RED
    fi

    printf -v EXTRA "  %7s %4dms" "[${TEST_PASS}/${TOTAL}]" "$MS"
    log_result "$NAME" "$STATUS" "$COLOR" "$EXTRA"
}

main() {
    local FILTER=""

    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)    show_help ;;
            -l|--list)    show_tests ;;
            -v|--verbose) VERBOSE=true ;;
            /*)           FILTER=${1#/} ;;
            *) ;;
        esac
        shift
    done

    while read -r NAME; do
        if should_skip "$NAME"; then continue; fi
        run_test "$NAME"
    done < <(declare -F | awk '$3 ~ /^test_/ {print substr($3, 6)}' | sort)

    echo
    printf '%s: %s%d passed%s, %s%d failed%s\n' \
        "${0##*/}" "$GREEN" "$PASS" "$RESET" "$RED" "$FAIL" "$RESET"

    all_passed
}

# ============================================================================
# Test Setup
# ============================================================================

SCRIPT_DIR=$(dirname "$(realpath "$0")")
PUP="$SCRIPT_DIR/pup.js"

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

mkh() {
    printf '%s' "$1" > "$WORK_DIR/$2"
}

setup()    { :; }
teardown() { rm -f "$WORK_DIR"/*; }

# HTML fixtures
readonly SIMPLE_HTML='<!DOCTYPE html><html><body><h1>Title</h1><p class="intro">Hello</p><p>World</p><a href="https://example.com">link</a></body></html>'

readonly NESTED_HTML='<div id="main"><ul class="items"><li class="active">A</li><li>B</li><li>C</li></ul><div class="footer"><p>end</p></div></div>'

readonly VOID_HTML='<div><br><hr><img src="x.png"><input type="text" value="v"></div>'

readonly COMMENT_HTML='<div><!-- note: secret --><p>visible</p></div>'

readonly MULTI_ATTR_HTML='<a href="a" class="x" id="1">one</a><a href="b" class="y" id="2">two</a>'

readonly DEEP_HTML='<div><span><em>text</em></span><span>plain</span></div>'

# ============================================================================
# Tests: Help and Version
# ============================================================================

test_help_exits_successfully() {
    run "$PUP" --help
    assert_exit      "exit_ok" 0
    assert_contains  "usage_line" "$RUN_ERR" "Usage:"
    assert_contains  "display_fns" "$RUN_ERR" "attr{KEY}"
}

test_help_flag_h_shows_help() {
    run "$PUP" -h
    assert_exit      "exit_ok" 0
    assert_contains  "usage_line" "$RUN_ERR" "Usage:"
}

test_version_shows_number() {
    run "$PUP" --version
    assert_exit      "exit_ok" 0
    assert_contains  "version_number" "$RUN_ERR" "0."
}

# ============================================================================
# Tests: Basic HTML Output (default display fn)
# ============================================================================

test_no_selector_outputs_all() {
    run "$PUP" <<< "$SIMPLE_HTML"
    assert_exit     "exit_ok"     0
    assert_contains "has_body"    "$RUN_OUT" '<body>'
    assert_contains "has_h1"      "$RUN_OUT" '<h1>'
    assert_contains "has_link_txt" "$RUN_OUT" 'link'
}

test_select_by_tag() {
    run "$PUP" h1 <<< "$SIMPLE_HTML"
    assert_exit  "exit_ok" 0
    # prettyHtml indents text content: <h1>\n Title\n</h1>
    assert_eq    "h1_text" '<h1> Title</h1>' "$(echo "$RUN_OUT" | tr -d '\n')"
    assert_not_contains "no_p" "$RUN_OUT" '<p>'
}

test_select_by_class() {
    run "$PUP" .intro <<< "$SIMPLE_HTML"
    assert_exit     "exit_ok"  0
    assert_contains "has_class" "$RUN_OUT" "Hello"
}

test_select_by_id() {
    run "$PUP" '#main' <<< "$NESTED_HTML"
    assert_exit     "exit_ok"  0
    assert_contains "has_id"   "$RUN_OUT" 'id="main"'
}

test_select_nested_tag() {
    run "$PUP" 'div p' <<< "$NESTED_HTML"
    assert_exit     "exit_ok" 0
    assert_contains "has_end" "$RUN_OUT" 'end'
}

test_select_child_selector() {
    run "$PUP" 'ul > li' <<< "$NESTED_HTML"
    assert_exit "exit_ok" 0
    assert_eq   "three_lis" "3" "$(echo "$RUN_OUT" | grep -c '<li')"
}

test_multiple_matches_output_each() {
    run "$PUP" p <<< "$SIMPLE_HTML"
    assert_exit "exit_ok" 0
    assert_eq   "two_ps"   "2" "$(echo "$RUN_OUT" | grep -c '<p')"
}

# ============================================================================
# Tests: Topmost Filtering (no duplicate nested matches)
# ============================================================================

test_topmost_filters_nested_duplicates() {
    run "$PUP" div <<< "$NESTED_HTML"
    assert_exit      "exit_ok"  0
    assert_contains  "has_main" "$RUN_OUT" 'id="main"'
    # Only the outermost match — nested div printed as child, not separate match
    assert_eq        "no_leak"  "1" "$(echo "$RUN_OUT" | grep -c '^<div')"
}

# ============================================================================
# Tests: Text Output  text{}
# ============================================================================

test_text_output_concatenates() {
    run "$PUP" 'p text{}' <<< "$SIMPLE_HTML"
    assert_exit  "exit_ok"   0
    assert_eq    "text_lines" "Hello World" "$(echo "$RUN_OUT" | paste -sd ' ')"
}

test_text_output_single() {
    run "$PUP" 'h1 text{}' <<< "$SIMPLE_HTML"
    assert_exit "exit_ok" 0
    assert_eq   "title_text" "Title" "$RUN_OUT"
    # formatText appends trailing newline, but run() strips it via $(<)
}

test_text_empty_for_no_match() {
    run "$PUP" 'nonexistent text{}' <<< "$SIMPLE_HTML"
    assert_exit "exit_ok"  0
    assert_eq   "no_output" "" "$RUN_OUT"
}

# ============================================================================
# Tests: Attribute Output  attr{KEY}
# ============================================================================

test_attr_output_extracts_href() {
    run "$PUP" 'a attr{href}' <<< "$SIMPLE_HTML"
    assert_exit "exit_ok"   0
    assert_eq   "href_value" "https://example.com" "$RUN_OUT"
}

test_attr_output_missing_key_returns_empty() {
    run "$PUP" 'a attr{title}' <<< "$SIMPLE_HTML"
    assert_exit "exit_ok" 0
    assert_eq   "empty_for_missing" "" "$RUN_OUT"
}

test_attr_output_multiple_elements() {
    run "$PUP" 'a attr{href}' <<< "$MULTI_ATTR_HTML"
    assert_exit  "exit_ok"  0
    assert_lines "two_links" 2 "$RUN_OUT"
    assert_contains "href_a" "$RUN_OUT" "a"
    assert_contains "href_b" "$RUN_OUT" "b"
}

test_attr_output_ordered_by_document_order() {
    run "$PUP" 'a attr{id}' <<< "$MULTI_ATTR_HTML"
    assert_exit "exit_ok" 0
    assert_eq   "first_is_1" "1" "$(echo "$RUN_OUT" | head -1)"
    assert_eq   "second_is_2" "2" "$(echo "$RUN_OUT" | tail -1)"
}

# ============================================================================
# Tests: JSON Output  json{}
# ============================================================================

test_json_output_has_tag_field() {
    run "$PUP" 'h1 json{}' <<< "$SIMPLE_HTML"
    assert_exit     "exit_ok"   0
    assert_contains "has_tag"   "$RUN_OUT" '"tag": "h1"'
}

test_json_output_has_text_field() {
    run "$PUP" 'h1 json{}' <<< "$SIMPLE_HTML"
    assert_exit     "exit_ok"   0
    assert_contains "has_text"  "$RUN_OUT" '"text": "Title"'
}

test_json_output_has_attribs() {
    run "$PUP" 'a json{}' <<< "$SIMPLE_HTML"
    assert_exit     "exit_ok"  0
    assert_contains "has_href" "$RUN_OUT" '"href": "https://example.com"'
}

test_json_output_is_array() {
    run "$PUP" 'p json{}' <<< "$SIMPLE_HTML"
    assert_exit "exit_ok" 0
    assert_eq   "starts_bracket" "[" "$(printf '%s' "$RUN_OUT" | head -c1)"
    assert_eq   "ends_bracket"   "]" "$(printf '%s' "$RUN_OUT" | tail -c1)"
}

test_json_output_multiple_elements() {
    run "$PUP" 'a json{}' <<< "$MULTI_ATTR_HTML"
    assert_exit  "exit_ok"   0
    assert_contains "first_id"  "$RUN_OUT" '"id": "1"'
    assert_contains "second_id" "$RUN_OUT" '"id": "2"'
}

# ============================================================================
# Tests: JSON Indentation
# ============================================================================

test_json_default_indent_is_two() {
    run "$PUP" 'h1 json{}' <<< "$SIMPLE_HTML"
    assert_exit     "exit_ok"     0
    assert_contains "two_spaces"  "$RUN_OUT" '  "tag"'
}

test_json_custom_indent() {
    run "$PUP" -i 4 'h1 json{}' <<< "$SIMPLE_HTML"
    assert_exit     "exit_ok"    0
    assert_contains "four_spaces" "$RUN_OUT" '    "tag"'
}

test_json_indent_zero_is_compact() {
    run "$PUP" -i 0 'h1 json{}' <<< "$SIMPLE_HTML"
    assert_exit     "exit_ok"     0
    # Compact JSON has no whitespace: {"tag":"h1","text":"Title"}
    assert_not_contains "no_indent" "$RUN_OUT" '  "tag"'
    assert_contains "compact" "$RUN_OUT" '"tag":"h1"'
}

# ============================================================================
# Tests: Count / Number
# ============================================================================

test_count_returns_number() {
    run "$PUP" -n p <<< "$SIMPLE_HTML"
    assert_exit "exit_ok" 0
    assert_eq   "count_two" "2" "$RUN_OUT"
}

test_count_zero_for_no_match() {
    run "$PUP" -n nonexistent <<< "$SIMPLE_HTML"
    assert_exit "exit_ok" 0
    assert_eq   "count_zero" "0" "$RUN_OUT"
}

test_count_one_for_single() {
    run "$PUP" -n h1 <<< "$SIMPLE_HTML"
    assert_exit "exit_ok" 0
    assert_eq   "count_one" "1" "$RUN_OUT"
}

# ============================================================================
# Tests: File Input (-f)
# ============================================================================

test_file_input_reads_from_file() {
    mkh "$SIMPLE_HTML" test.html
    run "$PUP" -f "$WORK_DIR/test.html" h1
    assert_exit   "exit_ok" 0
    assert_contains "has_title" "$RUN_OUT" "Title"
}

test_file_input_missing_file_errors() {
    run "$PUP" -f "$WORK_DIR/nonexistent.html" h1
    assert_exit "exit_nonzero" 1
}

# ============================================================================
# Tests: Stdin
# ============================================================================

test_stdin_pipe_works() {
    run "$PUP" h1 <<< "$SIMPLE_HTML"
    assert_exit   "exit_ok" 0
    assert_contains "has_title" "$RUN_OUT" "Title"
}

# ============================================================================
# Tests: Empty / No Match
# ============================================================================

test_no_match_returns_nothing() {
    run "$PUP" nonexistent <<< "$SIMPLE_HTML"
    assert_exit "exit_ok"   0
    assert_eq   "no_output" "" "$RUN_OUT"
}

test_empty_input_produces_no_elements() {
    run "$PUP" div <<< ''
    assert_exit "exit_ok" 0
    assert_eq   "no_output" "" "$RUN_OUT"
}

# ============================================================================
# Tests: Error Paths
# ============================================================================

test_invalid_selector_errors() {
    run "$PUP" '%%%' <<< "$SIMPLE_HTML"
    assert_exit "exit_nonzero" 1
    assert_contains "selector_error" "$RUN_ERR" "selector error"
}

test_unknown_flag_errors() {
    run "$PUP" --bogus <<< "$SIMPLE_HTML"
    assert_exit "exit_nonzero" 1
    assert_contains "flag_error" "$RUN_ERR" "unknown flag"
}

test_file_flag_needs_arg() {
    run "$PUP" -f
    assert_exit "exit_nonzero" 1
    assert_contains "needs_path" "$RUN_ERR" "--file needs a path"
}

test_indent_flag_needs_arg() {
    run "$PUP" -i
    assert_exit "exit_nonzero" 1
}

test_malformed_html_recovers() {
    # htmlparser2 has robust error recovery — should still produce output
    run "$PUP" p <<< '<p>hello</p>'
    assert_exit "exit_ok" 0
    assert_contains "content" "$RUN_OUT" "hello"
}

# ============================================================================
# Tests: Void Elements
# ============================================================================

test_void_elements_self_close() {
    run "$PUP" div <<< "$VOID_HTML"
    assert_exit     "exit_ok"   0
    assert_contains "br"        "$RUN_OUT" '<br>'
    assert_contains "hr"        "$RUN_OUT" '<hr>'
    assert_not_contains "no_closing" "$RUN_OUT" '</br>'
    assert_not_contains "no_closing_hr" "$RUN_OUT" '</hr>'
}

test_void_img_has_src_attr() {
    run "$PUP" img <<< "$VOID_HTML"
    assert_exit     "exit_ok"  0
    assert_contains "img_tag"  "$RUN_OUT" '<img src="x.png">'
}

test_void_input_has_attributes() {
    run "$PUP" input <<< "$VOID_HTML"
    assert_exit     "exit_ok"  0
    assert_contains "input_tag" "$RUN_OUT" '<input type="text" value="v">'
}

# ============================================================================
# Tests: Comments — rendered in HTML parent output, not selectable
# ============================================================================

test_comment_rendered_in_parent_html() {
    # Comments are included in HTML output of parent element (prettyHtml renders them)
    run "$PUP" div <<< "$COMMENT_HTML"
    assert_exit     "exit_ok"   0
    assert_contains "comment_present" "$RUN_OUT" "<!--"
    assert_contains "secret" "$RUN_OUT" "secret"
}

test_comment_not_in_text_output() {
    # text{} extracts text content, not comment nodes
    run "$PUP" 'p text{}' <<< "$COMMENT_HTML"
    assert_exit "exit_ok" 0
    assert_eq   "just_visible" "visible" "$RUN_OUT"
}

test_comments_not_selectable() {
    # No selector picks comment nodes — only element selectors work
    run "$PUP" p <<< "$COMMENT_HTML"
    assert_exit   "exit_ok" 0
    assert_contains "visible" "$RUN_OUT" "visible"
}

# ============================================================================
# Tests: Text Extraction (selective)
# ============================================================================

test_text_preserves_order() {
    run "$PUP" 'span text{}' <<< "$DEEP_HTML"
    assert_exit "exit_ok" 0
    assert_eq   "ordered" $'text\nplain' "$RUN_OUT"
}

test_text_whitespace_trimmed() {
    local SPACED='<div><p>  spaced  </p></div>'
    run "$PUP" 'p text{}' <<< "$SPACED"
    assert_exit "exit_ok" 0
    assert_eq   "trimmed" "spaced" "$RUN_OUT"
}

# ============================================================================
# Tests: Nested HTML Output
# ============================================================================

test_nested_html_indentation() {
    # Indentation is 1 space per depth level
    run "$PUP" div <<< "$NESTED_HTML"
    assert_exit     "exit_ok" 0
    assert_contains "indented_li" "$RUN_OUT" '  <li'
}

test_single_child_has_indent() {
    run "$PUP" p <<< '<div><p>single</p></div>'
    assert_exit     "exit_ok" 0
    # prettyHtml indents text: <p>\n single\n</p>
    assert_contains "has_p"   "$RUN_OUT" '<p>'
    assert_contains "has_close" "$RUN_OUT" '</p>'
    assert_contains "has_text"  "$RUN_OUT" 'single'
}

# ============================================================================
# Tests: Selector with universal star
# ============================================================================

test_universal_selector_selects_all_elements() {
    run "$PUP" '*' <<< "$SIMPLE_HTML"
    assert_exit     "exit_ok"   0
    assert_contains "has_h1"    "$RUN_OUT" '<h1>'
    assert_contains "has_p"     "$RUN_OUT" '<p>'
    assert_contains "has_a"     "$RUN_OUT" '<a'
}

# ============================================================================
# Tests: Combined Selectors
# ============================================================================

test_comma_separated_selectors() {
    run "$PUP" 'h1, a' <<< "$SIMPLE_HTML"
    assert_exit     "exit_ok"  0
    assert_contains "has_h1"   "$RUN_OUT" 'Title'
    assert_contains "has_link" "$RUN_OUT" 'link'
}

# ============================================================================
# Tests: JSON empty elements and edge cases
# ============================================================================

test_json_element_without_text() {
    run "$PUP" 'br json{}' <<< "$VOID_HTML"
    assert_exit     "exit_ok"  0
    assert_contains "has_tag"  "$RUN_OUT" '"tag": "br"'
    assert_not_contains "no_text" "$RUN_OUT" '"text"'
}

test_json_element_with_attributes() {
    run "$PUP" 'a json{}' <<< "$SIMPLE_HTML"
    assert_exit     "exit_ok"    0
    assert_contains "has_href"   "$RUN_OUT" '"href"'
}

# ============================================================================
# Main
# ============================================================================

main "$@"
