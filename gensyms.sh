#! /bin/sh

set -e

LC_ALL=C
export LC_ALL

TMP0="$(mktemp)"

cat >"$TMP0" <<EOF
#! /bin/sh

set -e

set -o pipefail 2>/dev/null
EOF

chmod +x "$TMP0"

"$TMP0" && set -o pipefail

rm "$TMP0"

TMP1="$(mktemp)"
TMP2="$(mktemp)"
TMP3="$(mktemp)"

trap "rm -f '$TMP1' '$TMP2' '$TMP3'; trap - EXIT; exit" EXIT INT TERM QUIT HUP

"$OBJDUMP" -t "$1" | ( "$SED" '/[[:<:]]d[[:>:]]/d' 2>/dev/null || "$SED" '/\bd\b/d' ) | sort > "$TMP1"
"$GREP" "\.text" < "$TMP1" | cut -d' ' -f1 > "$TMP2"
"$GREP" "\.text" < "$TMP1" | "$AWK" 'NF{ print $NF }' > "$TMP3"

cat <<EOF
#include <stdint.h>
#include <lib/trace.k.h>

struct symbol symbol_table[] = {
EOF

paste -d'$' "$TMP2" "$TMP3" | sed "s/^/    {0x/g;s/\\\$/, \"/g;s/\$/\"},/g"

cat <<EOF
    {0xffffffffffffffff, ""}
};
EOF
