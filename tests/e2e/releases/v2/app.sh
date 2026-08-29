#!/bin/sh
marker="$1"

if [ -n "$marker" ]; then
	printf 'v2 %s\n' "$$" > "$marker"
fi

while :; do
	sleep 0.5
done
