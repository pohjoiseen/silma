#!/bin/bash
# Prints the silma binary to use: one built in the Silmä checkout that contains the working
# directory (the most recently built <build dir>/src/app/silma that runs here: a checkout
# shared between machines can hold builds of the other one), else silma from $PATH.
root=$(git -C "$PWD" rev-parse --show-toplevel 2>/dev/null || echo "$PWD")
if [ -f "$root/src/app/main.cpp" ]; then
  while IFS= read -r b; do
    if [ -x "$b" ] && "$b" keys >/dev/null 2>&1; then echo "$b"; exit 0; fi
  done < <(ls -t "$root"/*/src/app/silma 2>/dev/null)
fi
command -v silma || { echo "no silma: build it in this checkout or install it" >&2; exit 1; }
