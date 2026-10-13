#!/bin/bash
# One-time: Python venv with OpenCV for the line/speck tools (a distribution's python-opencv
# may be broken), and a check for ImageMagick (contact sheets).
set -e
V=$HOME/.cache/silma-develop-venv
[ -x $V/bin/python ] || python3 -m venv $V
$V/bin/pip install -q opencv-python-headless numpy
command -v montage >/dev/null || { echo "install ImageMagick (montage, magick) with the system package manager" >&2; exit 1; }
echo "ok; silma: $("$(dirname "$0")/which-silma.sh")"
