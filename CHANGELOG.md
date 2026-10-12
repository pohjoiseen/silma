# Changelog

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
