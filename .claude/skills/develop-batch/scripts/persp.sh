#!/bin/bash
# persp.sh PHOTO [vfix args]: upright --mode vertical; if the measured residual is not small, also try vfix
# and keep whichever measured straighter. Prints: PHOTO method score r k (and the other method's numbers).
# Run many in parallel: xargs -P8 -n1 persp.sh < list.txt > persp.log
. "$(dirname "$0")/common.sh"
P=$1; shift
export WORK=$WORK/persp/$P; mkdir -p "$WORK"
score() { "$SIL" render "$P" "$WORK/m.jpg" --size 2400 >/dev/null 2>&1
  "$HERE/vfit.sh" "$WORK/m.jpg" | awk '{ if ($2=="none") print "none 0 0"; else { split($2,a,"="); split($3,b,"=");
    r=a[2]+0; k=b[2]+0; printf "%.2f %+.2f %+.2f\n", (r<0?-r:r)+(k<0?-k:k)/3, r, k } }'; }
# history position; upright/vfix add no step when they change nothing, so undo only what was added
pos() { "$SIL" history "$P" | awk '$1=="*"{print $2}'; }
undo_since() { [ "$(pos)" != "$1" ] && "$SIL" undo "$P" >/dev/null; }
p0=$(pos)
"$SIL" upright "$P" --mode vertical >/dev/null 2>&1
U=$(score); us=${U%% *}
if [ "$us" != none ] && awk -v s="$us" 'BEGIN{exit !(s<0.25)}'; then echo "$P upright $U"; exit; fi
undo_since "$p0"
"$HERE/vfix.sh" "$P" "$@" >/dev/null 2>&1
F=$(score); fs=${F%% *}
if [ "$us" != none ] && { [ "$fs" = none ] || awk -v a="$us" -v b="$fs" 'BEGIN{exit !(a<=b)}'; }; then
  undo_since "$p0"; "$SIL" upright "$P" --mode vertical >/dev/null 2>&1; echo "$P upright* $U (vfix $F)"
else echo "$P vfix $F (upright $U)"; fi
