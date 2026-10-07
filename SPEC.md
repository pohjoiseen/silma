The idea of this project is to write a desktop application, which would be a streamlined Linux version of Adobe Lightroom Classic, with only the features I need in practice.  I travel a lot around Finland and Nordic countries and typically photograph everything I see with a cheap DSLR.  I use Lightroom to sort through these pictures and do basic corrections, but I neved do any serious editing (and if I ever need to, I could use a full-featured app).  Lightroom is however slow, the subscription costs money, and I have to reboot into Windows for it, while I otherwise haven't had any use for Windows in years.

This project (Silmä, as in Finnish "eye") should be a desktop app for Linux (with Wayland display output), no other platforms need to be supported.  I run KDE, so using Qt/KDE frameworks probably would be a good way to make sure it visually integrates into the system.  However this is not a hard requirement, if there's anything faster or more convenient to use, that would do as well.  You can use any programming language(s) that you see convenient.

The basic requirements I envision for the application are:

* Keep information about image edits in a file-backed database (sqlite), like Lightroom.  Refer to it as a batch.  A batch can have one or more imports, and an import (a folder essentially) can have one or more RAW files.  The expectation is that a batch would contain a reasonable number of photos (thousands at most) from a single trip, not the entire history over years.  Note format versioning!
* Use basic OS file open dialog to pick files for an import.  They could go to a new or existing import folder, those should be by default named after dates
* The only RAW formats I need in practice are for now RAWs from Nikon D5600 (NEF), DJI Mini 2 (DNG) and maybe iPhone (DNG).  But if more can be added with reasonable effort and without compromising anything else, why not
* It should be possible to view photo metadata, but I don't really need to edit it
* The only cull feature I need is simply "mark as rejected".  It should be possible to physically delete all rejected files in a batch, but only when the user explicitly requests so
* Lens profile should be applied automatically
* The main view should be mostly similar to Lightroom's Develop view: the photo being developed, side pane with tools, bottom pane with all the photos from selected batch, and another side pane with batches in current import and undo stack.  Should be possible to hide panes
* Should be possible to zoom photo (to 1:1 by default, but to any reasonable ratio), display alongside unedited version, display aside another photo
* Editing should be non-destructive with an unlimited undo stack
* Editing tools I need are (arrange in subpanes like in Lightroom):
  - RGB histogram
  - White balance (temperature, tint; allow auto)
  - Tone: exposure, contrast, highlights, shadows, whites, blacks (have auto button)
  - Presence: clarity, vibrance, saturation, make them work like in Lightroom
  - Tone curve, rarely needed, collapsed by default
  - Lens correction: distortion, vignetting.  Remove chromatic aberration and basic lens profile should be always on (I guess we don't need to support lenses we don't know about)
  - Transform: vertical perspective, horizontal horizontal, rotate.  Allow auto (vertical, horizontal or both).  Show guide lines while manipulating these sliders
  - Crop: manual (keep aspect ratio on/off) and auto-crop button
  - Detail: sharpening and noise reduction, as in Lightroom, including AI denoise if possible.  (Lightroom saves a separate DNG if AI denoise is used -- I suppose makes sense since it takes quite long.  It would be very nice if the denoising could use GPUs, or at least AMD GPUs specifically)
  - Heal: allow to "heal" a patch using generative AI
  - Panorama: allow to merge several selected pictures into a cylindrical panorama, which should be saved as a separate DNG file
  - HDR: allow to merge several selected pictures into a HDR-enabled one, which should be saved as a separate DNG file
  - This should be all, but probably should make sure it is not to annoying to add more tools if needed.  I assume Lightroom in practice has some fixed pipeline which applies these
* Allow to export selected files, or all of an import or a batch, to a folder, as JPEGs; allow to set particular size (short size/long size), quality, filename pattern (default to YYYYMMDD-OriginalFilename.jpg)
* Closing and reopening app should by default reopen the same import and restore the last view as close as possible
* It should be possible for an AI agent to operate the app in some reasonable way, ideally with a CLI

This is probably not an exhaustive list but should be good enough as a starting point.

The algorithms to do all this image processing, I assume, are all very tricky and mathy.  Consider checking implementations from other open-source software (Darktable alone should in theory be able to do almost all of it, except probably panoramas/HDRs) if necessary, but if you feel you can do a good job as it is, go for it.

Commit your work as often as possible.

You will find many examples of RAWs from Nikon D5600 and DJI Mini 2 in example-raw folder.

Good luck!