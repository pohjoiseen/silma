# Ideas for later

## AI denoise on integrated GPUs (WebGPU)

ONNX Runtime's AMD GPU paths (ROCm, MIGraphX) do not support integrated Radeons (tried on a
Ryzen 5 8600G with Radeon 760M; the laptop has no discrete GPU either). ONNX Runtime's newer
**WebGPU execution provider** runs on Vulkan, which integrated GPUs support well, and might make
denoising several times faster than on the CPU, also on the go.

- Needs an ONNX Runtime built with `--use_webgpu` (Arch's packages do not include it); try in the VM first.
- Measure NAFNet width 32 and SCUNet on the 760M (SCUNet takes about 10 min per 24 MP photo on the
  CPU, NAFNet width 32 about 75-85 s). LaMa (AI heal) would benefit too.
- If it pays off: add it to `configureAiProviders()` in `src/core/AiDenoise.cpp`, in the order
  providers are tried, with a CPU fallback as now.

Lessons from the CPU work (2026-10-08) that likely apply to any backend: avoid strided channel
reductions (NAFNet's layer norm as 1x1 convolutions, see `tools/prepare_ai_models.py`) and check
per-operator time with `SILMA_AI_PROFILE` before guessing.
