#!/bin/bash
. "$(dirname "$0")/common.sh"
exec "$PY" -I "$HERE/vregion.py" "$@"
