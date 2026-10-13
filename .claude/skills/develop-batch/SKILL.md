---
name: develop-batch
description: Cull and develop a shoot (or one day of it) with Silmä's CLI the way the user likes it - documentary style, rejects/duplicates, white balance and tone, perspective, crops, heals, denoise, merges - and report back. Use when asked to "develop", "edit", "process" or "cull" photos/a batch/a day of a trip with Silmä.
---

# Develop a batch with Silmä

You cull and develop photos yourself through `silma`'s CLI, look at the results as rendered JPEGs, and
hand the user a `.silma` batch to review in the GUI. Developed first on the Jakobstad trip
(2026-10-10/11, `example-raws/jakobstad-claude/`); the user's review of that edit is folded in below.

Don't add or change develop features while doing this. Building CLI features or automation for
controlling Silmä is fine (commit those separately, with tests, as usual).

## The user's taste (from their brief and review)

- **Documentary, realistic, not artsy.** Good contrast (slightly exaggerated is fine when it helps),
  highlight detail brought back *only as far as it stays natural*. White balance may lean a little
  warm, very slightly.
- **Highlights: don't overdo it.** A white overcast sky stays white. (Before 4e7e30b pulling
  highlights down turned blown skies into flat grey or blue patches, the worst artefact of the
  first edit; the slider now keeps blown areas near white, like Lightroom's.)
- **Profile:** the user uses Lightroom's Adobe Color; when it is installed (`silma profiles PHOTO`),
  `set P profile="Adobe Color"` before judging colour.
- **Shadows: lift them.** Overcast coast/water scenes and shaded foregrounds looked "dark overall".
- **Forest/greenery: tint ~0.** Auto WB tends to drift purple there; mostly green is normal.
- **Verticals must be vertical** (buildings, trees, poles, masts) unless the shot is at an obviously
  off angle (tree from its base, plaque photographed from above). If full correction would distort
  or crop the subject badly, correct as much as is reasonable. Occasionally the right fix is
  *horizontal* upright (a subject shot at an angle along a rail/wall).
- **Crops:** bring in the main subject, remove garbage and partially cut objects/people at the
  edges; don't overcrop. Keep 3:2 unless there is a reason.
- **Culling:** reject accidental shots, severe blur, really bad angles. Of near-identical photos keep
  the best - but different *expressions* of an animal/person count as different photos, keep a few.
  Never delete: only `reject`.
- **Heal** only accidental out-of-focus birds/insects/specks in the sky. **Denoise** only low-light,
  severely noisy photos (`ai-denoise`). **Panorama/HDR** only when successive frames were obviously
  shot for it (overlapping pans at the same exposure / brackets).

## Setup

```bash
SK=/home/agent/shared/Silmä/.claude/skills/develop-batch/scripts
$SK/setup.sh                      # once: OpenCV venv in ~/.cache, imagemagick
ninja -C /home/agent/shared/Silmä/build-vm   # current silma (build-vm/, never build/)
export SILMA_BATCH=/path/to/New.silma WORK=<scratchpad>/develop SIL=/home/agent/shared/Silmä/build-vm/src/app/silma
```

Run the loops in **bash** (`bash -c '...'` or script files): the interactive shell is zsh, which
does not word-split `$VAR` lists and expands `!`.

**Working in the user's own batch** (e.g. "process day 1 of this .silma"): first
`sqlite3 X.silma ".backup '~/silma-work-NAME/backup/before.silma'"`, check which photos already have
history beyond "Import" (`history` table), touch only the asked-for photos, and at the end diff
`file_name|rejected|history_pos|count(history)` of the other photos against the backup. Keep work files in
a home-directory folder (the session scratchpad can disappear on a restart).

Otherwise create the batch next to (not inside) the user's folders and import - this *copies* the raws:
`$SIL create X.silma && $SIL -b X.silma import FILES... [--into NAME]`. Pick files by
`exiftool -T -FileName -DateTimeOriginal` when only one day is wanted. Raw paths in a batch are
relative to the `.silma` file: a copy of the batch (for experiments) must sit in the same folder
(`sqlite3 X.silma ".backup '.copy.silma'"`), and be deleted afterwards.

## Workflow

1. **Look at everything first**: `$SK/sheets.sh r0 list.txt` (3x3 labelled sheets, 900 px renders)
   and Read each sheet. Note bursts, duplicates, odd angles, subjects.
   `$SIL list` shows capture times: frames seconds apart at the same focal length are bursts or
   pano sweeps. Preview a suspected pano with `pano P... --preview out.jpg` (seconds) before
   stitching; the stitched DNG is added to the batch, and the source frames get rejected.
2. **Cull**: for bursts render the subject at 1:1 (`render P out.jpg --size full --roi x,y,w,h`)
   side by side and compare sharpness *and* expression. `reject` the rest.
3. **Auto tone**: `$SIL auto --tone P...` (keeps as-shot WB). Since a239564 it already gives about
   the contrast/highlights/shadows the user wanted on Jakobstad; don't stack a blanket look on top
   (a blanket `highlights-=10` caused the blue-sky patches). Use relative per-photo tweaks:
   `$SIL set P contrast+=10 shadows+=15 temperature+=150` (clamped to range).
   Warmth: at most ~+150..200 K, skip it for already warm light and greenery.
4. **Perspective**, for every photo with verticals. For a whole day:
   `xargs -P8 -n1 $SK/persp.sh < list.txt > persp.log` does upright, measures, falls back to vfix
   and keeps the straighter one (about 10 min for 130 photos); then look at the large values
   (`settings`: |rotate| > 2.5, |vertical| > 35) and at photos where both methods scored badly.
   Shots angled down at a small object (bin, grave, signpost) or tree-only scenes get rotation only:
   `set P rotate=0 vertical=0` then `vfix.sh P --no-keystone`. Panoramas: `vfix.sh PANO --no-keystone`.
   Per photo:
   - `$SIL upright P --mode vertical`, then **measure, don't eyeball**: render at 2400 px and run
     `$SK/vfit.sh img.jpg` -> `r` = residual rotation (deg), `k` = residual keystone (deg across
     the width). Aim for |r| < 0.2 and |k| < 0.7. Upright sometimes makes it worse (it took the
     roll from a leaning shed or a boat's rigging); then reset and use
     `$SK/vfix.sh P [--dry]`, which iterates `rotate`/`vertical` on the measured lines.
   - Water/horizon shots: `vfix.sh P --horizon` levels the horizon; add `--no-keystone` when the
     only verticals are masts or trees (they lean on their own).
   - When numbers and your eye disagree, `$SK/vdraw.sh img.jpg out.jpg` shows which lines were used,
     and `$SK/grid.sh out.jpg imgs...` overlays plumb lines. The user notices 0.5-1 deg; you won't
     by eye on a contact sheet.
   - `upright --mode horizontal` for subjects shot obliquely along a line (rail, fence).
   - Measure the edges too: `$SK/vregion.sh 0 0.2 img.jpg` / `0.8 1` gives the mean tilt of lines
     in that horizontal band - windows at the frame edge being off was a complaint.
5. **Crop**: `$SIL crop P X,Y,W,H --aspect 3:2` - coordinates are of the current rendered view,
   so you can read them straight off a render. Re-render to check.
6. **Specks**: render skies at 3000 px, `$SK/specks.sh imgs...`, crop each candidate (300 px) and
   look; heal real ones with `SILMA_MODEL_DIR=~/silma-models $SIL heal P x,y,0.005` and check 1:1.
7. **Noise**: check ISO in `exiftool`/`$SIL list`; at 1:1 on shadows. Daylight ISO <= 800 needs none.
8. **Review pass**: sheets of all accepted photos; then 1:1 crops of a few for halos/noise.
   Compare variants with `$SK/variant.sh P out.jpg KEY=V...` (renders, then undoes).
9. Check that history ends in a sane place (`$SIL history P`): `undo` leaves redo entries, fine.

## Report

Brief: where the batch is, how many rejected (with one-line reasons, grouped), what was done
globally and per photo, photos the user should look at (borderline rejects, compromises), and any
Silmä bugs/gaps found (with a suggested fix; offer to build CLI features). Save a memory note
about the batch; when the user reviews it, fold lasting preferences into "The user's taste" above.
