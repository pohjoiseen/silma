#pragma once
// Simple float images. Pixel values are linear unless stated otherwise.

#include <cstddef>
#include <vector>

namespace silma {

// Interleaved 3-channel float image.
struct ImageF {
    int width = 0, height = 0;
    std::vector<float> data;

    ImageF() = default;
    ImageF(int w, int h) : width(w), height(h), data(size_t(w) * h * 3, 0.0f) {}

    bool empty() const { return data.empty(); }
    size_t pixelCount() const { return size_t(width) * height; }
    float* px(int x, int y) { return data.data() + (size_t(y) * width + x) * 3; }
    const float* px(int x, int y) const { return data.data() + (size_t(y) * width + x) * 3; }
    float* row(int y) { return data.data() + size_t(y) * width * 3; }
    const float* row(int y) const { return data.data() + size_t(y) * width * 3; }
};

// Single-channel float image.
struct PlaneF {
    int width = 0, height = 0;
    std::vector<float> data;

    PlaneF() = default;
    PlaneF(int w, int h, float v = 0.0f) : width(w), height(h), data(size_t(w) * h, v) {}

    bool empty() const { return data.empty(); }
    float& at(int x, int y) { return data[size_t(y) * width + x]; }
    float at(int x, int y) const { return data[size_t(y) * width + x]; }
    float* row(int y) { return data.data() + size_t(y) * width; }
    const float* row(int y) const { return data.data() + size_t(y) * width; }
};

// Box-filter downscale by an integer factor (factor >= 1).
ImageF downscale(const ImageF& src, int factor);
// Area-averaging resize to an arbitrary smaller size (or bilinear if enlarging).
ImageF resize(const ImageF& src, int width, int height);
// Applies an EXIF orientation (1..8) to the image.
ImageF applyOrientation(const ImageF& src, int orientation);

} // namespace silma
