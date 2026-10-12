#!/bin/bash
# One-time: Python venv with OpenCV for the line/speck tools (Arch's python-opencv may be broken).
set -e
V=$HOME/.cache/silma-develop-venv
[ -x $V/bin/python ] || python -m venv $V
$V/bin/pip install -q opencv-python-headless numpy
command -v montage >/dev/null || sudo pacman -S --noconfirm imagemagick
echo ok
