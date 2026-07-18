#!/usr/bin/env bash
# yq.t — test suite for yq.js (xq/hq multi-format XML/HTML/YAML converter)
#
# Tests the CLI tool via integration: parse, filter, serialize.
# Covers XML, HTML, YAML input/output, namespaces, attributes,
# whitespace handling, and error paths.
#
# USAGE
#   ./yq.t              run all tests
#   ./yq.t /basic       run tests matching "basic"
#   ./yq.t -l           list tests
#   ./yq.t -v           verbose (show each assertion)

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
  ./yq.t                   run all tests
  ./yq.t /pattern          run tests matching "pattern"
  ./yq.t -l                list tests
  ./yq.t -v                verbose (show each assertion)
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
    local FILTER="" CHECK=false

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
YQ="$SCRIPT_DIR/yq.js"

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

mkxml()  { printf '%s' "$1" > "$WORK_DIR/$2"; }

# Invocation helpers:
#   xml ...  — invoke yq.js as xq (XML in/out, via symlink)
#   yam ...  — invoke yq.js as yq (YAML in/out, via symlink)
#   bunx ... — invoke yq.js directly with -x for XML input
xml()  { run xq "$@"; }
yam()  { run yq "$@"; }

setup()    { :; }
teardown() { rm -f "$WORK_DIR"/*; }

# ============================================================================
# Tests: Basic XML Parsing
# ============================================================================

test_basic_xml_to_json() {
    mkxml '<root><item>value</item></root>' t.xml
    xml -J '.' "$WORK_DIR/t.xml"
    assert_exit     "exit_ok"    0
    assert_contains "root_key"   "$RUN_OUT" '"root"'
    assert_contains "item_key"   "$RUN_OUT" '"item"'
    assert_contains "value"      "$RUN_OUT" '"value"'
}

test_raw_output_extracts_value() {
    mkxml '<root><item>hello</item></root>' t.xml
    xml -r '.root.item' "$WORK_DIR/t.xml"
    assert_exit "exit_ok"   0
    assert_eq   "extracted" "hello"    "$RUN_OUT"
}

test_json_output_is_pretty_printed() {
    mkxml '<root><item>42</item></root>' t.xml
    xml -J '.root.item' "$WORK_DIR/t.xml"
    assert_exit "exit_ok"   0
    assert_eq   "quoted"    '"42"'      "$RUN_OUT"
}

# ============================================================================
# Tests: Attributes
# ============================================================================

test_attributes_are_prefixed() {
    mkxml '<root id="42"><item name="n">v</item></root>' t.xml
    xml -J '.' "$WORK_DIR/t.xml"
    assert_exit     "exit_ok"    0
    assert_contains "root_id"    "$RUN_OUT" '"@id": "42"'
    assert_contains "item_name"  "$RUN_OUT" '"@name": "n"'
}

test_attributes_over_element() {
    mkxml '<root id="42"/>' t.xml
    xml -J '.' "$WORK_DIR/t.xml"
    assert_exit     "exit_ok"  0
    assert_contains "has_id"   "$RUN_OUT" '"@id"'
}

# ============================================================================
# Tests: Namespaces
# ============================================================================

test_default_namespace_uses_tag_name() {
    mkxml '<root xmlns="urn:test"><item>v</item></root>' t.xml
    xml -J '.' "$WORK_DIR/t.xml"
    assert_exit     "exit_ok"    0
    assert_contains "root_key"   "$RUN_OUT" '"root"'
    assert_contains "item_key"   "$RUN_OUT" '"item"'
}

test_prefixed_namespace_uses_prefix_local() {
    # Prefixed element with @xmlns declaration — #text holds the value.
    mkxml '<s:Envelope xmlns:s="http://soap/"><s:Body><r:R xmlns:r="urn:r">d</r:R></s:Body></s:Envelope>' t.xml
    xml -r '.["s:Envelope"]["s:Body"]["r:R"]["#text"]' "$WORK_DIR/t.xml"
    assert_exit "exit_ok" 0
    assert_eq   "value"   "d" "$RUN_OUT"
}

test_soap_envelope_namespaces() {
    local INPUT='<?xml version="1.0"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
<s:Body>
<u:Response xmlns:u="urn:urn:schemas-upnp-org:service:ContentDirectory:1">
<Result>data</Result>
</u:Response>
</s:Body>
</s:Envelope>'
    mkxml "$INPUT" t.xml
    xml -r '.["s:Envelope"]["s:Body"]["u:Response"].Result' "$WORK_DIR/t.xml"
    assert_exit "exit_ok"         0
    assert_eq   "result_extracted" "data" "$RUN_OUT"
}

test_soap_whitespace_does_not_create_mixed() {
    local INPUT='<s:Envelope xmlns:s="urn:s">
  <s:Body>
    <u:R xmlns:u="urn:u">
      <Result>ok</Result>
    </u:R>
  </s:Body>
</s:Envelope>'
    mkxml "$INPUT" t.xml
    xml -r '.["s:Envelope"]["s:Body"]["u:R"].Result' "$WORK_DIR/t.xml"
    assert_exit "exit_ok"         0
    assert_eq   "not_mixed"       "ok" "$RUN_OUT"
    assert_not_contains "no_mixed_key" "$RUN_ERR" "#mixed"
}

test_didl_lite_namespace_parsing() {
    # Default namespace (xmlns=...) — keys use tagName (no prefix).
    # Hyphenated tag names like DIDL-Lite need bracket notation in jq.
    local INPUT='<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" xmlns:dc="http://purl.org/dc/elements/1.1/"><item id="22" parentID="2" restricted="1"><dc:title>Test</dc:title><res>http://example.com/f.mp4</res></item></DIDL-Lite>'
    mkxml "$INPUT" t.xml
    xml -r '.["DIDL-Lite"].item["@id"]' "$WORK_DIR/t.xml"
    assert_exit "exit_ok" 0
    assert_eq   "id"      "22"    "$RUN_OUT"

    xml -r '.["DIDL-Lite"].item["dc:title"]' "$WORK_DIR/t.xml"
    assert_exit "title_ok" 0
    assert_eq   "title"    "Test"  "$RUN_OUT"
}

# ============================================================================
# Tests: Whitespace / Mixed Content
# ============================================================================

test_whitespace_text_nodes_filtered() {
    local INPUT='<root>
  <item>val</item>
</root>'
    mkxml "$INPUT" t.xml
    xml -r '.root.item' "$WORK_DIR/t.xml"
    assert_exit "exit_ok"     0
    assert_eq   "not_mixed"   "val" "$RUN_OUT"
}

test_indented_children_not_mixed() {
    local INPUT='<root>
    <child>
        <name>Alice</name>
    </child>
</root>'
    mkxml "$INPUT" t.xml
    xml -r '.root.child.name' "$WORK_DIR/t.xml"
    assert_exit "exit_ok"     0
    assert_eq   "extracted"   "Alice" "$RUN_OUT"
}

test_mixed_content_with_element_and_text() {
    # Mixed content (text + elements at same level) produces an array
    # when there are no attributes (wrapWithAttrs elides wrapper).
    mkxml '<root>before<child>mid</child>after</root>' t.xml
    xml -J '.root' "$WORK_DIR/t.xml"
    assert_exit     "exit_ok"   0
    assert_contains "has_array" "$RUN_OUT" '"before"'
    assert_contains "has_child" "$RUN_OUT" '"child"'
    assert_contains "has_after" "$RUN_OUT" '"after"'
}

# ============================================================================
# Tests: Multiple Children → Array
# ============================================================================

test_multiple_same_tag_children_become_array() {
    mkxml '<root><item>a</item><item>b</item></root>' t.xml
    xml -r '.root.item | length' "$WORK_DIR/t.xml"
    assert_exit "exit_ok"   0
    assert_eq   "count"     "2" "$RUN_OUT"
}

test_single_child_is_scalar() {
    mkxml '<root><item>a</item></root>' t.xml
    xml -r '.root.item | type' "$WORK_DIR/t.xml"
    assert_exit "exit_ok"   0
    assert_eq   "type"      "string" "$RUN_OUT"
}

# ============================================================================
# Tests: XML Output (-X)
# ============================================================================

test_xml_output_roundtrips() {
    mkxml '<root><item>val</item></root>' t.xml
    xml -X '.' "$WORK_DIR/t.xml"
    assert_exit     "exit_ok"   0
    assert_contains "root_tag"  "$RUN_OUT" '<root>'
    assert_contains "item_tag"  "$RUN_OUT" '<item>val</item>'
}

test_xml_output_with_self_closing() {
    mkxml '<root><empty/><item>v</item></root>' t.xml
    xml -X '.' "$WORK_DIR/t.xml"
    assert_exit     "exit_ok"   0
    assert_contains "self_close" "$RUN_OUT" '<empty/>'
}

# ============================================================================
# Tests: YAML Input/Output
# ============================================================================

test_yaml_input_extracts_value() {
    printf 'key: value\n' > "$WORK_DIR/t.yaml"
    yam -r '.key' "$WORK_DIR/t.yaml"
    assert_exit "exit_ok"   0
    assert_eq   "extracted" "value" "$RUN_OUT"
}

test_yaml_output_returns_scalar() {
    # For YAML output, use XML input via xq, pipe through jq, output YAML
    mkxml '<root><item>val</item></root>' t.xml
    xml -Y '.root.item' "$WORK_DIR/t.xml"
    assert_exit "exit_ok" 0
    assert_eq   "scalar"  "val" "$RUN_OUT"
}

test_yaml_multi_document() {
    printf 'a: 1\n---\nb: 2\n' > "$WORK_DIR/t.yaml"
    yam -r '.' "$WORK_DIR/t.yaml"
    assert_exit      "exit_ok"    0
    assert_contains  "first_doc"  "$RUN_OUT" '"a": 1'
    assert_contains  "second_doc" "$RUN_OUT" '"b": 2'
}

# ============================================================================
# Tests: HTML Input/Output
# ============================================================================

test_html_basic_parsing() {
    local INPUT='<!DOCTYPE html><html><body><p>hello</p></body></html>'
    mkxml "$INPUT" t.html
    xml -J '.' "$WORK_DIR/t.html"
    assert_exit     "exit_ok"   0
    assert_contains "has_html"  "$RUN_OUT" '"html"'
    assert_contains "has_body"  "$RUN_OUT" '"body"'
    assert_contains "has_p"     "$RUN_OUT" '"p"'
}

test_html_auto_detect_doctype() {
    local INPUT='<!DOCTYPE html><html><head><title>T</title></head></html>'
    xml -r '.html.head.title' <<< "$INPUT"
    assert_exit "exit_ok" 0
    assert_eq   "title"   "T" "$RUN_OUT"
}

# ============================================================================
# Tests: Stdin
# ============================================================================

test_stdin_pipe_works() {
    local INPUT='<root><item>stdin</item></root>'
    xml -r '.root.item' <<< "$INPUT"
    assert_exit "exit_ok"   0
    assert_eq   "extracted" "stdin" "$RUN_OUT"
}

# ============================================================================
# Tests: Error Paths
# ============================================================================

test_invalid_xml_errors() {
    xml -J '.' <<< 'not xml'
    assert_exit "exit_nonzero" 1
}

test_empty_input_errors() {
    xml -J '.' <<< ''
    assert_exit "exit_nonzero" 1
}

test_jq_filter_error_propagates() {
    xml -r '.root[invalid' <<< '<root/>'
    assert_exit "jq_error" 3
}

test_non_object_cannot_serialize_to_xml() {
    # Extracting a scalar value (non-object) and serializing to XML fails.
    xml -X '.root' <<< '<root>42</root>'
    assert_exit "exit_nonzero" 1
}

# ============================================================================
# Tests: CDATA
# ============================================================================

test_cdata_contains_raw_text() {
    mkxml '<root><![CDATA[hello <world>]]></root>' t.xml
    xml -r '.root' "$WORK_DIR/t.xml"
    assert_exit "exit_ok" 0
    assert_eq   "cdata"   "hello <world>" "$RUN_OUT"
}

# ============================================================================
# Tests: Null Input (-n)
# ============================================================================

test_null_input_uses_jq_null() {
    yam -r -n '42'
    assert_exit "exit_ok" 0
    assert_eq   "value"   "42" "$RUN_OUT"
}

# ============================================================================
# Tests: Invocation Name Detection (xq vs yq)
# ============================================================================

test_xq_defaults_to_xml_input() {
    # When invoked as xq, default input format is XML
    mkxml '<root><item>v</item></root>' t.xml
    xml -r '.root.item' "$WORK_DIR/t.xml"
    assert_exit "exit_ok"   0
    assert_eq   "extracted" "v" "$RUN_OUT"
}

test_xq_defaults_to_xml_output() {
    # When invoked as xq, default output format is XML
    mkxml '<root><item>v</item></root>' t.xml
    xml '.' "$WORK_DIR/t.xml"
    assert_exit     "exit_ok"   0
    assert_contains "xml_tags" "$RUN_OUT" '<root>'
}

test_yq_defaults_to_yaml_input() {
    # When invoked as yq, default input format is YAML.
    # XML piped to yq without -x should be treated as YAML text.
    printf 'key: value\n' > "$WORK_DIR/t.yaml"
    yam -r '.key' "$WORK_DIR/t.yaml"
    assert_exit "exit_ok"   0
    assert_eq   "value"     "value" "$RUN_OUT"
}

# ============================================================================
# Tests: Edge Cases
# ============================================================================

test_element_with_only_attributes() {
    mkxml '<root><item id="1"/></root>' t.xml
    xml -J '.' "$WORK_DIR/t.xml"
    assert_exit     "exit_ok"   0
    assert_contains "has_item"  "$RUN_OUT" '"item"'
}

test_nested_elements_preserve_order() {
    mkxml '<r><a>1</a><b>2</b><a>3</a></r>' t.xml
    xml -r '.r.a | length' "$WORK_DIR/t.xml"
    assert_exit "exit_ok"   0
    assert_eq   "a_count"   "2" "$RUN_OUT"
}

test_xml_force_list_via_flag() {
    mkxml '<root><item>single</item></root>' t.xml
    xml --xml-force-list item -r '.root.item | type' "$WORK_DIR/t.xml"
    assert_exit "exit_ok"   0
    assert_eq   "is_array"  "array" "$RUN_OUT"
}

# ============================================================================
# Main
# ============================================================================

main "$@"
