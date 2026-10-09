#include "AiDenoise.h"

#include "Color.h"
#include "RawDecoder.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <onnxruntime/onnxruntime_cxx_api.h>
#include <stdexcept>
#include <thread>

namespace silma {

QList<AiModelInfo> aiDenoiseModels()
{
    static const QList<AiModelInfo> models = {
        // Joint demosaicing and denoising on the raw mosaic; trained on real camera noise.
        {"rawnind", "RawNIND (raw, demosaics too)", "GPL-3.0", "rawdenoise-nind/model_bayer.onnx", {},
         "https://github.com/darktable-org/darktable-ai/releases/download/release-5.6.0/rawdenoise-nind.dtmodel", 512,
         "<a href=\"https://github.com/trougnouf/rawnind_jddc\">trougnouf/rawnind_jddc</a> UtNet2 (Brummer &amp; De Vleeschouwer, "
         "<a href=\"https://arxiv.org/abs/2501.08924\">arXiv:2501.08924</a>); ONNX: "
         "<a href=\"https://github.com/darktable-org/darktable-ai\">darktable-ai</a> release-5.6.0",
         "rawdenoise-nind/model_linear.onnx"},
    };
    return models;
}

AiModelInfo aiInpaintModel()
{
    return {"lama", "LaMa (big-lama)", "Apache-2.0", "lama_fp32.onnx",
            {{"lama_fp32.onnx", "https://huggingface.co/Carve/LaMa-ONNX/resolve/c3c0c9e468934d62e79c329e35d82dd09ff8c444/lama_fp32.onnx", 208044816}}, {}, 512,
            "<a href=\"https://github.com/advimman/lama\">advimman/lama</a> (big-lama); ONNX: <a href=\"https://huggingface.co/Carve/LaMa-ONNX\">Carve/LaMa-ONNX</a> @c3c0c9e"};
}

const AiModelInfo* findAiModel(const QString& id)
{
    static const QList<AiModelInfo> models = aiDenoiseModels() + QList<AiModelInfo>{aiInpaintModel()};
    for (const auto& m : models)
        if (m.id == id)
            return &m;
    return nullptr;
}

QString defaultAiDenoiseModel()
{
    return QStringLiteral("rawnind");
}

QString aiModelDir(const QString& id)
{
    QString base = qEnvironmentVariable("SILMA_MODEL_DIR");
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/silma/models";
    return base + "/" + id;
}

bool aiModelInstalled(const QString& id)
{
    const AiModelInfo* m = findAiModel(id);
    return m && QFile::exists(aiModelDir(id) + "/" + m->onnxFile);
}

namespace {

// `expectedSize` > 0: the download must have exactly that size (catches truncation).
void downloadFile(const QString& url, const QString& dest, qint64 expectedSize, const DownloadProgress& progress,
                  const std::atomic<bool>* cancel)
{
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(30000); // give up when no data arrives for 30 s
    QFile out(dest + ".part");
    if (!out.open(QIODevice::WriteOnly))
        throw std::runtime_error(("Cannot write " + dest).toStdString());
    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    bool canceled = false;
    QObject::connect(reply, &QNetworkReply::readyRead, [&] { out.write(reply->readAll()); });
    QObject::connect(reply, &QNetworkReply::downloadProgress, [&](qint64 r, qint64 t) {
        if (progress)
            progress(r, t > 0 ? t : expectedSize);
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    // Cancellation is polled so it also works while the transfer stalls.
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (cancel && *cancel && !canceled) {
            canceled = true;
            reply->abort();
        }
    });
    poll.start(100);
    loop.exec();
    poll.stop();
    out.write(reply->readAll());
    out.close();
    QString err = canceled ? QStringLiteral("canceled")
                           : reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
    reply->deleteLater();
    if (err.isEmpty() && expectedSize > 0 && QFileInfo(out).size() != expectedSize)
        err = QStringLiteral("incomplete download (%1 of %2 bytes)").arg(QFileInfo(out).size()).arg(expectedSize);
    if (!err.isEmpty()) {
        QFile::remove(dest + ".part");
        throw std::runtime_error(canceled ? std::string("Download canceled") : ("Download failed: " + url + ": " + err).toStdString());
    }
    QFile::remove(dest);
    QFile::rename(dest + ".part", dest);
}

} // namespace

void downloadAiModel(const QString& id, const DownloadProgress& progress, const std::atomic<bool>* cancel)
{
    const AiModelInfo* m = findAiModel(id);
    if (!m)
        throw std::runtime_error(("Unknown model " + id).toStdString());
    QString dir = aiModelDir(id);
    QDir().mkpath(dir);
    if (!m->archive.isEmpty()) {
        QString zip = dir + "/model.zip";
        downloadFile(m->archive, zip, 0, progress, cancel);
        // Unpack with the system tool (bsdtar handles zip; unzip as fallback).
        int rc = QProcess::execute("bsdtar", {"-xf", zip, "-C", dir});
        if (rc != 0)
            rc = QProcess::execute("unzip", {"-o", "-q", zip, "-d", dir});
        QFile::remove(zip);
        if (rc != 0)
            throw std::runtime_error("Cannot unpack the model archive (needs bsdtar or unzip)");
    } else {
        // Several files: report progress over all of them.
        qint64 total = 0, before = 0;
        for (const auto& f : m->files)
            total += f.size;
        for (const auto& f : m->files) {
            if (QFileInfo(dir + "/" + f.name).size() == f.size && f.size > 0) {
                before += f.size; // already there (an earlier attempt got this far)
                continue;
            }
            auto fileProgress = [&](qint64 r, qint64 t) {
                if (progress)
                    progress(before + r, total > 0 ? total : t);
            };
            downloadFile(f.url, dir + "/" + f.name, f.size, fileProgress, cancel);
            before += f.size;
        }
    }
    if (!aiModelInstalled(id))
        throw std::runtime_error("Model files missing after download");
}

// Inference threads: one per physical core. The networks are bound by vector units, which
// SMT siblings share, so extra threads only add contention (about 12% slower measured on a
// 6-core/12-thread Ryzen). SILMA_AI_THREADS overrides.
int aiThreadCount()
{
    if (qEnvironmentVariableIsSet("SILMA_AI_THREADS"))
        return std::max(1, qEnvironmentVariableIntValue("SILMA_AI_THREADS"));
    const int logical = int(std::max(1u, std::thread::hardware_concurrency()));
    QSet<QString> cores; // distinct sibling sets = physical cores
    for (int i = 0; i < logical; ++i) {
        QFile f(QStringLiteral("/sys/devices/system/cpu/cpu%1/topology/thread_siblings_list").arg(i));
        if (!f.open(QIODevice::ReadOnly))
            return logical;
        cores.insert(QString::fromLatin1(f.readAll()).trimmed());
    }
    return cores.isEmpty() ? logical : int(cores.size());
}

namespace {

// ORT_ENABLE_ALL includes the blocked (NCHWc) layout transform, EXTENDED does not. Its layout
// conversions cost more than they save for our models, up to 4x on some machines (strided
// memory traffic that can be dramatically slow natively). SILMA_AI_LAYOUT=1 enables it (for measuring).
GraphOptimizationLevel aiGraphOptimization()
{
    return qEnvironmentVariable("SILMA_AI_LAYOUT") == QLatin1String("1") ? GraphOptimizationLevel::ORT_ENABLE_ALL
                                                                          : GraphOptimizationLevel::ORT_ENABLE_EXTENDED;
}

Ort::Env& ortEnv()
{
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "silma");
    return env;
}

} // namespace

// Configures the best available provider; returns its name.
QString configureAiProviders(Ort::SessionOptions& so)
{
    auto avail = Ort::GetAvailableProviders();
    auto has = [&](const char* n) { return std::find(avail.begin(), avail.end(), std::string(n)) != avail.end(); };
    QString forced = qEnvironmentVariable("SILMA_AI_PROVIDER").toLower();
    try {
        if ((forced.isEmpty() || forced == "migraphx") && has("MIGraphXExecutionProvider")) {
            OrtMIGraphXProviderOptions o{};
            so.AppendExecutionProvider_MIGraphX(o);
            return "MIGraphX";
        }
        if ((forced.isEmpty() || forced == "rocm") && has("ROCMExecutionProvider")) {
            OrtROCMProviderOptions o{};
            so.AppendExecutionProvider_ROCM(o);
            return "ROCm";
        }
        if ((forced.isEmpty() || forced == "cuda") && has("CUDAExecutionProvider")) {
            OrtCUDAProviderOptions o{};
            so.AppendExecutionProvider_CUDA(o);
            return "CUDA";
        }
    } catch (const Ort::Exception&) {
        // Fall through to CPU.
    }
    return "CPU";
}

QStringList aiExecutionProviders()
{
    QStringList out;
    for (const auto& p : Ort::GetAvailableProviders())
        out << QString::fromStdString(p).remove("ExecutionProvider");
    return out;
}

namespace {

std::unique_ptr<Ort::Session> makeSession(const QString& modelPath, QString* providerUsed)
{
    Ort::SessionOptions so;
    so.SetGraphOptimizationLevel(aiGraphOptimization());
    so.SetIntraOpNumThreads(aiThreadCount());
    // Debug: per-operator timings (ONNX Runtime's JSON profile, written when the run ends).
    if (qEnvironmentVariableIsSet("SILMA_AI_PROFILE"))
        so.EnableProfiling(qEnvironmentVariable("SILMA_AI_PROFILE").toStdString().c_str());
    QString provider = configureAiProviders(so);
    if (providerUsed)
        *providerUsed = provider;
    QByteArray mp = QFile::encodeName(modelPath);
    return std::make_unique<Ort::Session>(ortEnv(), mp.constData(), so);
}

// Mirrors an index into [0, len) (repeatedly, for tiles larger than the image).
int mirror(int i, int len)
{
    if (len == 1)
        return 0;
    const int period = 2 * (len - 1);
    i %= period;
    if (i < 0)
        i += period;
    return i < len ? i : period - i;
}

// Runs a model with a fixed input size over planar data (inC planes of w x h) in overlapping
// tiles, mirrored at the edges and blended with linear ramps over the overlap. The model
// outputs outC planes, `scale` times the input size.
std::vector<float> runTiled(Ort::Session& session, const std::vector<float>& in, int inC, int w, int h, int tile, int outC, int scale,
                            const AiProgress& progress, const std::atomic<bool>* cancel)
{
    Ort::AllocatorWithDefaultOptions alloc;
    std::string inName = session.GetInputNameAllocated(0, alloc).get();
    std::string outName = session.GetOutputNameAllocated(0, alloc).get();
    const int overlap = std::max(16, tile / 8);
    const int stride = tile - overlap;
    // Tile origins covering the image (last tile aligned to the edge; small images get one
    // tile, mirrored beyond the image).
    auto origins = [&](int len) {
        std::vector<int> o;
        if (len <= tile) {
            o.push_back(0);
            return o;
        }
        for (int p = 0;; p += stride) {
            if (p + tile >= len) {
                o.push_back(len - tile);
                break;
            }
            o.push_back(p);
        }
        return o;
    };
    auto ox = origins(w), oy = origins(h);
    const int W = w * scale, H = h * scale, T = tile * scale;
    std::vector<float> acc(size_t(outC) * W * H, 0.0f), wsum(size_t(W) * H, 0.0f);
    std::vector<float> input(size_t(inC) * tile * tile), weight(size_t(T) * T);
    // Blend weights: linear ramps over the overlap.
    for (int y = 0; y < T; ++y)
        for (int x = 0; x < T; ++x) {
            auto ramp = [&](int i) { return std::min(1.0f, (std::min(i, T - 1 - i) + 1.0f) / float(overlap * scale)); };
            weight[size_t(y) * T + x] = std::max(1e-3f, ramp(x) * ramp(y));
        }
    std::array<int64_t, 4> shape{1, inC, tile, tile};
    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const int total = int(ox.size() * oy.size());
    int done = 0;
    for (int ty : oy) {
        for (int tx : ox) {
            if (cancel && *cancel)
                throw std::runtime_error("Cancelled");
            for (int c = 0; c < inC; ++c)
                for (int y = 0; y < tile; ++y) {
                    const float* src = in.data() + (size_t(c) * h + mirror(ty + y, h)) * w;
                    float* dst = input.data() + (size_t(c) * tile + y) * tile;
                    for (int x = 0; x < tile; ++x)
                        dst[x] = src[mirror(tx + x, w)];
                }
            Ort::Value inT = Ort::Value::CreateTensor<float>(memInfo, input.data(), input.size(), shape.data(), shape.size());
            const char* inNames[] = {inName.c_str()};
            const char* outNames[] = {outName.c_str()};
            auto outs = session.Run(Ort::RunOptions{nullptr}, inNames, &inT, 1, outNames, 1);
            const float* o = outs[0].GetTensorData<float>();
            const int X0 = tx * scale, Y0 = ty * scale;
            for (int y = 0; y < T && Y0 + y < H; ++y)
                for (int x = 0; x < T && X0 + x < W; ++x) {
                    const float wt = weight[size_t(y) * T + x];
                    const size_t at = size_t(Y0 + y) * W + X0 + x;
                    for (int c = 0; c < outC; ++c)
                        acc[size_t(c) * W * H + at] += wt * o[(size_t(c) * T + y) * T + x];
                    wsum[at] += wt;
                }
            if (progress)
                progress(++done, total);
        }
    }
    for (int c = 0; c < outC; ++c)
        for (size_t i = 0; i < wsum.size(); ++i)
            acc[size_t(c) * W * H + i] /= std::max(wsum[i], 1e-6f);
    return acc;
}

std::vector<float> toPlanar(const ImageF& img)
{
    const size_t n = img.pixelCount();
    std::vector<float> out(3 * n);
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c)
            out[c * n + i] = img.data[i * 3 + c];
    return out;
}

ImageF fromPlanar(const std::vector<float>& planes, int w, int h)
{
    ImageF out(w, h);
    const size_t n = out.pixelCount();
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c)
            out.data[i * 3 + c] = planes[c * n + i];
    return out;
}

} // namespace

ImageF runAiInpaint(const ImageF& img, const PlaneF& mask, QString* providerUsed)
{
    const AiModelInfo m = aiInpaintModel();
    if (!aiModelInstalled(m.id))
        downloadAiModel(m.id);
    const int S = m.tileSize;
    if (img.width != S || img.height != S || mask.width != S || mask.height != S)
        throw std::runtime_error("Inpainting input must be 512x512");
    Ort::SessionOptions so;
    so.SetGraphOptimizationLevel(aiGraphOptimization());
    so.SetIntraOpNumThreads(aiThreadCount());
    QString provider = configureAiProviders(so);
    if (providerUsed)
        *providerUsed = provider;
    // Sessions are expensive to create; keep one per process.
    static std::mutex mutex;
    static std::unique_ptr<Ort::Session> session;
    std::lock_guard lock(mutex);
    if (!session) {
        QByteArray mp = QFile::encodeName(aiModelDir(m.id) + "/" + m.onnxFile);
        session = std::make_unique<Ort::Session>(ortEnv(), mp.constData(), so);
    }
    std::vector<float> in(size_t(3) * S * S), mk(size_t(S) * S);
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < S * S; ++i)
            in[size_t(c) * S * S + i] = img.data[size_t(i) * 3 + c];
    for (int i = 0; i < S * S; ++i)
        mk[i] = mask.data[i] > 0.5f ? 1.0f : 0.0f;
    std::array<int64_t, 4> s1{1, 3, S, S}, s2{1, 1, S, S};
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::array<Ort::Value, 2> inputs{Ort::Value::CreateTensor<float>(mem, in.data(), in.size(), s1.data(), 4),
                                     Ort::Value::CreateTensor<float>(mem, mk.data(), mk.size(), s2.data(), 4)};
    const char* inNames[] = {"image", "mask"};
    const char* outNames[] = {"output"};
    auto outs = session->Run(Ort::RunOptions{nullptr}, inNames, inputs.data(), 2, outNames, 1);
    const float* o = outs[0].GetTensorData<float>();
    float mx = 0;
    for (int i = 0; i < 3 * S * S; ++i)
        mx = std::max(mx, o[i]);
    const float scale = mx > 2.0f ? 1.0f / 255.0f : 1.0f; // some exports output 0..255
    ImageF out(S, S);
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < S * S; ++i)
            out.data[size_t(i) * 3 + c] = std::clamp(o[size_t(c) * S * S + i] * scale, 0.0f, 1.0f);
    return out;
}

namespace {

// Scale making the mean of `out` that of `in` (the RawNIND models output at an arbitrary gain).
// `outWeights`: weights of the output planes in that mean (to match a mosaic, where green
// counts twice).
float matchGain(const std::vector<float>& in, const std::vector<float>& out, std::array<double, 3> outWeights = {1, 1, 1})
{
    double a = 0, b = 0;
    for (float v : in)
        a += v;
    const size_t n = out.size() / 3;
    for (int c = 0; c < 3; ++c) {
        double s = 0;
        for (size_t i = 0; i < n; ++i)
            s += out[c * n + i];
        b += outWeights[c] * s;
    }
    a /= double(in.size());
    b /= double(n) * (outWeights[0] + outWeights[1] + outWeights[2]);
    return std::abs(b) > 1e-12 ? float(a / b) : 1.0f;
}

// Bayer variant: the region `r` of the mosaic packed into R, G1, G2, B planes at half size;
// outputs camera RGB (not white-balanced), r-sized. Empty when the sensor is not a usable
// Bayer one.
ImageF denoiseBayer(Ort::Session& session, const BayerMosaic& m, const QRect& r, int tile, const AiProgress& progress, const std::atomic<bool>* cancel)
{
    // Offset of an RGGB quad (the model expects red at the top left).
    int ry = -1, rx = -1;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            if (m.color[y][x] == 0)
                ry = y, rx = x;
    if (ry < 0 || m.color[ry][1 - rx] != 1 || m.color[1 - ry][rx] != 1 || m.color[1 - ry][1 - rx] != 2)
        return {};
    // Whole quads covering the region.
    auto span = [](int from, int len, int off, int size, int& start, int& count) {
        start = std::max(off, off + 2 * int(std::floor((from - off) / 2.0)));
        count = std::min((from + len - start + 1) / 2, (size - start) / 2);
    };
    int x0, y0, pw, ph;
    span(r.x(), r.width(), rx, m.width, x0, pw);
    span(r.y(), r.height(), ry, m.height, y0, ph);
    if (pw < 16 || ph < 16)
        return {};
    const size_t pn = size_t(pw) * ph;
    std::vector<float> packed(4 * pn);
    for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) {
            const float* q = m.data.data() + size_t(y0 + 2 * y) * m.width + x0 + 2 * x;
            const size_t i = size_t(y) * pw + x;
            packed[i] = q[0];
            packed[pn + i] = q[1];
            packed[2 * pn + i] = q[m.width];
            packed[3 * pn + i] = q[m.width + 1];
        }
    std::vector<float> den = runTiled(session, packed, 4, pw, ph, tile, 3, 2, progress, cancel);
    const float gain = matchGain(packed, den, {1, 2, 1});
    // Back to the region; rows and columns the quads leave out are repeated.
    const int W = 2 * pw, H = 2 * ph;
    const size_t n = size_t(W) * H;
    ImageF out(r.width(), r.height());
#pragma omp parallel for schedule(static)
    for (int y = 0; y < r.height(); ++y) {
        const int sy = std::clamp(r.y() + y - y0, 0, H - 1);
        for (int x = 0; x < r.width(); ++x) {
            const size_t i = size_t(sy) * W + std::clamp(r.x() + x - x0, 0, W - 1);
            for (int c = 0; c < 3; ++c)
                out.px(x, y)[c] = den[c * n + i] * gain;
        }
    }
    return out;
}

} // namespace

struct AiDenoiser::Impl {
    const AiModelInfo* model = nullptr;
    const RawImage* raw = nullptr;
    BayerMosaic mosaic; // empty: the linear variant on raw->image
    std::unique_ptr<Ort::Session> session;
    QString provider;
};

AiDenoiser::AiDenoiser(const QString& modelId, const QString& path, const RawImage& raw) : d(std::make_unique<Impl>())
{
    d->model = findAiModel(modelId);
    if (!d->model || d->model->linearOnnxFile.isEmpty())
        throw std::runtime_error(("Not a denoise model: " + modelId).toStdString());
    if (!aiModelInstalled(modelId))
        throw std::runtime_error("Model not installed");
    d->raw = &raw;
    if (!raw.halfSize && !qEnvironmentVariableIsSet("SILMA_AI_FORCE_LINEAR")) {
        d->mosaic = decodeBayerMosaic(path);
        if (d->mosaic.width != raw.image.width || d->mosaic.height != raw.image.height)
            d->mosaic = {};
    }
    const QString file = d->mosaic.width ? d->model->onnxFile : d->model->linearOnnxFile;
    d->session = makeSession(aiModelDir(modelId) + "/" + file, &d->provider);
}

AiDenoiser::~AiDenoiser() = default;

QString AiDenoiser::provider() const { return d->provider; }

ImageF AiDenoiser::denoise(const QRect& rectIn, const AiProgress& progress, const std::atomic<bool>* cancel)
{
    const RawImage& raw = *d->raw;
    const QRect rect = rectIn.intersected(QRect(0, 0, raw.image.width, raw.image.height));
    if (rect.isEmpty())
        return {};
    const float mul[3] = {float(raw.color.appliedMul[0]), float(raw.color.appliedMul[1]), float(raw.color.appliedMul[2])};
    ImageF den;
    if (d->mosaic.width) {
        den = denoiseBayer(*d->session, d->mosaic, rect, d->model->tileSize, progress, cancel);
        // White balance and clip like the decoder does.
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < den.pixelCount(); ++i)
            for (int c = 0; c < 3; ++c)
                den.data[i * 3 + c] = std::clamp(den.data[i * 3 + c] * mul[c], 0.0f, 1.0f);
        if (!den.empty())
            return den;
    }
    // Other sensors: the linear variant on the demosaiced image in linear Rec.2020.
    if (d->mosaic.width) {
        // A Bayer sensor with an unusable pattern: load the linear variant after all.
        d->mosaic = {};
        d->session = makeSession(aiModelDir(d->model->id) + "/" + d->model->linearOnnxFile, &d->provider);
    }
    const Mat3 toRec = CameraColor(raw.color).camToRec2020(Vec3{raw.color.asShotMul[0], raw.color.asShotMul[1], raw.color.asShotMul[2]});
    const Mat3 fromRec = matInverse(toRec);
    ImageF rec(rect.width(), rect.height());
#pragma omp parallel for schedule(static)
    for (int y = 0; y < rect.height(); ++y)
        for (int x = 0; x < rect.width(); ++x) {
            const float* p = raw.image.px(rect.x() + x, rect.y() + y);
            const Vec3 v = matMul(toRec, Vec3{p[0], p[1], p[2]});
            for (int c = 0; c < 3; ++c)
                rec.px(x, y)[c] = float(v[c]);
        }
    const std::vector<float> in = toPlanar(rec);
    std::vector<float> out = runTiled(*d->session, in, 3, rec.width, rec.height, d->model->tileSize, 3, 1, progress, cancel);
    const float gain = matchGain(in, out);
    den = fromPlanar(out, rec.width, rec.height);
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < den.pixelCount(); ++i) {
        float* p = den.data.data() + i * 3;
        const Vec3 v = matMul(fromRec, Vec3{p[0] * gain, p[1] * gain, p[2] * gain});
        for (int c = 0; c < 3; ++c)
            p[c] = std::clamp(float(v[c]), 0.0f, 1.0f);
    }
    return den;
}

void blendDenoised(ImageF& den, const ImageF& original, const QRect& rect, double strength)
{
    const float s = float(std::clamp(strength, 0.0, 1.0));
#pragma omp parallel for schedule(static)
    for (int y = 0; y < den.height; ++y) {
        const float* o = original.px(rect.x(), rect.y() + y);
        float* p = den.row(y);
        for (int x = 0; x < den.width * 3; ++x)
            p[x] = o[x] + s * (p[x] - o[x]);
    }
}

ImageF aiDenoiseRaw(const QString& modelId, const QString& path, const RawImage& raw, double strength, const AiProgress& progress,
                    const std::atomic<bool>* cancel, QString* providerUsed)
{
    AiDenoiser denoiser(modelId, path, raw);
    if (providerUsed)
        *providerUsed = denoiser.provider();
    const QRect all(0, 0, raw.image.width, raw.image.height);
    ImageF den = denoiser.denoise(all, progress, cancel);
    blendDenoised(den, raw.image, all, strength);
    return den;
}

} // namespace silma
