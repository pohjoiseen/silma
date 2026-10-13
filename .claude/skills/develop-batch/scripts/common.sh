# Sourced by the scripts. SIL: the silma binary (default: which-silma.sh); SILMA_BATCH: the
# batch; WORK: scratch dir.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SIL=${SIL:-$("$HERE/which-silma.sh")} || exit 1
: "${SILMA_BATCH:?set SILMA_BATCH to the .silma file}"
WORK=${WORK:-${TMPDIR:-/tmp}/silma-develop}
mkdir -p "$WORK"
PY=${PY:-$HOME/.cache/silma-develop-venv/bin/python}
export SIL SILMA_BATCH WORK
