#!/bin/bash
# variant.sh PHOTO OUT.jpg KEY=V|KEY+=D... : render with temporary settings, then undo them
. "$(dirname "$0")/common.sh"
p=$1; o=$2; shift 2
"$SIL" set "$p" "$@" --label tmp >/dev/null && "$SIL" render "$p" "$o" --size ${SIZE:-1200} >/dev/null; "$SIL" undo "$p" >/dev/null
