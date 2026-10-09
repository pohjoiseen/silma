# Silmä

A streamlined raw photo developer for Linux (Qt 6), modelled on Lightroom Classic's
Develop module. See `SPEC.md` for the goals.

## Building

Dependencies (Arch): `cmake ninja qt6-base libraw exiv2 lensfun libjpeg-turbo libtiff libjxl sqlite lcms2 opencv onnxruntime-cpu`.
Optional: `kcolorscheme` (KDE color schemes: the app defaults to Breeze Dark and offers the installed schemes under View → Color Scheme; without it a built-in dark palette is used).

For GPU-accelerated AI denoise install `onnxruntime-opt-rocm` (AMD) or `onnxruntime-cuda`
instead of `onnxruntime-cpu`; Silmä picks the GPU provider automatically
(`SILMA_AI_PROVIDER=cpu|rocm|migraphx|cuda` overrides).

    cmake -S . -B build -G Ninja
    ninja -C build
    ctest --test-dir build        # GUI test needs QT_QPA_PLATFORM=offscreen when headless
    sudo cmake --install build    # binary, desktop entry and .silma file type (then update-mime-database)

The binary is `build/src/app/silma`.

## Concepts

- **Batch**: a `*.silma` file (sqlite) holding everything about one trip: imports, photos,
  develop settings, full edit history and UI state. Schema is versioned and migrated.
- **Import**: a folder (by default `YYYY-MM-DD` next to the batch file) into which raw files are copied.
- Editing is non-destructive. Every change is a history step; undo/redo moves within it.

## GUI

`silma` opens the last batch; `silma TRIP.silma` opens a specific one.
Keyboard: Left/Right navigate, X/U reject/unreject, Z or click toggles fit/1:1, wheel zooms,
`\` before/after, C compare, R crop tool, Q heal tool, Ctrl+I metadata, Ctrl+U auto tone, Ctrl+Z / Ctrl+Shift+Z undo/redo,
W white balance picker (click a neutral point), Ctrl+Shift+C/V copy/paste settings (pasting asks which settings, Ctrl+Alt+Shift+V reuses the previous choice; heal spots are not pasted), Ctrl+Tab / F6 / F7 / F8 hide panels (Tab moves between controls), the pin button in a panel title (or Shift+F6 / F7 / F8) switches the filmstrip, left or right panel to auto hide & show (the panel slides in while the mouse is at that window edge). Right-click a photo in the filmstrip for reject, settings, merge, export and file actions.

## CLI (also meant for AI agents)

Every command accepts `--json` for machine-readable output. Set the batch with `-b FILE`
or the `SILMA_BATCH` environment variable. Photos are referred to by id, `import/FILE`,
a unique file name, or `all`.

    silma create trip.silma
    silma -b trip.silma import /media/card/DCIM --split-by-date
    silma list --import 2026-07-05
    silma meta 12 --all
    silma keys                              # every settable develop key and range
    silma set 12 exposure=0.7 shadows=30    # one history step
    silma auto 12 --tone --wb
    silma undo 12; silma redo 12; silma history 12
    silma render 12 preview.jpg --size 1600 # look at the result
    silma render 12 crop.jpg --size full --roi 0.4,0.4,0.2,0.2   # 1:1 detail
    silma sync 12 13 14 --only wb,exposure
    silma reject 15; silma delete-rejected --yes
    silma export --import 2026-07-05 --to ~/Pictures/out --long-edge 2048
    silma show 12                           # make a running GUI jump to photo 12
    silma upright 12 --mode vertical        # auto perspective (+ constrained crop)
    silma ai-denoise 12                     # AI noise reduction -> new DNG photo
    silma hdr 20 21 22                      # bracketed exposures -> HDR DNG
    silma pano 30 31 32 33                  # cylindrical panorama DNG
    silma ai-models                         # models, download state, GPU providers
    silma heal 12 0.31,0.62,0.03            # generative heal at a spot of the rendered output

The GUI picks up changes made through the CLI within a second.

For automation, `SILMA_SCREENSHOT=out.png [SILMA_SCREENSHOT_DELAY=ms] silma` grabs the
window and exits (works with `QT_QPA_PLATFORM=offscreen`).

## Derived photos

AI denoise, HDR and panorama results are written as linear (floating point) DNGs next to the
source photo, added to the batch and developed like any raw. Panoramas include a DNG transparency
mask of the stitched area, so Auto Crop works on them later too (e.g. after `pano --no-crop`). AI models are downloaded on first
use to `~/.local/share/silma/models` (NAFNet-SIDD; LaMa for heal).
AI Heal (Q) fills a clicked spot or a dragged brush stroke; its fills are stored in the batch, so
heals are ordinary undoable edits. LaMa works at 512×512, so a heal may span at most a fifth of the
photo's long edge, and larger ones come out softer.

JPEG XL compressed DNGs (DNG 1.7, e.g. Lightroom's HDR and panorama merges) are decoded with
libjxl, as LibRaw cannot unpack them; their transparency masks (uncovered panorama areas)
come out black, and Auto Crop finds the largest fully covered rectangle (any aspect ratio
unless the crop already has a locked one). JPEG XL compressed mosaic (CFA) DNGs are not supported yet.


### Faster AI denoise models

NAFNet width 32 denoises as well as the default width-64 model in about half the time, but no
ready-made ONNX export of it exists, so it is made locally from the official weights (needs
Python 3 and the network; PyTorch is installed into a venv in the build dir):

    cmake --build build --target ai-models

For a 24 MP photo on a Ryzen 5 8600G: width 64 takes 225 s, width 32 73 s. It also makes an 8-bit
version, calibrated on the raw files in `example-raws/` (set `SILMA_CALIBRATION_RAWS` to use
another folder): a little faster (67 s), denoising a little less. Any Python 3.10+ works; the
target prints the versions it used. Width 32 becomes the default once prepared.

For measuring: `SILMA_AI_THREADS` sets the inference threads (default: one per physical core),
`SILMA_AI_LAYOUT=0/1` switches ONNX Runtime's blocked layout transform (on only for the 8-bit
model: elsewhere its layout conversions cost more than they save) and `SILMA_AI_PROFILE=PREFIX`
writes ONNX Runtime's per-operator profile.

## License

Silmä is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version (GPL-3.0-or-later). See [LICENSE](LICENSE).

The AI models it downloads have their own licenses (MIT and Apache-2.0; see Help › About).
