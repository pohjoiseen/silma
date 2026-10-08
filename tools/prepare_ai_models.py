#!/usr/bin/env python3
"""Prepares the NAFNet width-32 denoise models Silmä uses, from the official weights.

No ONNX export of NAFNet-SIDD-width32 is published, so this downloads the official PyTorch
checkpoint and architecture code (pinned, checksummed), exports the network to ONNX, and makes
an 8-bit version calibrated on tiles from your own raw files (encoded by Silmä exactly as the
denoiser feeds them). Run through the build: cmake --build <dir> --target ai-models
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

# NAFNet-SIDD-width32.pth as released by megvii-research (MIT); the official copy is on Google
# Drive, so a Hugging Face mirror is used (identical in two independent mirrors).
WEIGHTS_URL = "https://huggingface.co/nyanko7/nafnet-models/resolve/08e8c701eb662688cccda3bf3074e90e902bcd3b/NAFNet-SIDD-width32.pth"
WEIGHTS_SHA256 = "89c70e808d1783b6c07911306e106aaf0d4f7f3da8c61078b99ff7f8929a26f4"
ARCH_COMMIT = "2b4af71ebe098a92a75910c233a3965a3e93ede4"
ARCH_BASE = f"https://raw.githubusercontent.com/megvii-research/NAFNet/{ARCH_COMMIT}/basicsr/models/archs/"
ARCH_FILES = ["NAFNet_arch.py", "arch_util.py", "local_arch.py"]

TILE = 256  # Silmä feeds 256x256 tiles (AiModelInfo::tileSize)
RAW_EXTENSIONS = {".nef", ".nrw", ".dng", ".cr2", ".cr3", ".arw", ".raf", ".orf", ".rw2", ".pef", ".srw"}


def log(msg):
    print(f"[ai-models] {msg}", flush=True)


def download(url, dest, sha256=None):
    if dest.exists() and (sha256 is None or file_sha256(dest) == sha256):
        return dest
    log(f"downloading {url}")
    tmp = dest.with_suffix(dest.suffix + ".part")
    with urllib.request.urlopen(url, timeout=60) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    if sha256 is not None and file_sha256(tmp) != sha256:
        tmp.unlink()
        sys.exit(f"checksum mismatch for {url}")
    tmp.replace(dest)
    return dest


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def model_dir(silma):
    # Ask Silmä itself, so SILMA_MODEL_DIR and XDG paths are handled the same way.
    out = subprocess.run([silma, "ai-models"], capture_output=True, text=True, check=True).stdout
    m = re.search(r"^Model directory: (.*)$", out, re.M)
    if not m:
        sys.exit("cannot find the model directory in `silma ai-models` output")
    return Path(m.group(1).strip())


def build_network(work):
    arch = work / "arch"
    arch.mkdir(exist_ok=True)
    for name in ARCH_FILES:
        download(ARCH_BASE + name, arch / name)
        # The files import each other through the basicsr package; make them standalone.
        text = (arch / name).read_text()
        text = re.sub(r"^from basicsr\.models\.archs\.(\w+) import", r"from \1 import", text, flags=re.M)
        text = re.sub(r"^from basicsr.*$", "", text, flags=re.M)
        (arch / name).write_text(text)
    sys.path.insert(0, str(arch))
    import torch
    from NAFNet_arch import NAFNet

    # Configuration of the released SIDD width-32 model (options/test/SIDD/NAFNet-width32.yml).
    net = NAFNet(img_channel=3, width=32, middle_blk_num=12, enc_blk_nums=[2, 2, 4, 8], dec_blk_nums=[2, 2, 2, 2])
    weights = download(WEIGHTS_URL, work / "NAFNet-SIDD-width32.pth", WEIGHTS_SHA256)
    state = torch.load(weights, map_location="cpu", weights_only=True)
    net.load_state_dict(state.get("params", state), strict=True)
    return net.eval()


def export_onnx(net, path):
    import numpy as np
    import onnxruntime as ort
    import torch

    x = torch.rand(1, 3, TILE, TILE)
    torch.onnx.export(net, x, str(path), input_names=["image"], output_names=["output"], opset_version=17, dynamo=False)
    with torch.no_grad():
        ref = net(x).numpy()
    got = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"]).run(None, {"image": x.numpy()})[0]
    diff = float(np.abs(ref - got).max())
    if diff > 1e-3:
        sys.exit(f"ONNX export differs from PyTorch (max {diff})")
    log(f"exported {path.name} (matches PyTorch within {diff:.1e})")


def calibration_tiles(silma, raws, work):
    files = sorted(p for p in Path(raws).rglob("*") if p.suffix.lower() in RAW_EXTENSIONS)
    if not files:
        sys.exit(f"no raw files found in {raws} (set SILMA_CALIBRATION_RAWS to a folder with some of your raws)")
    # Up to 8 files spread over the collection, 6 tiles each.
    step = max(1, len(files) // 8)
    picked = files[::step][:8]
    tiles = work / "calibration"
    shutil.rmtree(tiles, ignore_errors=True)
    log(f"calibration tiles from {len(picked)} raw files")
    subprocess.run([silma, "ai-calibration-tiles", *map(str, picked), "--out", str(tiles), "--per-image", "6"], check=True)
    return sorted(tiles.glob("*.f32"))


def fold(src, dst):
    """Constant-folds the graph (ONNX Runtime's hardware-independent basic optimizations).

    The PyTorch export computes tensor shapes at run time (Shape/Gather/Reshape chains). Left in,
    the 8-bit graph wraps them in extra quantize/dequantize steps that ONNX Runtime cannot fuse
    (1.1x instead of 1.6x faster than full precision); newer quantization tools no longer fold them
    themselves, so this is done explicitly.
    """
    import onnxruntime as ort

    o = ort.SessionOptions()
    o.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
    o.optimized_model_filepath = str(dst)
    ort.InferenceSession(str(src), o, providers=["CPUExecutionProvider"])


def quantize(fp32, int8, tiles, work):
    import numpy as np
    import onnxruntime as ort
    from onnxruntime.quantization import CalibrationDataReader, CalibrationMethod, QuantFormat, QuantType, quantize_static
    from onnxruntime.quantization.shape_inference import quant_pre_process

    data = [np.fromfile(t, dtype=np.float32).reshape(1, 3, TILE, TILE) for t in tiles]

    class Reader(CalibrationDataReader):
        def __init__(self):
            self.it = iter(data)

        def get_next(self):
            x = next(self.it, None)
            return None if x is None else {"image": x}

    pre = work / "pre.onnx"
    quant_pre_process(str(fp32), str(pre), skip_symbolic_shape=True, skip_optimization=True)
    # 8-bit weights (per channel) and activations: about 2x faster on CPUs with VNNI.
    quantize_static(str(pre), str(int8), Reader(), quant_format=QuantFormat.QDQ, per_channel=True,
                    activation_type=QuantType.QUInt8, weight_type=QuantType.QInt8, calibrate_method=CalibrationMethod.MinMax)
    # How close the 8-bit model stays to the full-precision one.
    a = ort.InferenceSession(str(fp32), providers=["CPUExecutionProvider"])
    b = ort.InferenceSession(str(int8), providers=["CPUExecutionProvider"])
    psnr = [10 * np.log10(1 / np.mean((a.run(None, {"image": x})[0] - b.run(None, {"image": x})[0]) ** 2)) for x in data[:6]]
    log(f"quantized {int8.name}: {np.median(psnr):.1f} dB from full precision")


def install(src, dest_dir, name, about):
    dest_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dest_dir / (name + ".part"))
    os.replace(dest_dir / (name + ".part"), dest_dir / name)
    (dest_dir / "README.txt").write_text(about)
    log(f"installed {dest_dir / name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--silma", required=True, help="the silma executable")
    ap.add_argument("--raws", required=True, help="folder with raw files for calibrating the 8-bit model")
    ap.add_argument("--work", required=True, help="working folder (downloads, intermediate files)")
    args = ap.parse_args()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    import onnx
    import onnxruntime
    import torch
    log(f"Python {sys.version.split()[0]}, PyTorch {torch.__version__}, ONNX {onnx.__version__}, ONNX Runtime {onnxruntime.__version__}")

    net = build_network(work)
    exported = work / "nafnet_w32_export.onnx"
    export_onnx(net, exported)
    fp32 = work / "nafnet_w32.onnx"
    fold(exported, fp32)
    int8 = work / "nafnet_w32_int8.onnx"
    quantize(fp32, int8, calibration_tiles(args.silma, args.raws, work), work)

    models = model_dir(args.silma)
    origin = (f"NAFNet-SIDD-width32 by megvii-research (MIT), https://github.com/megvii-research/NAFNet\n"
              f"Weights: {WEIGHTS_URL} (sha256 {WEIGHTS_SHA256})\nArchitecture code: commit {ARCH_COMMIT}\n")
    install(fp32, models / "nafnet-sidd-w32", "nafnet_w32.onnx", origin + "Exported to ONNX by tools/prepare_ai_models.py.\n")
    install(int8, models / "nafnet-sidd-w32-int8", "nafnet_w32_int8.onnx",
            origin + "Exported to ONNX and quantized to 8 bits (calibrated on local raw files) by tools/prepare_ai_models.py.\n")


if __name__ == "__main__":
    main()
