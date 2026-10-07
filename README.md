# Silmä

A streamlined raw photo developer for Linux (Qt 6), modelled on Lightroom Classic's
Develop module. See `SPEC.md` for the goals.

## Building

Dependencies (Arch): `cmake ninja qt6-base libraw exiv2 lensfun libjpeg-turbo libtiff sqlite lcms2 opencv onnxruntime-cpu`.
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
Ctrl+Shift+C/V copy/paste settings, Tab / F6 / F7 / F8 hide panels.

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
source photo, added to the batch and developed like any raw. AI models are downloaded on first
use to `~/.local/share/silma/models` (NAFNet-SIDD by default, SCUNet optional; LaMa for heal).
Generated heal fills are stored in the batch, so heal spots are ordinary undoable edits.
