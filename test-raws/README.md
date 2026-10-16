# Test raws

Raw files the tests in `tests/` use (through `tests/TestRaws.h`). They are too big for git, so
only this file is tracked; tests that need a missing file are skipped. Each file is an
unmodified copy from the shoot named below.

| Path | What it is | Used by |
|---|---|---|
| `batch/DSC_0001.NEF` … `DSC_0010.NEF` | Nikon D5600, 2026-08-08: ten consecutive photos, daylight | GUI tests (`freshBatch()` takes the first *n*); `DSC_0001` as a typical NEF: WB picker, Bayer mosaic, JPEG export, JPEG/HEIC import |
| `other-day/DSC_0001.NEF` | Nikon D5600, 2026-07-05 | import split by date (with the drone DNG), sample renders |
| `drone/DJI_0488.DNG` | DJI FC7303 (drone) DNG, 2026-05-17: landscape with some haze | texture/dehaze, RawNIND denoise, Bayer mosaic of a DNG, hot pixels (a daylight photo has almost none), sample renders, import split by date |
| `denoise/DSC_0036.NEF` | Nikon D5600, 2026-09-01: ISO 1600, a stuck red pixel at (2412, 3936) | hot pixel removal |
| `upright/DSC_0154.NEF` | 2026-09-01: lighthouse shot looking up | Upright: strong vertical correction |
| `upright/DSC_0160.NEF` | 2026-09-01: walls with many verticals | Upright: vertical 14, rotate −0.9 |
| `upright/DSC_0164.NEF` | 2026-09-01: harbour with a mast, few building lines | Upright: level horizon |
| `merge/sequence/DSC_0010.NEF` … `DSC_0012.NEF` | 2026-08-08: handheld sequence (same as in `batch/` for `DSC_0010`) | HDR and panorama merges, merge dialog |
| `merge/apart/DSC_0153.NEF`, `DSC_0160.NEF` | 2026-09-01: two photos that do not overlap | panorama preview refuses them |
| `auto-tone/DSC_0251.NEF` | 2026-10-10 (Jakobstad): overcast sky, about 11% blown | auto tone hardly pulls blown skies down |
| `auto-tone/DSC_0160.NEF` | 2026-10-10 (Jakobstad): bright, nothing clipped | auto tone: moderate highlight pull, some contrast; look table looks up brightness without most of Exposure |
| `sunset/DSC_0076.NEF` | 2026-07-18 (Vörå): sunset with the sun in the frame, partly clipped | highlights pull keeps the glare under the sun brighter than beside it |
| `highlights/DSC_0775.NEF` | 2026-06-01: street, blue sky with green and blue clipped near the sun, red not | the clipped sky is rebuilt blue, not grey |
| `highlights/DSC_0789.NEF` | 2026-06-01: park, the sun blown through haze behind trees | the blown haze stays neutral, not pink |
| `moire/DSC_0193.NEF` | 2026-02-21: dark interior, a wall of fine vertical rods | a downscaled render shows no coloured moire on the rods |
| `heal/DSC_0320.NEF` | 2026-10-10 (Jakobstad): school building, a small bird against an even cloudy sky | AI heal: the fill matches the sky's colour (with the LaMa model) |
| `dng/DSC_0785-Pano.dng` | Lightroom panorama: DNG 1.7, JPEG XL compressed linear raw with a transparency mask | JPEG XL DNG decoding |
| `dng/DJI_0487-Pano.dng` | Lightroom panorama from the drone (DJI FC7303), 2026-02-22: JPEG XL linear raw stored scaled, with MapPolynomial opcodes | MapPolynomial (OpcodeList2) decoding |

The AI tests also need the models: run them with `SILMA_MODEL_DIR` pointing at the installed
models, else they are skipped.
