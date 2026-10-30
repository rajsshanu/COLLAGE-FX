#!/usr/bin/env python3
"""Compile cuda/MixedMediaKernels.cu to embedded PTX using Resolve's NVRTC.

The plugin uses the CUDA driver API at runtime (nvcuda.dll). No CUDA toolkit,
CUDA runtime DLL or nvcc installation is needed either to build or to run it.
"""
from __future__ import annotations

import argparse
import ctypes
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CUDA_SOURCE = ROOT / "cuda" / "MixedMediaKernels.cu"
SRC_DIR = ROOT / "src"
OUT_PTX = ROOT / "generated" / "MixedMediaKernels.ptx"
OUT_HEADER = ROOT / "generated" / "MixedMediaPTX.h"

DEFAULT_NVRTC_CANDIDATES = [
    Path(r"C:\Program Files\Blackmagic Design\DaVinci Resolve\nvrtc64_120_0.dll"),
    Path(r"C:\Program Files\Blackmagic Design\DaVinci Resolve\nvrtc64_130_0.dll"),
    Path(r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin\nvrtc64_130_0.dll"),
    Path(r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\bin\nvrtc64_120_0.dll"),
    Path(r"C:\Windows\System32\nvrtc64_120_0.dll"),
    Path(r"C:\Windows\System32\nvrtc64_130_0.dll"),
]


def find_nvrtc(explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return p
        raise FileNotFoundError(p)
    env = os.environ.get("MIXEDMEDIA_NVRTC")
    if env and Path(env).is_file():
        return Path(env)
    for p in DEFAULT_NVRTC_CANDIDATES:
        if p.is_file():
            return p
    for folder in os.environ.get("PATH", "").split(os.pathsep):
        try:
            for p in Path(folder).glob("nvrtc64_*.dll"):
                return p
        except OSError:
            pass
    raise FileNotFoundError(
        "Could not find nvrtc64_*.dll. Install DaVinci Resolve or pass --nvrtc PATH."
    )


class NVRTC:
    def __init__(self, dll_path: Path):
        if hasattr(os, "add_dll_directory"):
            self._dll_dir = os.add_dll_directory(str(dll_path.parent))
        os.environ["PATH"] = str(dll_path.parent) + os.pathsep + os.environ.get("PATH", "")
        for name in (
            "nvrtc-builtins64_130.dll",
            "nvrtc-builtins64_128.dll",
            "nvrtc-builtins64_120.dll",
            "nvrtc-builtins64_110.dll",
            "nvrtc-builtins64_80.dll",
        ):
            builtins = dll_path.parent / name
            if builtins.is_file():
                try:
                    ctypes.WinDLL(str(builtins))
                    break
                except OSError:
                    pass
        self.dll = ctypes.WinDLL(str(dll_path))
        self._bind()

    def _fn(self, name, restype, *argtypes):
        fn = getattr(self.dll, name)
        fn.restype = restype
        fn.argtypes = list(argtypes)
        return fn

    def _bind(self):
        c_void_p = ctypes.c_void_p
        c_int = ctypes.c_int
        c_size_t = ctypes.c_size_t
        self.create = self._fn(
            "nvrtcCreateProgram", c_int, ctypes.POINTER(c_void_p), ctypes.c_char_p,
            ctypes.c_char_p, c_int, ctypes.POINTER(ctypes.c_char_p),
            ctypes.POINTER(ctypes.c_char_p),
        )
        self._compile = self._fn(
            "nvrtcCompileProgram", c_int, c_void_p, c_int, ctypes.POINTER(ctypes.c_char_p)
        )
        self.log_size = self._fn("nvrtcGetProgramLogSize", c_int, c_void_p, ctypes.POINTER(c_size_t))
        self.log = self._fn("nvrtcGetProgramLog", c_int, c_void_p, ctypes.c_char_p)
        self.ptx_size = self._fn("nvrtcGetPTXSize", c_int, c_void_p, ctypes.POINTER(c_size_t))
        self.ptx = self._fn("nvrtcGetPTX", c_int, c_void_p, ctypes.c_char_p)
        self.destroy = self._fn("nvrtcDestroyProgram", c_int, ctypes.POINTER(c_void_p))

    def compile(self, source: str, name: str, options: list[str]) -> tuple[bytes, str]:
        prog = ctypes.c_void_p()
        src = source.encode("utf-8")
        name_b = name.encode("utf-8")
        c_opts = (ctypes.c_char_p * len(options))(*[o.encode("utf-8") for o in options])
        res = self.create(ctypes.byref(prog), src, name_b, 0, None, c_opts)
        if res != 0:
            raise RuntimeError(f"nvrtcCreateProgram failed: {res}")
        try:
            res = self._compile(prog, len(options), c_opts)
            log_size = ctypes.c_size_t()
            self.log_size(prog, ctypes.byref(log_size))
            log_text = ""
            if log_size.value > 1:
                buf = ctypes.create_string_buffer(log_size.value)
                self.log(prog, buf)
                log_text = buf.value.decode("utf-8", "replace")
            if res != 0:
                raise RuntimeError(f"nvrtcCompileProgram failed ({res}):\n{log_text}")
            size = ctypes.c_size_t()
            self.ptx_size(prog, ctypes.byref(size))
            buf = ctypes.create_string_buffer(size.value)
            self.ptx(prog, buf)
            return bytes(buf.raw[: size.value]), log_text
        finally:
            self.destroy(ctypes.byref(prog))


def write_header(ptx: bytes) -> None:
    OUT_HEADER.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "// Auto-generated by tools/compile_ptx.py. Do not edit.",
        "#pragma once",
        "static const unsigned char kMixedMediaPTX[] = {",
    ]
    for i in range(0, len(ptx), 12):
        chunk = ptx[i : i + 12]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("    0x00")
    lines.append("};")
    lines.append("static const unsigned long long kMixedMediaPTXSize = sizeof(kMixedMediaPTX);")
    lines.append("")
    OUT_HEADER.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--nvrtc", help="Path to nvrtc64_*.dll")
    ap.add_argument("--arch", default="compute_75", help="PTX virtual architecture (default compute_75)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    nvrtc_path = find_nvrtc(args.nvrtc)
    print(f"[ptx] NVRTC: {nvrtc_path}")
    source_text = CUDA_SOURCE.read_text(encoding="utf-8")
    options = [
        f"--gpu-architecture={args.arch}",
        "--std=c++14",
        f"-I{SRC_DIR}",
        "--restrict",
        "--prec-div=true",
        "--prec-sqrt=true",
    ]
    compiler = NVRTC(nvrtc_path)
    ptx, log = compiler.compile(source_text, "MixedMediaKernels.cu", options)
    if args.verbose and log.strip():
        print(log)
    OUT_PTX.parent.mkdir(parents=True, exist_ok=True)
    OUT_PTX.write_bytes(ptx)
    write_header(ptx)
    print(f"[ptx] wrote {OUT_PTX} ({len(ptx):,} bytes)")
    print(f"[ptx] wrote {OUT_HEADER} ({OUT_HEADER.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
