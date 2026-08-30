#!/bin/sh
marker="$1"
here="$(dirname "$0")"

label=""
if [ -f "$here/app.json" ]; then
	label="$(sed -n 's/.*"label"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$here/app.json" | head -1)"
fi

if [ -n "$marker" ]; then
	printf 'v1 %s %s\n' "$$" "$label" > "$marker"
fi

while :; do
	sleep 0.5
done
