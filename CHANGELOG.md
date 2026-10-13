# Changelog

## Unreleased

### Develop
- Auto tone, tuned on a reviewed edit of a whole day's shoot: highlights are pulled down
  less, and hardly at all where the sky is blown (it turned into flat grey or blue patches);
  shaded foregrounds get their shadows lifted; more contrast throughout.
- Highlights (pulled down) work like Lightroom's: skies and clouds come down about evenly,
  blown areas stay near white instead of turning flat grey, and clouds keep their brightness
  against the sky. Existing edits render with the new curve.
- Auto tone holds highlights back less on blown skies (they now keep detail or stay white).
- Highlight reconstruction: where only some channels are clipped (often green in white clouds)
  the others' detail is kept instead of everything being cut at the first clipped channel, so
  pulling highlights down brings back cloud detail like Lightroom does.
- Camera profiles: Lightroom / Camera Raw profiles (Adobe Standard `.dcp`, look profiles such
  as Adobe Color `.xmp`) installed from your own copy can be chosen per photo (Profile in the
  White Balance panel). File › Install Camera Profiles copies them to
  `~/.local/share/silma/profiles`. New photos (and Reset) get the profile chosen last. A look
  without its base camera profile shows in the menu with what it needs.

### Performance
- Thumbnail updates (e.g. after pasting settings to hundreds of photos) run in the background
  on a few cores, visible photos first, instead of all at once on every core.
- Pasted edits are no longer rendered a second time when another process writes to the batch
  (e.g. after AI Denoise).

### Command line
- `install-profiles FILE|DIR...`, `profiles [PHOTO]`, and `set PHOTO profile=NAME`.
- `set` takes relative changes: `KEY+=D` and `KEY-=D` (clamped to the setting's range).
- New `crop PHOTO... X,Y,W,H [--aspect W:H]`: crop to a part of the current crop, in the
  coordinates of what `render` shows.
- `undo`/`redo` no longer fail with "database is locked" when another `silma` process writes
  to the batch at the same time.

### Export
- Parallel exports could render photos without lens corrections now and then (lensfun is not
  thread-safe); those then also got "empty borders" warnings.
- Empty-border warnings only for black edges of 2 px or more (constrained crops can end a
  fraction of a pixel past the image, which shows nothing), and they say how wide they are.
- When an exported file is there already, asks: Overwrite, Use Another Name ("-2"), Skip or
  Cancel Export, optionally for all such files. (Two photos of one export with the same
  name: the second gets another name, as before.)
- Warns about photos set to a camera profile that is not installed (they are exported with
  Silmä's own colour).

## 1.0.1 (2026-10-12)

Fixes and conveniences from editing a first real trip with Silmä.

### Develop
- Sharper previews: the fitted (and other zoomed-out) views are rendered with bicubic
  resampling and the export's output sharpening, and drawn on the device pixel grid, so they
  look like an export at that size.
- Auto tone pulls the highlights down less when much of the photo is blown (an overcast sky
  turned grey).
- AI heal: fills match their surroundings in colour (they could come out as a colour cast,
  e.g. a cyan-green disc in a cloudy sky). Heals made before keep their fills; redo them to
  get the new ones.
- Constrain crop (Transform panel, on by default): perspective, rotation, scale and lens
  distortion changes keep the crop within the image; an automatic crop follows them, a crop
  set by hand only shrinks when it would show empty borders. Upright follows it too.
- The image area of a raw is its default crop: the DNG DefaultCrop, or else 8 pixels left
  out on each side, like Lightroom (a 6016x4016 Nikon raw becomes 6000x4000). DNGs written
  from a raw (AI denoise, HDR) keep it.
- Quick repeated edits of one slider (arrow keys, wheel, typing) make a single history step.
- Transform guides are 3x denser.

### Keyboard
- Space toggles fit / 1:1 zoom (unless a button or a combo box has focus).
- Rejecting a photo reached with Left (Previous Photo) moves on to the left.
- Crop tool: Enter closes it, Esc cancels the changes made in it.

### Export
- Warnings about photos whose crop shows empty (black) borders, listed after the export
  with the photos that failed (the CLI prints them, `warnings` in `--json`).
- A New Folder button next to Browse.
- Open the folder after export (on by default), with the exported photos selected.

### Development
- Tests use the raws in `test-raws/`, grouped by purpose and listed in its README.

## 1.0.0 (2026-10-09)

First release.
