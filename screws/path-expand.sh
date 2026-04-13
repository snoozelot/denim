#!/usr/bin/env bash
# path-expand-bash — expand shortened paths to full paths
#
# WHAT IT DOES
#
# For each directory level, expands a prefix to the full directory name
# if it uniquely matches exactly one entry. If ambiguous or no match,
# the prefix is kept unchanged.
#
# USAGE
#
#   path-expand-bash PATH
#   echo PATH | path-expand-bash
#
# EXAMPLES
#
#   path-expand-bash /h/l/bin/bad/parse_number.h
#   -> /home/luthor/bin/bad/parse_number.h
#
#   echo /h/l/bin/bad/parse_number.h | path-expand-bash
#   -> /home/luthor/bin/bad/parse_number.h

die() {
    printf 'path-expand-bash: %s\n' "$1" >&2
    exit 1
}

is_hidden() {
    local ENTRY=$1
    [[ $ENTRY == .* ]]
}

starts_with_pattern() {
    local ENTRY=$1
    local PATTERN=$2
    [[ $ENTRY == $PATTERN* ]]
}

is_absolute() {
    local PATH=$1
    [[ $PATH == /* ]]
}

is_empty() {
    local S=$1
    [[ -z $S ]]
}

is_last() {
    local I=$1
    local N=$2
    [[ $I -eq $((N - 1)) ]]
}

is_not_last() {
    local I=$1
    local N=$2
    [[ $I -lt $((N - 1)) ]]
}

ends_with_slash() {
    local S=$1
    [[ $S == */ ]]
}

find_unique_match() {
    local PARENT=$1
    local PREFIX=$2
    local MATCHES=0
    local MATCH=""
    local ENTRY

    [[ ! -d $PARENT ]] && {
        printf '%s' "$PREFIX"
        return
    }

    for ENTRY in "$PARENT"/*; do
        ENTRY=${ENTRY##*/}
        is_hidden "$ENTRY" && continue
        starts_with_pattern "$ENTRY" "$PREFIX" && {
            ((MATCHES++))
            MATCH=$ENTRY
        }
    done

    [[ $MATCHES -eq 1 ]] && {
        printf '%s' "$MATCH"
        return
    }

    printf '%s' "$PREFIX"
}

append_slash() {
    local S=$1
    ends_with_slash "$S" && {
        printf '%s' "$S"
        return
    }
    printf '%s/' "$S"
}

expand_path() {
    local PATH=$1
    local ABSOLUTE
    local PARENT
    local RESULT=()
    local PARTS
    local PART
    local EXPANDED
    local I

    is_absolute "$PATH" && ABSOLUTE=1 || ABSOLUTE=0

    IFS='/' read -ra PARTS <<<"$PATH"

    PARENT='.'
    is_absolute "$PATH" && PARENT='/'

    for I in "${!PARTS[@]}"; do
        PART=${PARTS[$I]}
        is_empty "$PART" && continue

        is_last "$I" "${#PARTS[@]}" && {
            EXPANDED=$(find_unique_match "$PARENT" "$PART")
            RESULT+=("$EXPANDED")
            break
        }

        EXPANDED=$(find_unique_match "$PARENT" "$PART")
        RESULT+=("$EXPANDED")

        PARENT=$(append_slash "$PARENT")
        PARENT+="$EXPANDED"
    done

    [[ $ABSOLUTE -eq 1 ]] && printf '/'

    for I in "${!RESULT[@]}"; do
        printf '%s' "${RESULT[$I]}"
        is_not_last "$I" "${#RESULT[@]}" && printf '/'
    done

    printf '\n'
}

main() {
    local INPUT

    if [[ $# -eq 1 ]]; then
        INPUT=$1
    elif [[ $# -eq 0 ]]; then
        read -r INPUT || die "no input"
    else
        die "usage: path-expand-bash PATH"
    fi

    is_empty "$INPUT" && die "empty path"

    expand_path "$INPUT"
}

main "$@"
