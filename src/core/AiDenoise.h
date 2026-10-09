#pragma once
// AI noise reduction with pretrained ONNX models (ONNX Runtime). Runs on the GPU when the
// installed ONNX Runtime provides MIGraphX/ROCm (AMD) or CUDA, else on the CPU.

#include "Image.h"

#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>

namespace silma {

struct AiModelFile {
    QString name;
    QString url;
    qint64 size = 0;
};

struct AiModelInfo {
    QString id;          // stable identifier stored with derived photos
    QString name;        // display name
    QString license;
    QString onnxFile;    // the graph file (relative to the model dir)
    QList<AiModelFile> files;
    QString archive;     // optional zip URL instead of files
    int tileSize = 256;  // fixed input size (0 = dynamic)
    int multiple = 8;    // dynamic sizes must be multiples of this
    QString about;       // origin of the weights and of the ONNX export (HTML)
    bool prepared = false; // not downloaded: made from the original weights by the ai-models build target
    // Use ONNX Runtime's blocked (NCHWc) layout transform. It pays off only for the 8-bit model:
    // for the others its layout conversions cost more than they save, up to 4x on some machines
    // (strided memory traffic that can be dramatically slow natively).
    bool blockedLayout = false;
    // Works on raw data (aiDenoiseRaw): `onnxFile` on the Bayer mosaic, demosaicing at the same
    // time; `linearOnnxFile` on demosaiced linear Rec.2020 for other sensors.
    bool rawInput = false;
    QString linearOnnxFile;
};

QList<AiModelInfo> aiDenoiseModels();
AiModelInfo aiInpaintModel();
const AiModelInfo* findAiModel(const QString& id);
// The default denoise model (RawNIND).
QString defaultAiDenoiseModel();

// Model storage: $XDG_DATA_HOME/silma/models/<id>/
QString aiModelDir(const QString& id);
bool aiModelInstalled(const QString& id);
// Downloads model files (blocking; runs a local event loop, so a GUI stays responsive
// when called from the main thread). Throws on failure, also when `cancel` gets set.
using DownloadProgress = std::function<void(qint64 received, qint64 total)>;
void downloadAiModel(const QString& id, const DownloadProgress& progress = {}, const std::atomic<bool>* cancel = nullptr);

// Names of execution providers that will be tried, in order (e.g. "MIGraphX", "CPU").
QStringList aiExecutionProviders();
// CPU threads used for inference: one per physical core (SILMA_AI_THREADS overrides).
int aiThreadCount();

using AiProgress = std::function<void(int done, int total)>;

// Denoises an image with values in [0, 1] (model domain: display-like encoding) using
// overlapping tiles. `cancel` may be set from another thread.
ImageF runAiDenoise(const QString& modelPath, const ImageF& encoded, int tileSize, bool blockedLayout, const AiProgress& progress = {},
                    const std::atomic<bool>* cancel = nullptr, QString* providerUsed = nullptr);

// LaMa inpainting of a 512x512 image (0..1, display-like encoding); mask 1 = fill.
ImageF runAiInpaint(const ImageF& img, const PlaneF& mask, QString* providerUsed = nullptr);

// The model-domain encoding of linear camera RGB used for denoising: an invertible log
// curve adapted to the image brightness. `k` and `norm` are needed to decode.
ImageF aiDenoiseEncode(const ImageF& linear, float* k = nullptr, float* norm = nullptr);

// Denoises linear camera RGB (0..1, white-balanced). Wraps runAiDenoise with an invertible
// log encoding adapted to the image brightness. strength 0..1 blends with the input.
ImageF aiDenoiseLinear(const QString& modelId, const ImageF& linear, double strength, const AiProgress& progress = {},
                       const std::atomic<bool>* cancel = nullptr, QString* providerUsed = nullptr);

struct RawImage;

// Denoises a raw file with a raw-input model (AiModelInfo::rawInput). `raw` is its decodeRaw()
// result; returns the same kind of image (white-balanced camera RGB, 1 = clip). Bayer sensors
// are denoised on the mosaic, the others on `raw.image`. strength 0..1 blends with the input.
ImageF aiDenoiseRaw(const QString& modelId, const QString& path, const RawImage& raw, double strength, const AiProgress& progress = {},
                    const std::atomic<bool>* cancel = nullptr, QString* providerUsed = nullptr);

} // namespace silma
