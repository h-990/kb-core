#!/bin/sh
# embed_html.sh — Convert a file to a C byte array using xxd.
# Usage: embed_html.sh <input> <output.c>

INPUT="$1"
OUTPUT="$2"

if [ -z "$INPUT" ] || [ -z "$OUTPUT" ]; then
    echo "Usage: $0 <input> <output.c>"
    exit 1
fi

SIZE=$(wc -c < "$INPUT" | tr -d ' ')

cat > "$OUTPUT" << EOF
/* Auto-generated — do not edit. */
const unsigned int kb_gui_html_len = ${SIZE};
const char kb_gui_html[] = {
EOF

xxd -i < "$INPUT" >> "$OUTPUT"

cat >> "$OUTPUT" << EOF
, 0x00
};
EOF
