# Changelog

## Unreleased

### Develop
- A camera profile's look table now applies after Highlights and Shadows, and looks up the
  brightness without most of the Exposure (Lightroom's Exposure rolls highlights off). Before,
  bright areas of a brightened sunset took on the colour the table gives to near-white (orange
  around the sun and on cloud edges turned yellow-green); they keep their colour now.
- Blown highlights keep the colour around them: where channels are clipped (a sun in a sunset,
  bright cloud edges in warm light), the missing channels come from the colour of the bright
  unclipped pixels nearby instead of fading to white or neutral, so a blown sun blends
  smoothly into its glare instead of being a flat white patch with an edge.
- Highlights no longer turns very bright areas darker than slightly less bright ones next to
  them (a dark star around the sun in sunsets with strong Highlights), and a small, far brighter
  light no longer pulls down its surroundings.
- Highlights keeps sun rays crisp: they no longer merge with the field between them into an
  orange glow.

## 1.1.1 (2026-10-15)

Tone sliders that behave like Lightroom's, measured on 820 of your own photos against their
Lightroom exports, and Lightroom panoramas that no longer come out magenta.

### Develop
- Tone sliders recalibrated against Lightroom: the same Exposure, Contrast, Highlights,
  Shadows and Whites values now give about the same result as in Lightroom (fitted on 820
  photos from six shoots with their Lightroom exports; the difference fell from about 7 to
  4.7 L* on average). Like Lightroom's, Highlights and Shadows now adapt to the photo:
  Highlights pulls only as far as the scene's range needs (little on overcast or snowy
  scenes, fully on bright clouds over a darker landscape), Shadows lifts more in bright
  photos than in dark ones. Whites is stronger, Contrast somewhat stronger. Existing edits
  render with the new sliders.
- Auto tone gives about the same result as before with the recalibrated sliders (its slider
  values differ: e.g. stronger Highlights on flat scenes, where the slider now does less).

### Interface
- The status bar shows the photo's place in the filmstrip, e.g. 42/275.

### Fixes
- Lightroom panoramas (and other merged DNGs) that Lightroom stores scaled per channel no
  longer come out magenta or green: the DNG's MapPolynomial opcodes are applied.

## 1.1.0 (2026-10-13)

Lightroom's camera profiles, highlights that behave like Lightroom's, and fixes found while
developing a whole trip with Silmä (by hand and by an AI agent through the CLI).

### Camera profiles
- Lightroom / Camera Raw profiles installed from your own copy (Silmä cannot ship them) can be
  chosen per photo: Profile in the White Balance panel. DNG camera profiles (`.dcp`, e.g. Adobe
  Standard) and look profiles over them (`.xmp`, e.g. Adobe Color) are rendered as the DNG
  specification describes; with Adobe Color, colours come close to Lightroom's.
- File › Install Camera Profiles (or `silma install-profiles`) copies them to
  `~/.local/share/silma/profiles`. A look without its base camera profile shows in the menu
  with what it needs.
- New photos (and Reset) get the profile chosen last.

### Develop
- Highlights (pulled down) work like Lightroom's: skies and clouds come down about evenly,
  blown areas stay near white instead of turning flat grey, and clouds keep their brightness
  against the sky. Existing edits render with the new curve.
- Highlight reconstruction: where only some channels are clipped (often green in white clouds)
  the others' detail is kept instead of everything being cut at the first clipped channel, so
  pulling highlights down brings back cloud detail like Lightroom does.
- Auto tone, tuned on a reviewed edit of a whole day's shoot: shaded foregrounds get their
  shadows lifted, more contrast throughout, and a moderate highlights pull, less on blown skies.

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
