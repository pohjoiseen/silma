#pragma once

#include "Develop.h"
#include "Image.h"

namespace silma {

// Classic noise reduction on linear Rec.2020 data (in place): à-trous wavelet soft
// thresholding of luma and chroma in a gamma-encoded opponent space. The noise level is
// estimated from the finest wavelet band, so it adapts to the rendering scale.
// `pxScale`: output pixels per full-resolution pixel (skips work when noise is invisible).
void denoise(ImageF& img, const DevelopSettings& s, double pxScale);

// Robust noise sigma estimate of a plane (MAD of the finest à-trous detail band).
float estimateNoiseSigma(const PlaneF& p);

} // namespace silma
