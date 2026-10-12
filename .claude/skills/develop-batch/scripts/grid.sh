#!/bin/bash
# grid.sh OUT.jpg IMG... : overlay plumb lines every 1/10 of the width (--h: level lines every 1/20 of the height); 2 columns
. "$(dirname "$0")/common.sh"
horiz=0; [ "$1" = --h ] && { horiz=1; shift; }
out=$1; shift; args=()
for f in "$@"; do
  g=$WORK/grid_$(basename "$f"); w=$(magick identify -format %w "$f"); h=$(magick identify -format %h "$f"); d=()
  if [ $horiz = 1 ]; then for i in $(seq 1 19); do y=$((h*i/20)); d+=(-draw "line 0,$y $w,$y"); done
  else for i in $(seq 1 9); do x=$((w*i/10)); d+=(-draw "line $x,0 $x,$h"); done; fi
  magick "$f" -stroke 'rgba(255,0,255,0.6)' "${d[@]}" "$g"; args+=(-label "$(basename "$f" .jpg)" "$g")
done
montage "${args[@]}" -font Adwaita-Mono -pointsize 16 -fill white -tile 2x -geometry 780x520+3+3 -background '#222' "$out"
