#include "ImageIO.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

std::wstring HResultText(HRESULT hr)
{
    wchar_t buf[96];
    swprintf(buf, 96, L"0x%08X", (unsigned int)hr);
    return buf;
}

void SetError(std::wstring* error, const wchar_t* what, HRESULT hr)
{
    if (!error)
        return;
    *error = what;
    *error += L" (";
    *error += HResultText(hr);
    *error += L")";
}

void BgraToFloat(const BYTE* bgra, int width, int height, UINT stride,
                 ImageRGBA& out)
{
    out.width = width;
    out.height = height;
    out.pixels.assign((size_t)width * (size_t)height * 4, 0.0f);
    for (int y = 0; y < height; ++y)
    {
        const BYTE* row = bgra + (size_t)(height - 1 - y) * stride;
        float* dst = out.pixels.data() + (size_t)y * (size_t)width * 4;
        for (int x = 0; x < width; ++x)
        {
            dst[0] = (float)row[(size_t)x * 4 + 2] * (1.0f / 255.0f);
            dst[1] = (float)row[(size_t)x * 4 + 1] * (1.0f / 255.0f);
            dst[2] = (float)row[(size_t)x * 4 + 0] * (1.0f / 255.0f);
            dst[3] = (float)row[(size_t)x * 4 + 3] * (1.0f / 255.0f);
            dst += 4;
        }
    }
}

} // namespace

bool LoadImageFile(const wchar_t* path, ImageRGBA& out, int previewMaxW,
                   int previewMaxH, ImageRGBA* previewOut, std::wstring* error)
{
    out.clear();
    if (previewOut)
        previewOut->clear();
    if (error)
        error->clear();

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        SetError(error, L"Could not create the Windows imaging factory", hr);
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand,
                                            &decoder);
    if (FAILED(hr))
    {
        SetError(error, L"Could not open the image", hr);
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
    {
        SetError(error, L"Could not read the first image frame", hr);
        return false;
    }

    UINT width = 0, height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0)
    {
        SetError(error, L"The image has no pixels", hr);
        return false;
    }
    if ((uint64_t)width * (uint64_t)height > 64000000ULL)
    {
        if (error)
            *error = L"The image is larger than 64 megapixels. Please "
                     L"downscale it before opening it in Mixed Media Studio.";
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr))
    {
        SetError(error, L"Could not create the pixel converter", hr);
        return false;
    }
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        SetError(error, L"Could not convert the image to RGBA", hr);
        return false;
    }

    const UINT stride = width * 4;
    std::vector<BYTE> bgra((size_t)stride * height);
    hr = converter->CopyPixels(nullptr, stride, (UINT)bgra.size(), bgra.data());
    if (FAILED(hr))
    {
        SetError(error, L"Could not read the image pixels", hr);
        return false;
    }

    BgraToFloat(bgra.data(), (int)width, (int)height, stride, out);

    if (previewOut && previewMaxW > 0 && previewMaxH > 0)
    {
        double scale = 1.0;
        scale = std::min(scale, (double)previewMaxW / (double)width);
        scale = std::min(scale, (double)previewMaxH / (double)height);
        if (scale < 0.999)
        {
            UINT pw = (UINT)std::max(1, (int)std::lround(width * scale));
            UINT ph = (UINT)std::max(1, (int)std::lround(height * scale));

            ComPtr<IWICBitmapScaler> scaler;
            hr = factory->CreateBitmapScaler(&scaler);
            if (SUCCEEDED(hr))
                hr = scaler->Initialize(converter.Get(), pw, ph,
                                        WICBitmapInterpolationModeFant);
            if (SUCCEEDED(hr))
            {
                const UINT ps = pw * 4;
                std::vector<BYTE> scaled((size_t)ps * ph);
                hr = scaler->CopyPixels(nullptr, ps, (UINT)scaled.size(),
                                        scaled.data());
                if (SUCCEEDED(hr))
                    BgraToFloat(scaled.data(), (int)pw, (int)ph, ps,
                                *previewOut);
            }
        }
        if (previewOut->empty())
        {
            // No scaling was needed, or the scaler failed. Use the full image.
            *previewOut = out;
        }
    }

    return true;
}

bool SavePngFile(const wchar_t* path, const ImageRGBA& image, std::wstring* error)
{
    if (error)
        error->clear();
    if (image.empty())
    {
        if (error)
            *error = L"There is no image to save.";
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        SetError(error, L"Could not create the Windows imaging factory", hr);
        return false;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr))
    {
        SetError(error, L"Could not create the output stream", hr);
        return false;
    }
    hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
    if (FAILED(hr))
    {
        SetError(error, L"Could not create the PNG file", hr);
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr))
    {
        SetError(error, L"Could not create the PNG encoder", hr);
        return false;
    }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr))
    {
        SetError(error, L"Could not initialise the PNG encoder", hr);
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr))
    {
        SetError(error, L"Could not create a PNG frame", hr);
        return false;
    }
    hr = frame->Initialize(props.Get());
    if (FAILED(hr))
    {
        SetError(error, L"Could not initialise the PNG frame", hr);
        return false;
    }
    hr = frame->SetSize((UINT)image.width, (UINT)image.height);
    if (FAILED(hr))
    {
        SetError(error, L"Could not set the PNG size", hr);
        return false;
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr))
    {
        SetError(error, L"The PNG encoder rejected 32-bit RGBA", hr);
        return false;
    }

    const UINT stride = (UINT)image.width * 4;
    std::vector<BYTE> bgra((size_t)stride * (size_t)image.height);
    for (int y = 0; y < image.height; ++y)
    {
        const float* src = image.pixels.data() +
                           (size_t)(image.height - 1 - y) * (size_t)image.width * 4;
        BYTE* dst = bgra.data() + (size_t)y * stride;
        for (int x = 0; x < image.width; ++x)
        {
            dst[(size_t)x * 4 + 0] =
                (BYTE)(std::max(0.0f, std::min(1.0f, src[(size_t)x * 4 + 2])) *
                           255.0f + 0.5f);
            dst[(size_t)x * 4 + 1] =
                (BYTE)(std::max(0.0f, std::min(1.0f, src[(size_t)x * 4 + 1])) *
                           255.0f + 0.5f);
            dst[(size_t)x * 4 + 2] =
                (BYTE)(std::max(0.0f, std::min(1.0f, src[(size_t)x * 4 + 0])) *
                           255.0f + 0.5f);
            dst[(size_t)x * 4 + 3] =
                (BYTE)(std::max(0.0f, std::min(1.0f, src[(size_t)x * 4 + 3])) *
                           255.0f + 0.5f);
        }
    }

    hr = frame->WritePixels((UINT)image.height, stride, (UINT)bgra.size(),
                            bgra.data());
    if (FAILED(hr))
    {
        SetError(error, L"Could not write the PNG pixels", hr);
        return false;
    }
    hr = frame->Commit();
    if (FAILED(hr))
    {
        SetError(error, L"Could not finish the PNG frame", hr);
        return false;
    }
    hr = encoder->Commit();
    if (FAILED(hr))
    {
        SetError(error, L"Could not finish the PNG file", hr);
        return false;
    }
    return true;
}

bool SaveBgraPng(const wchar_t* path, const uint8_t* bgra, int width,
                 int height, std::wstring* error)
{
    if (error)
        error->clear();
    if (!bgra || width <= 0 || height <= 0)
    {
        if (error)
            *error = L"There is no image to save.";
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        SetError(error, L"Could not create the Windows imaging factory", hr);
        return false;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr))
    {
        SetError(error, L"Could not create the output stream", hr);
        return false;
    }
    hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
    if (FAILED(hr))
    {
        SetError(error, L"Could not create the PNG file", hr);
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr))
    {
        SetError(error, L"Could not create the PNG encoder", hr);
        return false;
    }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr))
    {
        SetError(error, L"Could not initialise the PNG encoder", hr);
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr))
    {
        SetError(error, L"Could not create a PNG frame", hr);
        return false;
    }
    hr = frame->Initialize(props.Get());
    if (FAILED(hr))
    {
        SetError(error, L"Could not initialise the PNG frame", hr);
        return false;
    }
    hr = frame->SetSize((UINT)width, (UINT)height);
    if (FAILED(hr))
    {
        SetError(error, L"Could not set the PNG size", hr);
        return false;
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr))
    {
        SetError(error, L"The PNG encoder rejected 32-bit RGBA", hr);
        return false;
    }

    const UINT stride = (UINT)width * 4;
    hr = frame->WritePixels((UINT)height, stride, stride * (UINT)height,
                            const_cast<BYTE*>(bgra));
    if (FAILED(hr))
    {
        SetError(error, L"Could not write the PNG pixels", hr);
        return false;
    }
    hr = frame->Commit();
    if (FAILED(hr))
    {
        SetError(error, L"Could not finish the PNG frame", hr);
        return false;
    }
    hr = encoder->Commit();
    if (FAILED(hr))
    {
        SetError(error, L"Could not finish the PNG file", hr);
        return false;
    }
    return true;
}

bool ScaleImage(const ImageRGBA& src, int maxW, int maxH, ImageRGBA& out,
                std::wstring* error)
{
    if (error)
        error->clear();
    out.clear();
    if (src.empty() || maxW <= 0 || maxH <= 0)
    {
        if (error)
            *error = L"Nothing to scale.";
        return false;
    }

    double scale = 1.0;
    scale = std::min(scale, (double)maxW / (double)src.width);
    scale = std::min(scale, (double)maxH / (double)src.height);
    int dw = (int)std::max(1, (int)std::lround(src.width * scale));
    int dh = (int)std::max(1, (int)std::lround(src.height * scale));
    if (dw == src.width && dh == src.height)
    {
        out = src;
        return true;
    }

    out.width = dw;
    out.height = dh;
    out.pixels.assign((size_t)dw * (size_t)dh * 4, 0.0f);
    const double sx = (double)src.width / (double)dw;
    const double sy = (double)src.height / (double)dh;

    if (dw < src.width || dh < src.height)
    {
        // Box average, weighted by alpha, for clean previews.
        for (int y = 0; y < dh; ++y)
        {
            int y0 = (int)std::floor(y * sy);
            int y1 = (int)std::ceil((y + 1) * sy);
            y0 = std::max(0, std::min(src.height - 1, y0));
            y1 = std::max(y0 + 1, std::min(src.height, y1));
            for (int x = 0; x < dw; ++x)
            {
                int x0 = (int)std::floor(x * sx);
                int x1 = (int)std::ceil((x + 1) * sx);
                x0 = std::max(0, std::min(src.width - 1, x0));
                x1 = std::max(x0 + 1, std::min(src.width, x1));
                double r = 0, g = 0, b = 0, a = 0;
                int count = 0;
                for (int yy = y0; yy < y1; ++yy)
                {
                    const float* row = src.pixels.data() +
                                       (size_t)yy * (size_t)src.width * 4;
                    for (int xx = x0; xx < x1; ++xx)
                    {
                        const float* p = row + (size_t)xx * 4;
                        double av = p[3];
                        r += p[0] * av;
                        g += p[1] * av;
                        b += p[2] * av;
                        a += av;
                        ++count;
                    }
                }
                float* dst = out.pixels.data() +
                             ((size_t)y * (size_t)dw + (size_t)x) * 4;
                if (a > 1.0e-6)
                {
                    dst[0] = (float)(r / a);
                    dst[1] = (float)(g / a);
                    dst[2] = (float)(b / a);
                    dst[3] = (float)(a / (double)std::max(1, count));
                }
            }
        }
    }
    else
    {
        // Bilinear upscale for small source images.
        auto sample = [&](int x, int y) -> const float* {
            x = std::max(0, std::min(src.width - 1, x));
            y = std::max(0, std::min(src.height - 1, y));
            return src.pixels.data() +
                   ((size_t)y * (size_t)src.width + (size_t)x) * 4;
        };
        for (int y = 0; y < dh; ++y)
        {
            double fy = (y + 0.5) * sy - 0.5;
            int y0 = (int)std::floor(fy);
            double ty = fy - y0;
            for (int x = 0; x < dw; ++x)
            {
                double fx = (x + 0.5) * sx - 0.5;
                int x0 = (int)std::floor(fx);
                double tx = fx - x0;
                const float* p00 = sample(x0, y0);
                const float* p10 = sample(x0 + 1, y0);
                const float* p01 = sample(x0, y0 + 1);
                const float* p11 = sample(x0 + 1, y0 + 1);
                float* dst = out.pixels.data() +
                             ((size_t)y * (size_t)dw + (size_t)x) * 4;
                for (int c = 0; c < 4; ++c)
                {
                    double top = p00[c] + (p10[c] - p00[c]) * tx;
                    double bot = p01[c] + (p11[c] - p01[c]) * tx;
                    dst[c] = (float)(top + (bot - top) * ty);
                }
            }
        }
    }
    return true;
}
