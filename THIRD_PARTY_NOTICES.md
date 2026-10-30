# Third-party notices

## OpenFX

The plugin uses the OpenFX image-effect API headers in `vendor/openfx/include`.

OpenFX is Copyright (c) 2012-2024, Academy Software Foundation and contributors,
licensed under the BSD 3-Clause license. The full license text is in
`vendor/openfx/LICENSE.md`.

## NVIDIA CUDA / NVRTC

The build script uses the NVRTC runtime that ships with DaVinci Resolve (or a
local CUDA Toolkit installation) to compile our own CUDA kernel source to PTX.
NVRTC is not redistributed with this project. The generated PTX contains only
Mixed Media's own shader code and is embedded in the plugin binary.

## DaVinci Resolve

DaVinci Resolve is a trademark of Blackmagic Design Pty. Ltd. This project is an
independent OpenFX plugin and is not affiliated with or endorsed by Blackmagic
Design.

## Effect.app

The control surface and visual goals are inspired by the public Paper Cutout and
Halftone Screen effects at effect.app. No Effect.app code, shader source, media,
or preset files are included in this repository or plugin. This is an
independent implementation.
