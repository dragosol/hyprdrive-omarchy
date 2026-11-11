#!/usr/bin/env bash
# check-shared.sh [OTHER_TREE] -- the files both Hyprdrive trees (native and
# Omarchy) must carry byte for byte, compared by sha256.
#
# The list is scripts/shared-files.txt: one path per line, relative to the tree
# root; a directory stands for every file below it; '#' starts a comment. The
# list itself and this script are on it, so the two trees also agree on what is
# shared. A symlink is compared by its target, not followed.
#
# OTHER_TREE is the argument, else $HYPRDRIVE_SIBLING_TREE. With neither, or
# when that tree is not there or does not carry scripts/shared-files.txt yet,
# the check prints SKIP and passes: a lone checkout has nothing to compare.
# Exit 1 when a listed file differs or exists on one side only.
set -u
cd "$(dirname "$0")/.."
HERE=$PWD
OTHER=${1:-${HYPRDRIVE_SIBLING_TREE:-}}
LIST=scripts/shared-files.txt

if [ -z "$OTHER" ]; then
    echo "check-shared: SKIP -- no sibling tree given (argument or HYPRDRIVE_SIBLING_TREE)"
    exit 0
fi
if [ ! -d "$OTHER" ]; then
    echo "check-shared: SKIP -- no sibling tree at $OTHER"
    exit 0
fi
OTHER=$(cd "$OTHER" && pwd)
if [ "$OTHER" = "$HERE" ]; then
    echo "check-shared: SKIP -- $OTHER is this tree"
    exit 0
fi
if [ ! -f "$OTHER/$LIST" ]; then
    echo "check-shared: SKIP -- $OTHER has no $LIST"
    exit 0
fi

# Every file a list entry names in one tree, relative to that tree.
expand() { # root
    local root=$1 entry
    while IFS= read -r entry || [ -n "$entry" ]; do
        entry=${entry%%#*}
        entry=$(printf '%s' "$entry" | sed 's/[[:space:]]*$//; s/^[[:space:]]*//')
        [ -n "$entry" ] || continue
        if [ -d "$root/$entry" ] && [ ! -L "$root/$entry" ]; then
            (cd "$root" && find "$entry" \( -type f -o -type l \) -print)
        else
            printf '%s\n' "$entry"
        fi
    done < "$HERE/$LIST"
}

digest() { # root path
    local p="$1/$2"
    if [ -L "$p" ]; then
        printf 'link:%s' "$(readlink "$p")" | sha256sum | cut -d' ' -f1
    elif [ -f "$p" ]; then
        sha256sum < "$p" | cut -d' ' -f1
    else
        echo missing
    fi
}

fail=0
n=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    n=$((n + 1))
    a=$(digest "$HERE" "$f")
    b=$(digest "$OTHER" "$f")
    if [ "$a" = missing ] && [ "$b" = missing ]; then
        echo "check-shared: listed but in neither tree: $f" >&2
        fail=1
    elif [ "$a" = missing ]; then
        echo "check-shared: only in $OTHER: $f" >&2
        fail=1
    elif [ "$b" = missing ]; then
        echo "check-shared: only here: $f" >&2
        fail=1
    elif [ "$a" != "$b" ]; then
        echo "check-shared: DIFFERS from $OTHER: $f" >&2
        fail=1
    fi
done < <({ expand "$HERE"; expand "$OTHER"; } | LC_ALL=C sort -u)

if [ "$fail" = 0 ]; then
    echo "check-shared: OK -- $n shared files identical with $OTHER"
else
    echo "check-shared: FAILED -- the trees disagree on files that must be identical ($LIST)" >&2
fi
exit "$fail"
