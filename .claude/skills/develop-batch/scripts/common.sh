# Sourced by the scripts. SIL: the silma binary; SILMA_BATCH: the batch; WORK: scratch dir.
SIL=${SIL:-$(command -v silma 2>/dev/null || echo /home/agent/shared/Silmä/build-vm/src/app/silma)}
: "${SILMA_BATCH:?set SILMA_BATCH to the .silma file}"
WORK=${WORK:-${TMPDIR:-/tmp}/silma-develop}
mkdir -p "$WORK"
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PY=${PY:-$HOME/.cache/silma-develop-venv/bin/python}
export SIL SILMA_BATCH WORK
