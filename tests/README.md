# Tests and previews

`build.ps1` compiles and runs the two important checks:

- `ofx_host_test.exe` — a minimal OFX host that loads the real `.ofx` binary,
  describes both plugins, creates instances, exercises keyframes, renders
  through the CPU fallback, and exercises the real CUDA OFX path with actual
  device buffers and a supplied CUDA stream.
- `gpu_parity.exe` — creates a CUDA context, allocates device source/output
  buffers, calls the same `CudaBackend` entry points used by the plugin, and
  compares the frame against the CPU reference.

The host test uses 128×128 frames. The GPU test can be run at any size:

```powershell
.\build\gpu_parity.exe 1920 1080 tests\data\portrait.rgba
```

If the raw input is absent it generates a synthetic gradient, so a fresh
checkout can run the test without test media.

## Development renderer

`render_cli.exe` renders the CPU reference to a 24-bit BMP:

```powershell
.\build\render_cli.exe --effect paper --width 900 --height 900 `
  --in tests\data\frame.rgba --out paper.bmp

.\build\render_cli.exe --effect halftone --width 900 --height 900 `
  --in tests\data\frame.rgba --out halftone.bmp
```

The input is tightly packed, bottom-up float RGBA. Use
`tests/make_preview_input.py` (requires Pillow) to generate the synthetic poster
used in the README, then:

```powershell
python tests\make_preview_input.py
.\build\render_cli.exe --effect paper --width 1200 --height 1200 `
  --in tests\data\synthetic.rgba --out tests\data\paper_doc.bmp `
  --columns 6 --rows 6 --missing 22 --seed 31 --rotation 7 `
  --sizeVar 0.25 --widthVar 0.38 --heightVar 0.38 --uvShift 22 `
  --colorVar 0.3 --tear 22 --chunk 90 --rough 0.7 --white 5 `
  --uneven 0.45 --shadow 0.45 --bgImage 0
```

The renderer is deliberately a CPU build so it can run without Resolve. It is a
development tool, not part of the installed plugin.
