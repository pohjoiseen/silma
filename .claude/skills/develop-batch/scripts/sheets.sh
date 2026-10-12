#!/bin/bash
# sheets.sh NAME LISTFILE|PHOTO...  -> $WORK/NAME/*.jpg and labeled 3x3 contact sheets $WORK/NAME_N.jpg
# Env: SIZE (render long edge, default 900).
. "$(dirname "$0")/common.sh"
name=$1; shift
if [ -f "$1" ]; then mapfile -t files < "$1"; else files=("$@"); fi
mkdir -p "$WORK/$name"
printf '%s\n' "${files[@]}" | xargs -P6 -I{} "$SIL" render {} "$WORK/$name/{}.jpg" --size ${SIZE:-900} >/dev/null 2>&1
n=0
for ((i=0;i<${#files[@]};i+=9)); do
  args=(); for f in "${files[@]:i:9}"; do args+=(-label "${f%.*}" "$WORK/$name/$f.jpg"); done
  montage "${args[@]}" -font Adwaita-Mono -pointsize 18 -fill white -tile 3x3 -geometry 520x347+4+4 -background '#222' "$WORK/${name}_$n.jpg"
  echo "$WORK/${name}_$n.jpg"; n=$((n+1))
done
