#!/usr/bin/env bash
# path-shorten-bash — shorten paths using minimal unique prefixes
#
# WHAT IT DOES
#
# For each directory level (except the last), finds the shortest prefix
# that uniquely identifies it among its siblings. The final component
# (filename) is kept unchanged.
#
# USAGE
#
#   path-shorten-bash PATH
#   echo PATH | path-shorten-bash
#
# EXAMPLES
#
#   path-shorten-bash /home/luthor/bin/bad/parse_number.h
#   -> /h/l/bin/bad/parse_number.h
#
#   echo /home/luthor/bin/bad/parse_number.h | path-shorten-bash
#   -> /h/l/bin/bad/parse_number.h

die() {
    printf 'path-shorten-bash: %s\n' "$1" >&2
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

is_unique() {
    local MATCHES=$1
    [[ $MATCHES -eq 1 ]]
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

find_min_prefix() {
    local PARENT=$1
    local TARGET=$2
    local LEN=1
    local MATCHES
    local PATTERN
    local ENTRY

    [[ ! -d $PARENT ]] && {
        printf '%s' "$TARGET"
        return
    }

    while [[ $LEN -le ${#TARGET} ]]; do
        PATTERN=${TARGET:0:$LEN}
        MATCHES=0

        for ENTRY in "$PARENT"/*; do
            ENTRY=${ENTRY##*/}
            is_hidden "$ENTRY" && continue
            starts_with_pattern "$ENTRY" "$PATTERN" && ((MATCHES++))
        done

        is_unique "$MATCHES" && {
            printf '%s' "$PATTERN"
            return
        }

        ((LEN++))
    done

    printf '%s' "$TARGET"
}

append_slash() {
    local S=$1
    ends_with_slash "$S" && {
        printf '%s' "$S"
        return
    }
    printf '%s/' "$S"
}

shorten_path() {
    local PATH=$1
    local ABSOLUTE
    local PARENT
    local RESULT=()
    local PARTS
    local PART
    local SHORTENED
    local I

    is_absolute "$PATH" && ABSOLUTE=1 || ABSOLUTE=0

    IFS='/' read -ra PARTS <<< "$PATH"

    PARENT='.'
    is_absolute "$PATH" && PARENT='/'

    for I in "${!PARTS[@]}"; do
        PART=${PARTS[$I]}
        is_empty "$PART" && continue

        is_last "$I" "${#PARTS[@]}" && {
            RESULT+=("$PART")
            break
        }

        SHORTENED=$(find_min_prefix "$PARENT" "$PART")
        RESULT+=("$SHORTENED")

        PARENT=$(append_slash "$PARENT")
        PARENT+="$PART"
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
        die "usage: path-shorten-bash PATH"
    fi

    is_empty "$INPUT" && die "empty path"

    shorten_path "$INPUT"
}

main "$@"
