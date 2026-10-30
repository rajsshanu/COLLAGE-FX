Mixed Media Studio 1.0
======================

Paper Cutout + Halftone Screen for Windows, with the same look and controls as
the Mixed Media OpenFX plugin for DaVinci Resolve.

WHAT IT DOES
------------
Mixed Media Studio is a standalone app. Open a still, tweak the two effect
tabs, and export a PNG. It also batch-processes a folder of images.

It exists because OpenFX plugins only work in OpenFX hosts such as DaVinci
Resolve and Fusion. Adobe After Effects, Adobe Premiere Pro and Wondershare
Filmora do not load OpenFX plugins. The workflow for those editors is:

  1. Open an image, or export a PNG sequence from your editor.
  2. Use Export PNG or Batch folder in Mixed Media Studio.
  3. Import the rendered PNGs back into After Effects, Premiere Pro, Filmora,
     or any other editor.

Run MixedMediaSetup.exe to install the Studio app, Start Menu shortcut,
uninstaller and optionally the native OpenFX plugin. The plugin is copied to:

  C:\Program Files\Common Files\OFX\Plugins\MixedMedia.ofx.bundle

After restarting DaVinci Resolve, the effects appear under:

  Effects > OpenFX > Mixed Media > Paper Cutout
  Effects > OpenFX > Mixed Media > Halftone Screen

QUICK START
-----------
- Drag a PNG, JPEG, BMP or TIFF onto the window, or click Open image.
- Use the Paper Cutout and Halftone Screen tabs.
- Turn on "Chain Halftone -> Paper" to run both effects in that order.
- Click Export PNG to render the current frame at full resolution.
- Click Batch to apply the current look to every image in a folder.
  Output files are named name_mixedmedia.png.

KEYBOARD
--------
Ctrl+O   Open image
Ctrl+S   Export PNG
Ctrl+B   Batch folder
Ctrl+R   Reset all settings

NOTES
-----
- The app uses the CPU renderer, so it works on any Windows 10/11 x64 PC.
- The OpenFX plugin can use CUDA in Resolve when Resolve is set to CUDA GPU
  processing. The plugin and the Studio app share the same effect math.
- Settings are stored in %APPDATA%\MixedMediaStudio\settings.ini.
- The plugin log is %LOCALAPPDATA%\MixedMediaOFX\mixedmedia.log.
- Silent install: MixedMediaSetup.exe /S /NOLAUNCH
- Silent uninstall: "C:\Program Files\Mixed Media Studio\Uninstall.exe"
  /uninstall /silent

LICENCE
-------
MIT licensed. OpenFX headers are BSD-3-Clause. See LICENSE and
THIRD_PARTY_NOTICES.md in the installation folder.
