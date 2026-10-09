# Ideas for later

## AI denoise on integrated GPUs (WebGPU)

ONNX Runtime's AMD GPU paths (ROCm, MIGraphX) do not support integrated Radeons (tried on a
Ryzen 5 8600G with Radeon 760M; the laptop has no discrete GPU either). ONNX Runtime's newer
**WebGPU execution provider** runs on Vulkan, which integrated GPUs support well, and might make
denoising several times faster than on the CPU, also on the go.

- Needs an ONNX Runtime built with `--use_webgpu` (Arch's packages do not include it); try in the VM first.
- Measure RawNIND on the 760M (about 10 s per 24 MP photo on the CPU, so less pressing than it was
  with NAFNet at 75-85 s). LaMa (AI heal) would benefit too.
- If it pays off: add it to `configureAiProviders()` in `src/core/AiDenoise.cpp`, in the order
  providers are tried, with a CPU fallback as now.

Lessons from the CPU work (2026-10-08) that likely apply to any backend: avoid strided channel
reductions (NAFNet's layer norm had to become 1x1 convolutions; see git history) and check
per-operator time with `SILMA_AI_PROFILE` before guessing.
