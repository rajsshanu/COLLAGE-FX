#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Straight-alpha RGBA float image. Row 0 is the bottom row, matching the
// OpenFX/CPU renderer convention used by Mixed Media.
struct ImageRGBA
{
    int width = 0;
    int height = 0;
    std::vector<float> pixels;

    bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
    void clear()
    {
        width = 0;
        height = 0;
        pixels.clear();
    }
};

// Loads PNG/JPEG/BMP/TIFF through Windows Imaging Component. If previewOut is
// non-null, a high-quality downscale is produced that fits inside
// previewMaxW x previewMaxH.
bool LoadImageFile(const wchar_t* path, ImageRGBA& out, int previewMaxW,
                   int previewMaxH, ImageRGBA* previewOut, std::wstring* error);

// Writes a straight-alpha 32-bit PNG.
bool SavePngFile(const wchar_t* path, const ImageRGBA& image, std::wstring* error);

// Writes top-down 32-bit BGRA pixels to a PNG. Used by the GDI+ preview path
// and available for diagnostics.
bool SaveBgraPng(const wchar_t* path, const uint8_t* bgra, int width,
                 int height, std::wstring* error);

// Area-friendly high-quality downscale used for previews built in memory.
bool ScaleImage(const ImageRGBA& src, int maxW, int maxH, ImageRGBA& out,
                std::wstring* error);
