// Mixed Media Studio - standalone Windows UI for the Mixed Media CPU renderer.
//
// OpenFX is only supported by OFX hosts (DaVinci Resolve, Fusion and similar).
// This app exists so editors that do not load OFX - After Effects, Premiere Pro,
// Filmora - can still use the same Paper Cutout and Halftone Screen looks by
// exporting PNG stills or a PNG sequence and importing them into their editor.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <gdiplus.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../src/CpuRender.h"
#include "../src/EffectMath.h"
#include "ImageIO.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;

namespace {

const wchar_t* kAppName = L"Mixed Media Studio";
const wchar_t* kVersion = L"1.0.0";
const wchar_t* kMainClass = L"MixedMediaStudio.Main";
const wchar_t* kPageClass = L"MixedMediaStudio.Page";
const wchar_t* kSliderClass = L"MixedMediaStudio.Slider";

// Cool, modern palette inspired by a soft studio rack UI.
const COLORREF kBgTop = RGB(232, 238, 248);
const COLORREF kBgBottom = RGB(204, 216, 234);
const COLORREF kCard = RGB(249, 250, 254);
const COLORREF kCardAlt = RGB(237, 241, 249);
const COLORREF kInk = RGB(28, 27, 45);
const COLORREF kMuted = RGB(112, 118, 140);
const COLORREF kLine = RGB(216, 222, 234);
const COLORREF kAccent = RGB(124, 92, 255);
const COLORREF kAccentDark = RGB(94, 63, 220);
const COLORREF kAccentSoft = RGB(234, 229, 255);
const COLORREF kDark = RGB(30, 28, 49);
const COLORREF kDarkAlt = RGB(39, 36, 62);
const COLORREF kPreview = RGB(24, 23, 40);
const COLORREF kPreviewLine = RGB(43, 40, 66);
const COLORREF kGreen = RGB(64, 196, 140);
const COLORREF kCyan = RGB(80, 200, 220);
const COLORREF kMagenta = RGB(235, 100, 170);
const COLORREF kGold = RGB(240, 186, 78);
const COLORREF kTeal = RGB(80, 200, 220);

const int kPreviewMaxW = 1600;
const int kPreviewMaxH = 1000;

enum ControlId : int
{
    IDC_OPEN = 1000,
    IDC_EXPORT,
    IDC_BATCH,
    IDC_ABOUT,
    IDC_RESET,
    IDC_CHAIN,
    IDC_ORIGINAL,
    IDC_TAB_PAPER,
    IDC_TAB_HALFTONE,
    IDC_BGIMAGE = 1020,
    IDC_CHAN_C = 1030,
    IDC_CHAN_M,
    IDC_CHAN_Y,
    IDC_CHAN_K,
    IDC_PATTERN_BASE = 1060,
    IDC_SECTION_BASE = 1500,
    IDC_SLIDER_BASE = 2000
};

const UINT_PTR kPreviewTimer = 1;
const int kPreviewDelayMs = 120;
const UINT kSliderSetPos = WM_APP + 10;

struct PaperSettings
{
    double columns = 6.0;
    double rows = 5.0;
    double missing = 16.0;
    double seed = 7.0;
    double rotation = 5.0;
    double sizeVar = 0.18;
    double widthVar = 0.28;
    double heightVar = 0.28;
    double uvShift = 14.0;
    double colorVar = 0.24;
    double tear = 16.0;
    double chunk = 60.0;
    double roughness = 0.58;
    double white = 4.0;
    double unevenness = 0.35;
    double shadow = 0.35;
    bool bgImage = true;
    double bgR = 0.96;
    double bgG = 0.94;
    double bgB = 0.90;
};

struct HalftoneSettings
{
    double pattern = 0.0;
    double frequency = 95.0;
    double roughness = 0.18;
    double fuzziness = 0.22;
    double fiber = 0.25;
    double inkTexture = 0.20;
    double density = 1.0;
    bool useC = true;
    bool useM = true;
    bool useY = true;
    bool useK = true;
    double angle[4] = { 15.0, 75.0, 0.0, 45.0 };
    double offsetX[4] = { 0.0, 0.0, 0.0, 0.0 };
    double offsetY[4] = { 0.0, 0.0, 0.0, 0.0 };
};

struct Settings
{
    PaperSettings paper;
    HalftoneSettings ht;
    bool chain = false;
};

enum class PageKind { Paper, Halftone };

enum class ButtonStyle { Secondary, Primary, Ghost };

struct Slider
{
    HWND track = nullptr;
    double* value = nullptr;
    double minValue = 0.0;
    double maxValue = 1.0;
    double step = 0.01;
    int decimals = 2;
    std::wstring suffix;
    std::wstring labelText;
    PageKind page = PageKind::Paper;
    int col = 0;
    int row = 0;
    int id = 0;
    bool hovering = false;
    bool dragging = false;
    bool swatch = false;
};

struct ButtonInfo
{
    HWND hwnd = nullptr;
    int id = 0;
    std::wstring text;
    ButtonStyle style = ButtonStyle::Secondary;
    RECT rect = {};
};

struct App
{
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    HWND paperPage = nullptr;
    HWND halftonePage = nullptr;
    HWND tabPaper = nullptr;
    HWND tabHalftone = nullptr;
    HWND chainCheck = nullptr;
    HWND originalCheck = nullptr;
    HWND resetButton = nullptr;
    HWND bgImageCheck = nullptr;
    HWND channelC = nullptr;
    HWND channelM = nullptr;
    HWND channelY = nullptr;
    HWND channelK = nullptr;
    HWND patternChips[6] = {};
    HWND sectionPaper0 = nullptr;
    HWND sectionPaper1 = nullptr;
    HWND sectionHt0 = nullptr;
    HWND sectionHt1 = nullptr;
    HWND sectionPaper2 = nullptr;
    HWND sectionHt2 = nullptr;

    std::deque<Slider> sliders;
    std::vector<ButtonInfo> headerButtons;

    Settings settings;
    std::wstring lastDir;
    std::wstring currentPath;
    RECT savedWindowRect = {};
    bool hasSavedWindowRect = false;

    ImageRGBA fullImage;
    ImageRGBA previewSource;
    ImageRGBA previewOutput;

    std::vector<uint8_t> sourceBgra;
    std::vector<uint8_t> previewBgra;
    std::unique_ptr<Gdiplus::Bitmap> sourceBitmap;
    std::unique_ptr<Gdiplus::Bitmap> previewBitmap;

    int activeTab = 0;
    bool showOriginal = false;
    int dpi = 96;
    double previewMs = 0.0;
    bool batchRunning = false;
    bool batchCancel = false;
    std::wstring batchStatus;
    std::wstring statusMessage;

    HICON iconLarge = nullptr;
    HICON iconSmall = nullptr;
    HFONT fontTitle = nullptr;
    HFONT fontSubtitle = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontLabel = nullptr;
    HFONT fontValue = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontSection = nullptr;
    HFONT fontButton = nullptr;
    HFONT fontTab = nullptr;
    HFONT fontBadge = nullptr;

    HBRUSH brushCard = nullptr;
    HBRUSH brushCardAlt = nullptr;
    HBRUSH brushLine = nullptr;
    HBRUSH brushDark = nullptr;

    RECT headerRect = {};
    RECT panelRect = {};
    RECT previewCardRect = {};
    RECT previewHeader = {};
    RECT previewRect = {};
    RECT previewFooter = {};
    RECT statusRect = {};
    RECT segmentRect = {};
    RECT originalRect = {};
    RECT chainRect = {};
    RECT resetRect = {};
    RECT paperSwatchRect = {};
    RECT screenPreviewRect = {};
    int pageDividerX = 0;

    std::wstring initialPath;
};

App g_app;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

int Dp(int value)
{
    return MulDiv(value, g_app.dpi, 96);
}

double ClampD(double value, double lo, double hi)
{
    return std::max(lo, std::min(hi, value));
}

int ClampI(int value, int lo, int hi)
{
    return std::max(lo, std::min(hi, value));
}

Gdiplus::Color Gd(COLORREF c, BYTE alpha = 255)
{
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

void ShowError(HWND owner, const std::wstring& message)
{
    MessageBoxW(owner, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
}

std::wstring GetAppDataDir()
{
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, path)))
    {
        std::wstring dir = path;
        dir += L"\\MixedMediaStudio";
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        return dir;
    }
    return L".";
}

std::wstring SettingsPath()
{
    return GetAppDataDir() + L"\\settings.ini";
}

std::wstring OfxPluginPath()
{
    wchar_t programFiles[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramW6432", programFiles, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        n = GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
    std::wstring path = programFiles;
    path += L"\\Common Files\\OFX\\Plugins\\MixedMedia.ofx.bundle\\Contents\\Win64\\MixedMedia.ofx";
    return path;
}

bool IsOfxInstalled()
{
    return GetFileAttributesW(OfxPluginPath().c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring LowerExtension(const std::filesystem::path& path)
{
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return (wchar_t)towlower(c); });
    return ext;
}

bool IsImageExtension(const std::filesystem::path& path)
{
    const std::wstring ext = LowerExtension(path);
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" ||
           ext == L".bmp" || ext == L".tif" || ext == L".tiff";
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------

double ReadIniDouble(const wchar_t* section, const wchar_t* key, double fallback)
{
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(section, key, L"", buf, 64, SettingsPath().c_str());
    if (!buf[0])
        return fallback;
    return _wtof(buf);
}

int ReadIniInt(const wchar_t* section, const wchar_t* key, int fallback)
{
    return (int)ReadIniDouble(section, key, fallback);
}

void WriteIniDouble(const wchar_t* section, const wchar_t* key, double value,
                    int decimals = 6)
{
    wchar_t buf[64];
    swprintf(buf, 64, L"%.*f", decimals, value);
    WritePrivateProfileStringW(section, key, buf, SettingsPath().c_str());
}

void WriteIniInt(const wchar_t* section, const wchar_t* key, int value)
{
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", value);
    WritePrivateProfileStringW(section, key, buf, SettingsPath().c_str());
}

void LoadSettings()
{
    Settings s;
    PaperSettings& p = s.paper;
    HalftoneSettings& h = s.ht;

    p.columns = ReadIniDouble(L"paper", L"columns", p.columns);
    p.rows = ReadIniDouble(L"paper", L"rows", p.rows);
    p.missing = ReadIniDouble(L"paper", L"missing", p.missing);
    p.seed = ReadIniDouble(L"paper", L"seed", p.seed);
    p.rotation = ReadIniDouble(L"paper", L"rotation", p.rotation);
    p.sizeVar = ReadIniDouble(L"paper", L"sizeVar", p.sizeVar);
    p.widthVar = ReadIniDouble(L"paper", L"widthVar", p.widthVar);
    p.heightVar = ReadIniDouble(L"paper", L"heightVar", p.heightVar);
    p.uvShift = ReadIniDouble(L"paper", L"uvShift", p.uvShift);
    p.colorVar = ReadIniDouble(L"paper", L"colorVar", p.colorVar);
    p.tear = ReadIniDouble(L"paper", L"tear", p.tear);
    p.chunk = ReadIniDouble(L"paper", L"chunk", p.chunk);
    p.roughness = ReadIniDouble(L"paper", L"roughness", p.roughness);
    p.white = ReadIniDouble(L"paper", L"white", p.white);
    p.unevenness = ReadIniDouble(L"paper", L"unevenness", p.unevenness);
    p.shadow = ReadIniDouble(L"paper", L"shadow", p.shadow);
    p.bgImage = ReadIniInt(L"paper", L"bgImage", p.bgImage ? 1 : 0) != 0;
    p.bgR = ReadIniDouble(L"paper", L"bgR", p.bgR);
    p.bgG = ReadIniDouble(L"paper", L"bgG", p.bgG);
    p.bgB = ReadIniDouble(L"paper", L"bgB", p.bgB);

    h.pattern = ReadIniDouble(L"halftone", L"pattern", h.pattern);
    h.frequency = ReadIniDouble(L"halftone", L"frequency", h.frequency);
    h.roughness = ReadIniDouble(L"halftone", L"roughness", h.roughness);
    h.fuzziness = ReadIniDouble(L"halftone", L"fuzziness", h.fuzziness);
    h.fiber = ReadIniDouble(L"halftone", L"fiber", h.fiber);
    h.inkTexture = ReadIniDouble(L"halftone", L"inkTexture", h.inkTexture);
    h.density = ReadIniDouble(L"halftone", L"density", h.density);
    h.useC = ReadIniInt(L"halftone", L"useC", h.useC ? 1 : 0) != 0;
    h.useM = ReadIniInt(L"halftone", L"useM", h.useM ? 1 : 0) != 0;
    h.useY = ReadIniInt(L"halftone", L"useY", h.useY ? 1 : 0) != 0;
    h.useK = ReadIniInt(L"halftone", L"useK", h.useK ? 1 : 0) != 0;
    const wchar_t* angleKeys[4] = { L"angleC", L"angleM", L"angleY", L"angleK" };
    const wchar_t* oxKeys[4] = { L"offsetXC", L"offsetXM", L"offsetXY", L"offsetXK" };
    const wchar_t* oyKeys[4] = { L"offsetYC", L"offsetYM", L"offsetYY", L"offsetYK" };
    for (int c = 0; c < 4; ++c)
    {
        h.angle[c] = ReadIniDouble(L"halftone", angleKeys[c], h.angle[c]);
        h.offsetX[c] = ReadIniDouble(L"halftone", oxKeys[c], h.offsetX[c]);
        h.offsetY[c] = ReadIniDouble(L"halftone", oyKeys[c], h.offsetY[c]);
    }

    s.chain = ReadIniInt(L"ui", L"chain", s.chain ? 1 : 0) != 0;
    g_app.settings = s;

    wchar_t dir[MAX_PATH] = {};
    GetPrivateProfileStringW(L"ui", L"lastDir", L"", dir, MAX_PATH,
                             SettingsPath().c_str());
    g_app.lastDir = dir;

    g_app.savedWindowRect.left = ReadIniInt(L"window", L"x", 0);
    g_app.savedWindowRect.top = ReadIniInt(L"window", L"y", 0);
    g_app.savedWindowRect.right =
        g_app.savedWindowRect.left + ReadIniInt(L"window", L"w", 0);
    g_app.savedWindowRect.bottom =
        g_app.savedWindowRect.top + ReadIniInt(L"window", L"h", 0);
    g_app.hasSavedWindowRect =
        (g_app.savedWindowRect.right - g_app.savedWindowRect.left) >= Dp(900) &&
        (g_app.savedWindowRect.bottom - g_app.savedWindowRect.top) >= Dp(650);
}

void SaveSettings()
{
    const Settings& s = g_app.settings;
    const PaperSettings& p = s.paper;
    const HalftoneSettings& h = s.ht;

    WriteIniDouble(L"paper", L"columns", p.columns, 0);
    WriteIniDouble(L"paper", L"rows", p.rows, 0);
    WriteIniDouble(L"paper", L"missing", p.missing, 2);
    WriteIniDouble(L"paper", L"seed", p.seed, 0);
    WriteIniDouble(L"paper", L"rotation", p.rotation, 2);
    WriteIniDouble(L"paper", L"sizeVar", p.sizeVar, 4);
    WriteIniDouble(L"paper", L"widthVar", p.widthVar, 4);
    WriteIniDouble(L"paper", L"heightVar", p.heightVar, 4);
    WriteIniDouble(L"paper", L"uvShift", p.uvShift, 2);
    WriteIniDouble(L"paper", L"colorVar", p.colorVar, 4);
    WriteIniDouble(L"paper", L"tear", p.tear, 2);
    WriteIniDouble(L"paper", L"chunk", p.chunk, 2);
    WriteIniDouble(L"paper", L"roughness", p.roughness, 4);
    WriteIniDouble(L"paper", L"white", p.white, 2);
    WriteIniDouble(L"paper", L"unevenness", p.unevenness, 4);
    WriteIniDouble(L"paper", L"shadow", p.shadow, 4);
    WriteIniInt(L"paper", L"bgImage", p.bgImage ? 1 : 0);
    WriteIniDouble(L"paper", L"bgR", p.bgR, 4);
    WriteIniDouble(L"paper", L"bgG", p.bgG, 4);
    WriteIniDouble(L"paper", L"bgB", p.bgB, 4);

    WriteIniDouble(L"halftone", L"pattern", h.pattern, 0);
    WriteIniDouble(L"halftone", L"frequency", h.frequency, 2);
    WriteIniDouble(L"halftone", L"roughness", h.roughness, 4);
    WriteIniDouble(L"halftone", L"fuzziness", h.fuzziness, 4);
    WriteIniDouble(L"halftone", L"fiber", h.fiber, 4);
    WriteIniDouble(L"halftone", L"inkTexture", h.inkTexture, 4);
    WriteIniDouble(L"halftone", L"density", h.density, 4);
    WriteIniInt(L"halftone", L"useC", h.useC ? 1 : 0);
    WriteIniInt(L"halftone", L"useM", h.useM ? 1 : 0);
    WriteIniInt(L"halftone", L"useY", h.useY ? 1 : 0);
    WriteIniInt(L"halftone", L"useK", h.useK ? 1 : 0);
    const wchar_t* angleKeys[4] = { L"angleC", L"angleM", L"angleY", L"angleK" };
    const wchar_t* oxKeys[4] = { L"offsetXC", L"offsetXM", L"offsetXY", L"offsetXK" };
    const wchar_t* oyKeys[4] = { L"offsetYC", L"offsetYM", L"offsetYY", L"offsetYK" };
    for (int c = 0; c < 4; ++c)
    {
        WriteIniDouble(L"halftone", angleKeys[c], h.angle[c], 3);
        WriteIniDouble(L"halftone", oxKeys[c], h.offsetX[c], 3);
        WriteIniDouble(L"halftone", oyKeys[c], h.offsetY[c], 3);
    }

    WriteIniInt(L"ui", L"chain", s.chain ? 1 : 0);
    WritePrivateProfileStringW(L"ui", L"lastDir", g_app.lastDir.c_str(),
                               SettingsPath().c_str());

    if (g_app.hwnd)
    {
        RECT rc;
        GetWindowRect(g_app.hwnd, &rc);
        WriteIniInt(L"window", L"x", rc.left);
        WriteIniInt(L"window", L"y", rc.top);
        WriteIniInt(L"window", L"w", rc.right - rc.left);
        WriteIniInt(L"window", L"h", rc.bottom - rc.top);
    }
}

// ---------------------------------------------------------------------------
// File dialogs
// ---------------------------------------------------------------------------

bool PickImageFile(HWND owner, bool save, std::wstring& path)
{
    ComPtr<IFileDialog> dialog;
    CLSID clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr))
        return false;

    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (!save)
        options |= FOS_FILEMUSTEXIST;
    dialog->SetOptions(options);

    COMDLG_FILTERSPEC filters[] = {
        { L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff" },
        { L"PNG image", L"*.png" },
        { L"All files", L"*.*" }
    };
    COMDLG_FILTERSPEC saveFilters[] = {
        { L"PNG image", L"*.png" }
    };
    if (save)
    {
        dialog->SetFileTypes(1, saveFilters);
        dialog->SetFileTypeIndex(1);
    }
    else
    {
        dialog->SetFileTypes(3, filters);
        dialog->SetFileTypeIndex(1);
    }
    dialog->SetDefaultExtension(L"png");
    dialog->SetTitle(save ? L"Export PNG" : L"Open image");

    if (!g_app.lastDir.empty())
    {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(g_app.lastDir.c_str(), nullptr,
                                                  IID_PPV_ARGS(&folder))))
            dialog->SetFolder(folder.Get());
    }

    if (save)
    {
        std::filesystem::path suggested = L"mixedmedia.png";
        if (!g_app.currentPath.empty())
        {
            std::filesystem::path src(g_app.currentPath);
            suggested = src.stem().wstring() + L"_mixedmedia.png";
        }
        dialog->SetFileName(suggested.c_str());
    }

    if (dialog->Show(owner) != S_OK)
        return false;

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)))
        return false;
    PWSTR filePath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &filePath)))
        return false;
    path = filePath;
    CoTaskMemFree(filePath);

    std::filesystem::path p(path);
    g_app.lastDir = p.parent_path().wstring();
    return true;
}

bool PickFolder(HWND owner, const wchar_t* title, std::wstring& path)
{
    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr))
        return false;
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    dialog->SetOptions(options);
    dialog->SetTitle(title);

    if (!g_app.lastDir.empty())
    {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(g_app.lastDir.c_str(), nullptr,
                                                  IID_PPV_ARGS(&folder))))
            dialog->SetFolder(folder.Get());
    }

    if (dialog->Show(owner) != S_OK)
        return false;

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)))
        return false;
    PWSTR folderPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &folderPath)))
        return false;
    path = folderPath;
    CoTaskMemFree(folderPath);
    return true;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void RenderPaperPass(const ImageRGBA& src, const PaperSettings& s, ImageRGBA& out)
{
    out.width = src.width;
    out.height = src.height;
    out.pixels.assign((size_t)src.width * (size_t)src.height * 4, 0.0f);

    PaperParams p{};
    p.width = src.width;
    p.height = src.height;
    p.originX = 0;
    p.originY = 0;
    p.outPitch = src.width * 4;
    p.srcPitch = src.width * 4;
    p.srcX = 0;
    p.srcY = 0;
    p.srcW = src.width;
    p.srcH = src.height;
    p.columns = ClampI((int)std::lround(s.columns), 1, 40);
    p.rows = ClampI((int)std::lround(s.rows), 1, 40);
    p.seed = ClampI((int)std::lround(s.seed), 0, 99999);
    p.missing = (float)(ClampD(s.missing, 0.0, 100.0) / 100.0);
    p.rotation = (float)(ClampD(s.rotation, 0.0, 45.0) * 3.14159265358979323846 / 180.0);
    p.sizeVar = (float)ClampD(s.sizeVar, 0.0, 1.0);
    p.widthVar = (float)ClampD(s.widthVar, 0.0, 1.0);
    p.heightVar = (float)ClampD(s.heightVar, 0.0, 1.0);
    p.uvShift = (float)ClampD(s.uvShift, 0.0, 150.0);
    p.colorVar = (float)ClampD(s.colorVar, 0.0, 1.0);
    p.tearSize = (float)ClampD(s.tear, 0.0, 120.0);
    p.chunkSize = (float)ClampD(s.chunk, 0.0, 240.0);
    p.roughness = (float)ClampD(s.roughness, 0.0, 1.0);
    p.whiteEdge = (float)ClampD(s.white, 0.0, 60.0);
    p.unevenness = (float)ClampD(s.unevenness, 0.0, 1.0);
    p.shadow = (float)ClampD(s.shadow, 0.0, 1.0);
    p.bgImage = s.bgImage ? 1 : 0;
    p.bgR = (float)ClampD(s.bgR, 0.0, 1.0);
    p.bgG = (float)ClampD(s.bgG, 0.0, 1.0);
    p.bgB = (float)ClampD(s.bgB, 0.0, 1.0);
    p.premult = 0;

    const float pxScale =
        std::max(0.05f, (float)std::min(src.width, src.height) / 1080.0f);
    p.pxScale = pxScale;
    p.uvShift *= pxScale;
    p.tearSize *= pxScale;
    p.chunkSize *= pxScale;
    p.whiteEdge *= pxScale;

    mm::renderPaperCpu(p, src.pixels.data(), out.pixels.data(), 0);
}

void RenderHalftonePass(const ImageRGBA& src, const HalftoneSettings& s,
                        ImageRGBA& out)
{
    out.width = src.width;
    out.height = src.height;
    out.pixels.assign((size_t)src.width * (size_t)src.height * 4, 0.0f);

    HalftoneParams p{};
    p.width = src.width;
    p.height = src.height;
    p.originX = 0;
    p.originY = 0;
    p.outPitch = src.width * 4;
    p.srcPitch = src.width * 4;
    p.srcX = 0;
    p.srcY = 0;
    p.srcW = src.width;
    p.srcH = src.height;
    p.pattern = ClampI((int)std::lround(s.pattern), 0, 5);
    p.frequency = (float)ClampD(s.frequency, 4.0, 400.0);
    p.roughness = (float)ClampD(s.roughness, 0.0, 1.0);
    p.fuzziness = (float)ClampD(s.fuzziness, 0.0, 1.0);
    p.paperFiber = (float)ClampD(s.fiber, 0.0, 1.0);
    p.inkTexture = (float)ClampD(s.inkTexture, 0.0, 1.0);
    p.inkDensity = (float)ClampD(s.density, 0.0, 2.0);
    p.useC = s.useC ? 1 : 0;
    p.useM = s.useM ? 1 : 0;
    p.useY = s.useY ? 1 : 0;
    p.useK = s.useK ? 1 : 0;
    p.premult = 0;

    const float pxScale =
        std::max(0.05f, (float)std::min(src.width, src.height) / 1080.0f);
    p.pxScale = pxScale;
    for (int c = 0; c < 4; ++c)
    {
        p.angle[c] = (float)(ClampD(s.angle[c], -180.0, 180.0) *
                             3.14159265358979323846 / 180.0);
        p.offsetX[c] = (float)(ClampD(s.offsetX[c], -200.0, 200.0) * pxScale);
        p.offsetY[c] = (float)(ClampD(s.offsetY[c], -200.0, 200.0) * pxScale);
    }

    mm::renderHalftoneCpu(p, src.pixels.data(), out.pixels.data(), 0);
}

bool RenderChain(const ImageRGBA& src, const Settings& s, bool chain,
                 bool halftoneOnly, ImageRGBA& out)
{
    out.clear();
    if (src.empty())
        return false;

    const bool useHalftone = chain || halftoneOnly;
    const bool usePaper = chain || !halftoneOnly;

    ImageRGBA intermediate;
    const ImageRGBA* input = &src;
    if (useHalftone)
    {
        RenderHalftonePass(src, s.ht, intermediate);
        input = &intermediate;
    }
    if (usePaper)
    {
        RenderPaperPass(*input, s.paper, out);
    }
    else
    {
        out = std::move(intermediate);
    }
    return !out.empty();
}

void ConvertToBgra(const ImageRGBA& image, std::vector<uint8_t>& bytes)
{
    bytes.assign((size_t)image.width * (size_t)image.height * 4, 0);
    const size_t stride = (size_t)image.width * 4;
    for (int y = 0; y < image.height; ++y)
    {
        const float* src =
            image.pixels.data() +
            (size_t)(image.height - 1 - y) * (size_t)image.width * 4;
        uint8_t* dst = bytes.data() + (size_t)y * stride;
        for (int x = 0; x < image.width; ++x)
        {
            dst[(size_t)x * 4 + 0] =
                (uint8_t)(ClampD(src[(size_t)x * 4 + 2], 0, 1) * 255.0 + 0.5);
            dst[(size_t)x * 4 + 1] =
                (uint8_t)(ClampD(src[(size_t)x * 4 + 1], 0, 1) * 255.0 + 0.5);
            dst[(size_t)x * 4 + 2] =
                (uint8_t)(ClampD(src[(size_t)x * 4 + 0], 0, 1) * 255.0 + 0.5);
            dst[(size_t)x * 4 + 3] =
                (uint8_t)(ClampD(src[(size_t)x * 4 + 3], 0, 1) * 255.0 + 0.5);
        }
    }
}

void BuildBitmap(const ImageRGBA& image, std::vector<uint8_t>& bytes,
                 std::unique_ptr<Gdiplus::Bitmap>& bitmap)
{
    bitmap.reset();
    bytes.clear();
    if (image.empty())
        return;
    ConvertToBgra(image, bytes);
    bitmap = std::make_unique<Gdiplus::Bitmap>(
        image.width, image.height, image.width * 4,
        PixelFormat32bppARGB, bytes.data());
    if (bitmap && bitmap->GetLastStatus() != Gdiplus::Ok)
        bitmap.reset();
}

void RefreshStatus()
{
    if (g_app.hwnd)
        InvalidateRect(g_app.hwnd, &g_app.statusRect, FALSE);
}

void InvalidatePreview()
{
    if (g_app.hwnd)
        InvalidateRect(g_app.hwnd, &g_app.previewRect, FALSE);
}

void SchedulePreview()
{
    if (!g_app.hwnd || g_app.batchRunning)
        return;
    KillTimer(g_app.hwnd, kPreviewTimer);
    SetTimer(g_app.hwnd, kPreviewTimer, kPreviewDelayMs, nullptr);
}

void RenderPreviewNow()
{
    if (g_app.previewSource.empty() ||
        g_app.previewRect.right <= g_app.previewRect.left ||
        g_app.previewRect.bottom <= g_app.previewRect.top)
        return;

    const int areaW = std::max(
        1, (int)(g_app.previewRect.right - g_app.previewRect.left) - Dp(36));
    const int areaH = std::max(
        1, (int)(g_app.previewRect.bottom - g_app.previewRect.top) - Dp(36));
    double scale = std::min((double)areaW / g_app.previewSource.width,
                            (double)areaH / g_app.previewSource.height);
    scale = std::min(scale, 1.0);
    const int renderW = std::max(
        1, (int)std::lround(g_app.previewSource.width * scale));
    const int renderH = std::max(
        1, (int)std::lround(g_app.previewSource.height * scale));

    ImageRGBA renderSource;
    if (renderW != g_app.previewSource.width ||
        renderH != g_app.previewSource.height)
    {
        if (!ScaleImage(g_app.previewSource, renderW, renderH, renderSource,
                        nullptr))
            return;
    }
    else
    {
        renderSource = g_app.previewSource;
    }

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    ImageRGBA out;
    RenderChain(renderSource, g_app.settings, g_app.settings.chain,
                g_app.activeTab == 1, out);
    if (out.empty())
        return;

    g_app.previewOutput = std::move(out);
    BuildBitmap(g_app.previewOutput, g_app.previewBgra, g_app.previewBitmap);

    QueryPerformanceCounter(&end);
    g_app.previewMs =
        (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;

    InvalidatePreview();
    RefreshStatus();
}

void SyncSliders()
{
    for (Slider& s : g_app.sliders)
        InvalidateRect(s.track, nullptr, FALSE);
}

void UpdateControls()
{
    InvalidateRect(g_app.chainCheck, nullptr, FALSE);
    InvalidateRect(g_app.bgImageCheck, nullptr, FALSE);
    InvalidateRect(g_app.channelC, nullptr, FALSE);
    InvalidateRect(g_app.channelM, nullptr, FALSE);
    InvalidateRect(g_app.channelY, nullptr, FALSE);
    InvalidateRect(g_app.channelK, nullptr, FALSE);
    InvalidateRect(g_app.originalCheck, nullptr, FALSE);
    InvalidateRect(g_app.tabPaper, nullptr, FALSE);
    InvalidateRect(g_app.tabHalftone, nullptr, FALSE);
    for (HWND chip : g_app.patternChips)
        if (chip)
            InvalidateRect(chip, nullptr, FALSE);
    if (g_app.paperPage)
        InvalidateRect(g_app.paperPage, &g_app.paperSwatchRect, FALSE);
    if (g_app.halftonePage)
        InvalidateRect(g_app.halftonePage, &g_app.screenPreviewRect, FALSE);
}

void SetSourceImage(ImageRGBA full, ImageRGBA preview, const std::wstring& path)
{
    g_app.fullImage = std::move(full);
    g_app.previewSource = std::move(preview);
    g_app.currentPath = path;
    g_app.previewOutput.clear();
    BuildBitmap(g_app.previewSource, g_app.sourceBgra, g_app.sourceBitmap);
    BuildBitmap(g_app.previewOutput, g_app.previewBgra, g_app.previewBitmap);

    if (!path.empty())
    {
        std::filesystem::path p(path);
        g_app.lastDir = p.parent_path().wstring();
        std::wstring title = std::wstring(kAppName) + L" - " + p.filename().wstring();
        SetWindowTextW(g_app.hwnd, title.c_str());
    }
    else
    {
        SetWindowTextW(g_app.hwnd, kAppName);
    }

    g_app.showOriginal = false;
    UpdateControls();
    RenderPreviewNow();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

bool LoadImageIntoApp(const std::wstring& path, bool showErrors)
{
    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    ImageRGBA full, preview;
    std::wstring error;
    const bool ok = LoadImageFile(path.c_str(), full, kPreviewMaxW,
                                  kPreviewMaxH, &preview, &error);
    SetCursor(oldCursor);
    if (!ok)
    {
        if (showErrors)
        {
            std::wstring message = L"Could not open this image:\n\n";
            message += path;
            message += L"\n\n";
            message += error;
            ShowError(g_app.hwnd, message);
        }
        return false;
    }
    if ((long long)full.width * full.height > 64000000LL)
    {
        if (showErrors)
            ShowError(g_app.hwnd,
                      L"This image is larger than 64 megapixels. Please "
                      L"downscale it before opening it in Mixed Media Studio.");
        return false;
    }
    SetSourceImage(std::move(full), std::move(preview), path);
    return true;
}

// ---------------------------------------------------------------------------
// Sample image
// ---------------------------------------------------------------------------

bool MakeSampleImage(ImageRGBA& out)
{
    const int w = 1600;
    const int h = 1000;
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
    if (bmp.GetLastStatus() != Gdiplus::Ok)
        return false;
    {
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

        Gdiplus::LinearGradientBrush bg(
            Gdiplus::RectF(0, 0, (Gdiplus::REAL)w, (Gdiplus::REAL)h),
            Gdiplus::Color(255, 27, 96, 106),
            Gdiplus::Color(255, 244, 181, 121),
            Gdiplus::LinearGradientModeVertical);
        g.FillRectangle(&bg, 0, 0, w, h);

        for (int i = 0; i < 8; ++i)
        {
            const int a = 16 + i * 6;
            Gdiplus::SolidBrush glow(Gdiplus::Color((BYTE)a, 255, 216, 120));
            const int grow = 150 + i * 26;
            g.FillEllipse(&glow, 1030 - grow / 2, 130 - grow / 2, grow, grow);
        }
        Gdiplus::SolidBrush sun(Gdiplus::Color(245, 255, 231, 148));
        g.FillEllipse(&sun, 950, 65, 330, 330);

        {
            Gdiplus::PointF hills[] = {
                Gdiplus::PointF(0, 660), Gdiplus::PointF(210, 500),
                Gdiplus::PointF(380, 610), Gdiplus::PointF(600, 470),
                Gdiplus::PointF(830, 640), Gdiplus::PointF(1080, 500),
                Gdiplus::PointF(1330, 640), Gdiplus::PointF(1600, 530),
                Gdiplus::PointF(1600, 1000), Gdiplus::PointF(0, 1000)
            };
            Gdiplus::SolidBrush hill(Gdiplus::Color(255, 48, 105, 109));
            g.FillPolygon(&hill, hills, 10);
        }

        {
            Gdiplus::LinearGradientBrush water(
                Gdiplus::RectF(0, 620, (Gdiplus::REAL)w, 380),
                Gdiplus::Color(255, 39, 143, 139),
                Gdiplus::Color(255, 26, 82, 88),
                Gdiplus::LinearGradientModeVertical);
            g.FillRectangle(&water, 0, 700, w, 300);
            Gdiplus::Pen pen(Gdiplus::Color(120, 255, 214, 140), 3.0f);
            for (int i = 0; i < 12; ++i)
            {
                const float y = 735.0f + i * 19.0f;
                g.DrawLine(&pen, 90.0f, y, 1510.0f, y + (i % 2 ? 7.0f : -5.0f));
            }
        }

        {
            Gdiplus::PointF paper[] = {
                Gdiplus::PointF(70, 240), Gdiplus::PointF(660, 210),
                Gdiplus::PointF(690, 275), Gdiplus::PointF(620, 300),
                Gdiplus::PointF(640, 360), Gdiplus::PointF(120, 390),
                Gdiplus::PointF(95, 340), Gdiplus::PointF(130, 300)
            };
            Gdiplus::SolidBrush paperBrush(Gdiplus::Color(235, 247, 239, 220));
            g.FillPolygon(&paperBrush, paper, 8);
            Gdiplus::FontFamily family(L"Segoe UI");
            Gdiplus::Font titleFont(&family, 74.0f, Gdiplus::FontStyleBold,
                                    Gdiplus::UnitPixel);
            Gdiplus::SolidBrush textBrush(Gdiplus::Color(235, 31, 27, 22));
            g.DrawString(L"MIXED", -1, &titleFont, Gdiplus::PointF(118, 255),
                         &textBrush);
        }

        {
            Gdiplus::SolidBrush teal(Gdiplus::Color(210, 45, 156, 148));
            Gdiplus::SolidBrush orange(Gdiplus::Color(220, 228, 87, 46));
            Gdiplus::SolidBrush gold(Gdiplus::Color(210, 232, 176, 72));
            g.TranslateTransform(1230.0f, 690.0f);
            g.RotateTransform(-8.0f);
            g.FillRectangle(&teal, -170, -70, 340, 140);
            g.ResetTransform();
            g.TranslateTransform(1180.0f, 820.0f);
            g.RotateTransform(6.0f);
            g.FillRectangle(&orange, -160, -55, 320, 110);
            g.ResetTransform();
            g.TranslateTransform(480.0f, 820.0f);
            g.RotateTransform(-5.0f);
            g.FillRectangle(&gold, -200, -45, 400, 90);
            g.ResetTransform();
        }

        {
            Gdiplus::SolidBrush dots(Gdiplus::Color(80, 255, 244, 220));
            for (int row = 0; row < 9; ++row)
            {
                for (int col = 0; col < 22; ++col)
                {
                    const float x = 55.0f + col * 31.0f;
                    const float y = 45.0f + row * 24.0f;
                    const float r = 2.2f + (float)((row + col) % 4) * 0.8f;
                    g.FillEllipse(&dots, x, y, r * 2, r * 2);
                }
            }
        }

        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font subtitle(&family, 22.0f, Gdiplus::FontStyleRegular,
                               Gdiplus::UnitPixel);
        Gdiplus::SolidBrush subBrush(Gdiplus::Color(200, 255, 248, 235));
        g.DrawString(L"PAPER CUTOUT  /  HALFTONE SCREEN", -1, &subtitle,
                     Gdiplus::PointF(680, 315), &subBrush);
    }

    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, w, h);
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead,
                     PixelFormat32bppARGB, &data) != Gdiplus::Ok)
        return false;

    out.width = w;
    out.height = h;
    out.pixels.assign((size_t)w * (size_t)h * 4, 0.0f);
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* row = (const uint8_t*)data.Scan0 + (size_t)y * data.Stride;
        float* dst = out.pixels.data() + (size_t)(h - 1 - y) * (size_t)w * 4;
        for (int x = 0; x < w; ++x)
        {
            const uint8_t b = row[(size_t)x * 4 + 0];
            const uint8_t g = row[(size_t)x * 4 + 1];
            const uint8_t r = row[(size_t)x * 4 + 2];
            const uint8_t a = row[(size_t)x * 4 + 3];
            const float grain =
                ((float)((mm_hash_cell(x, y, 41u, 7u) * 255.0f) - 127.5f) /
                 127.5f) *
                0.022f;
            dst[(size_t)x * 4 + 0] =
                (float)ClampD((float)r / 255.0f + grain, 0.0, 1.0);
            dst[(size_t)x * 4 + 1] =
                (float)ClampD((float)g / 255.0f + grain, 0.0, 1.0);
            dst[(size_t)x * 4 + 2] =
                (float)ClampD((float)b / 255.0f + grain, 0.0, 1.0);
            dst[(size_t)x * 4 + 3] = (float)a / 255.0f;
        }
    }
    bmp.UnlockBits(&data);
    return true;
}

// ---------------------------------------------------------------------------
// Fonts and brushes
// ---------------------------------------------------------------------------

HFONT MakeFont(double points, int weight)
{
    return CreateFontW(-MulDiv((int)std::lround(points * 10.0), g_app.dpi, 720),
                       0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void RecreateFonts()
{
    HFONT* fonts[] = { &g_app.fontTitle,   &g_app.fontSubtitle,
                       &g_app.fontBody,    &g_app.fontLabel,
                       &g_app.fontValue,   &g_app.fontSmall,
                       &g_app.fontSection, &g_app.fontButton,
                       &g_app.fontTab,     &g_app.fontBadge };
    for (HFONT* font : fonts)
    {
        if (*font)
            DeleteObject(*font);
        *font = nullptr;
    }
    g_app.fontTitle = MakeFont(21.0, FW_SEMIBOLD);
    g_app.fontSubtitle = MakeFont(9.0, FW_NORMAL);
    g_app.fontBody = MakeFont(9.5, FW_NORMAL);
    g_app.fontLabel = MakeFont(8.5, FW_NORMAL);
    g_app.fontValue = MakeFont(8.5, FW_SEMIBOLD);
    g_app.fontSmall = MakeFont(8.0, FW_NORMAL);
    g_app.fontSection = MakeFont(7.5, FW_SEMIBOLD);
    g_app.fontButton = MakeFont(9.5, FW_SEMIBOLD);
    g_app.fontTab = MakeFont(9.5, FW_SEMIBOLD);
    g_app.fontBadge = MakeFont(7.5, FW_SEMIBOLD);
}

void CreateBrushes()
{
    g_app.brushCard = CreateSolidBrush(kCard);
    g_app.brushCardAlt = CreateSolidBrush(kCardAlt);
    g_app.brushLine = CreateSolidBrush(kLine);
    g_app.brushDark = CreateSolidBrush(kDark);
}

void DestroyBrushes()
{
    HBRUSH* brushes[] = { &g_app.brushCard, &g_app.brushCardAlt,
                          &g_app.brushLine, &g_app.brushDark };
    for (HBRUSH* brush : brushes)
    {
        if (*brush)
            DeleteObject(*brush);
        *brush = nullptr;
    }
}

void ApplyFonts()
{
    for (const ButtonInfo& b : g_app.headerButtons)
        SendMessageW(b.hwnd, WM_SETFONT, (WPARAM)g_app.fontButton, TRUE);
    SendMessageW(g_app.tabPaper, WM_SETFONT, (WPARAM)g_app.fontTab, TRUE);
    SendMessageW(g_app.tabHalftone, WM_SETFONT, (WPARAM)g_app.fontTab, TRUE);
    SendMessageW(g_app.chainCheck, WM_SETFONT, (WPARAM)g_app.fontLabel, TRUE);
    SendMessageW(g_app.originalCheck, WM_SETFONT, (WPARAM)g_app.fontSmall, TRUE);
    SendMessageW(g_app.resetButton, WM_SETFONT, (WPARAM)g_app.fontButton, TRUE);
    HWND switches[] = { g_app.bgImageCheck };
    for (HWND sw : switches)
        if (sw)
            SendMessageW(sw, WM_SETFONT, (WPARAM)g_app.fontLabel, TRUE);
    HWND chips[] = { g_app.channelC, g_app.channelM, g_app.channelY,
                     g_app.channelK, g_app.patternChips[0], g_app.patternChips[1],
                     g_app.patternChips[2], g_app.patternChips[3],
                     g_app.patternChips[4], g_app.patternChips[5] };
    for (HWND chip : chips)
        if (chip)
            SendMessageW(chip, WM_SETFONT, (WPARAM)g_app.fontBadge, TRUE);
    HWND sections[] = { g_app.sectionPaper0, g_app.sectionPaper1,
                        g_app.sectionPaper2, g_app.sectionHt0,
                        g_app.sectionHt1, g_app.sectionHt2 };
    for (HWND section : sections)
        if (section)
            SendMessageW(section, WM_SETFONT, (WPARAM)g_app.fontSection, TRUE);
}

// ---------------------------------------------------------------------------
// GDI+ drawing helpers
// ---------------------------------------------------------------------------

void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r,
                  float radius)
{
    float d = radius * 2.0f;
    d = std::min(d, std::min(r.Width, r.Height));
    if (d <= 0.0f)
    {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRounded(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                 const Gdiplus::Color& color)
{
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, r, radius);
    Gdiplus::SolidBrush brush(color);
    g.FillPath(&brush, &path);
}

void StrokeRounded(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                   const Gdiplus::Color& color, float width)
{
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, r, radius);
    Gdiplus::Pen pen(color, width);
    g.DrawPath(&pen, &path);
}

void FillSolid(HDC dc, const RECT& rc, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
}

void DrawCard(HDC dc, const RECT& rc, int radius, COLORREF fill,
              COLORREF border, bool shadow, bool dark = false)
{
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    if (shadow)
    {
        const int spread = Dp(10);
        for (int i = spread; i >= 2; i -= 2)
        {
            const BYTE alpha = (BYTE)(dark ? 16 : 9);
            Gdiplus::RectF r((Gdiplus::REAL)rc.left - i / 2,
                             (Gdiplus::REAL)rc.top + (Gdiplus::REAL)i / 3,
                             (Gdiplus::REAL)(rc.right - rc.left + i),
                             (Gdiplus::REAL)(rc.bottom - rc.top + i));
            FillRounded(g, r, (Gdiplus::REAL)radius + i / 2,
                        Gdiplus::Color(alpha, 44, 42, 72));
        }
    }
    Gdiplus::RectF r((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                     (Gdiplus::REAL)(rc.right - rc.left),
                     (Gdiplus::REAL)(rc.bottom - rc.top));
    FillRounded(g, r, (Gdiplus::REAL)radius, Gd(fill));
    StrokeRounded(g, r, (Gdiplus::REAL)radius, Gd(border), 1.0f);
}

void DrawPill(HDC dc, const RECT& rc, COLORREF fill, COLORREF textColor,
              const wchar_t* text)
{
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF r((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                     (Gdiplus::REAL)(rc.right - rc.left),
                     (Gdiplus::REAL)(rc.bottom - rc.top));
    FillRounded(g, r, (Gdiplus::REAL)(rc.bottom - rc.top) / 2.0f, Gd(fill));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_app.fontBadge);
    SetTextColor(dc, textColor);
    RECT tr = rc;
    DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ---------------------------------------------------------------------------
// Slider control
// ---------------------------------------------------------------------------

void SliderSendChange(Slider* s)
{
    SendMessageW(GetParent(s->track), WM_HSCROLL,
                 MAKEWPARAM(0, TB_THUMBTRACK), (LPARAM)s->track);
}

void SliderSetFromX(Slider* s, int x)
{
    RECT rc;
    GetClientRect(s->track, &rc);
    const double left = (double)Dp(4);
    const double width = std::max(1.0, (double)rc.right - left - Dp(4));
    double t = ((double)x - left) / width;
    t = ClampD(t, 0.0, 1.0);
    double v = s->minValue + t * (s->maxValue - s->minValue);
    v = s->minValue + std::round((v - s->minValue) / s->step) * s->step;
    *s->value = ClampD(v, s->minValue, s->maxValue);
    InvalidateRect(s->track, nullptr, FALSE);
    SliderSendChange(s);
}

void PaintSlider(HWND hwnd, HDC dc)
{
    Slider* s = (Slider*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s)
        return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    FillSolid(dc, rc, kCard);

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_app.fontLabel);
    SetTextColor(dc, kMuted);
    RECT labelRect = { 0, 0, w - Dp(64), Dp(17) };
    DrawTextW(dc, s->labelText.c_str(), -1, &labelRect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    wchar_t value[64];
    swprintf(value, 64, L"%.*f%s", s->decimals, *s->value, s->suffix.c_str());
    SelectObject(dc, g_app.fontValue);
    SetTextColor(dc, kInk);
    RECT valueRect = { w - Dp(70), 0, w, Dp(17) };
    DrawTextW(dc, value, -1, &valueRect,
              DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float trackX = (float)Dp(4);
    const float trackW = (float)std::max(1, w - Dp(8));
    const float trackH = (float)Dp(6);
    const float trackY = (float)(h - Dp(12));
    Gdiplus::RectF track(trackX, trackY, trackW, trackH);
    FillRounded(g, track, trackH / 2.0f, Gdiplus::Color(255, 226, 230, 241));

    const double t =
        ClampD((*s->value - s->minValue) / (s->maxValue - s->minValue), 0, 1);
    const float thumbX = trackX + trackW * (float)t;
    if (t > 0.001)
    {
        Gdiplus::RectF fill(trackX, trackY, trackW * (float)t, trackH);
        FillRounded(g, fill, trackH / 2.0f, Gd(kAccent));
    }

    const float r = (float)Dp(s->dragging || s->hovering ? 8 : 7);
    Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(45, 40, 30, 60));
    g.FillEllipse(&shadowBrush, thumbX - r + 1, trackY + 3.5f - r + 2,
                  r * 2, r * 2);
    Gdiplus::SolidBrush thumbBrush(Gdiplus::Color(255, 255, 255, 255));
    g.FillEllipse(&thumbBrush, thumbX - r, trackY + 3.5f - r, r * 2, r * 2);
    Gdiplus::Pen thumbPen(Gd(kAccent), (float)Dp(2));
    g.DrawEllipse(&thumbPen, thumbX - r, trackY + 3.5f - r, r * 2, r * 2);

    if (GetFocus() == hwnd)
    {
        Gdiplus::RectF focus((Gdiplus::REAL)Dp(1), (Gdiplus::REAL)Dp(1),
                             (Gdiplus::REAL)(w - Dp(2)),
                             (Gdiplus::REAL)(h - Dp(2)));
        StrokeRounded(g, focus, (Gdiplus::REAL)Dp(7),
                      Gdiplus::Color(80, 124, 92, 255), 1.0f);
    }
}

LRESULT CALLBACK SliderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Slider* s = (Slider*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintSlider(hwnd, mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (s)
        {
            SetFocus(hwnd);
            SetCapture(hwnd);
            s->dragging = true;
            SliderSetFromX(s, GET_X_LPARAM(lParam));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (s)
        {
            if (s->dragging)
                SliderSetFromX(s, GET_X_LPARAM(lParam));
            else if (!s->hovering)
            {
                s->hovering = true;
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (s)
        {
            s->hovering = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (s)
        {
            s->dragging = false;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        if (s)
        {
            double delta = 0.0;
            if (wParam == VK_LEFT)
                delta = -s->step;
            else if (wParam == VK_RIGHT)
                delta = s->step;
            else if (wParam == VK_HOME)
                *s->value = s->minValue;
            else if (wParam == VK_END)
                *s->value = s->maxValue;
            else if (wParam == VK_PRIOR)
                delta = s->step * 10.0;
            else if (wParam == VK_NEXT)
                delta = -s->step * 10.0;
            if (delta != 0.0)
                *s->value = ClampD(*s->value + delta, s->minValue, s->maxValue);
            if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_HOME ||
                wParam == VK_END || wParam == VK_PRIOR || wParam == VK_NEXT)
            {
                InvalidateRect(hwnd, nullptr, FALSE);
                SliderSendChange(s);
                return 0;
            }
        }
        break;
    case WM_MOUSEWHEEL:
        if (s)
        {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            *s->value = ClampD(*s->value + s->step * 5.0 * (delta / WHEEL_DELTA),
                               s->minValue, s->maxValue);
            InvalidateRect(hwnd, nullptr, FALSE);
            SliderSendChange(s);
        }
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// UI page procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, g_app.brushCard);
        return 1;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, g_app.brushCard);

        if (hwnd == g_app.paperPage && g_app.paperSwatchRect.right >
                                            g_app.paperSwatchRect.left)
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            RECT sw = { g_app.paperSwatchRect.left,
                        g_app.paperSwatchRect.top + Dp(2),
                        g_app.paperSwatchRect.right,
                        g_app.paperSwatchRect.bottom };
            const PaperSettings& p = g_app.settings.paper;
            const COLORREF color =
                RGB((int)(ClampD(p.bgR, 0, 1) * 255.0 + 0.5),
                    (int)(ClampD(p.bgG, 0, 1) * 255.0 + 0.5),
                    (int)(ClampD(p.bgB, 0, 1) * 255.0 + 0.5));
            Gdiplus::RectF r((Gdiplus::REAL)sw.left, (Gdiplus::REAL)sw.top,
                             (Gdiplus::REAL)(sw.right - sw.left),
                             (Gdiplus::REAL)(sw.bottom - sw.top));
            FillRounded(g, r, (Gdiplus::REAL)Dp(12), Gd(color));
            StrokeRounded(g, r, (Gdiplus::REAL)Dp(12), Gd(kLine), 1.0f);

            const double luma = 0.2126 * GetRValue(color) +
                                0.7152 * GetGValue(color) +
                                0.0722 * GetBValue(color);
            wchar_t text[64];
            swprintf(text, 64, L"R %d   G %d   B %d", GetRValue(color),
                     GetGValue(color), GetBValue(color));
            SetTextColor(dc, luma > 150 ? kInk : RGB(255, 255, 255));
            SelectObject(dc, g_app.fontSmall);
            RECT tr = sw;
            InflateRect(&tr, -Dp(10), -Dp(6));
            DrawTextW(dc, text, -1, &tr,
                      DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
        }

        if (hwnd == g_app.halftonePage &&
            g_app.screenPreviewRect.right > g_app.screenPreviewRect.left &&
            g_app.screenPreviewRect.bottom - g_app.screenPreviewRect.top >
                Dp(70))
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::RectF card(
                (Gdiplus::REAL)g_app.screenPreviewRect.left,
                (Gdiplus::REAL)g_app.screenPreviewRect.top,
                (Gdiplus::REAL)(g_app.screenPreviewRect.right -
                                g_app.screenPreviewRect.left),
                (Gdiplus::REAL)(g_app.screenPreviewRect.bottom -
                                g_app.screenPreviewRect.top));
            FillRounded(g, card, (Gdiplus::REAL)Dp(14), Gd(kDark));
            StrokeRounded(g, card, (Gdiplus::REAL)Dp(14),
                          Gdiplus::Color(90, 84, 76, 120), 1.0f);

            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, g_app.fontSection);
            SetTextColor(dc, RGB(162, 156, 190));
            RECT label = { g_app.screenPreviewRect.left + Dp(10),
                           g_app.screenPreviewRect.top + Dp(6),
                           g_app.screenPreviewRect.right - Dp(8),
                           g_app.screenPreviewRect.top + Dp(22) };
            DrawTextW(dc, L"SCREEN PREVIEW", -1, &label,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER);

            const int cols = 6;
            const int rows = 4;
            const float pad = (float)Dp(12);
            const float topPad = (float)Dp(24);
            const float innerW =
                (float)(g_app.screenPreviewRect.right -
                        g_app.screenPreviewRect.left) - pad * 2.0f;
            const float innerH =
                (float)(g_app.screenPreviewRect.bottom -
                        g_app.screenPreviewRect.top) - topPad - pad;
            const float cellW = innerW / (float)cols;
            const float cellH = innerH / (float)rows;
            const float freqScale = (float)ClampD(
                95.0 / ClampD(g_app.settings.ht.frequency, 4.0, 400.0), 0.55,
                2.2);
            const float r = std::max(
                1.0f, std::min(cellW, cellH) * 0.18f * freqScale);
            const int pattern = ClampI(
                (int)std::lround(g_app.settings.ht.pattern), 0, 5);
            const Gdiplus::Color colors[4] = {
                Gdiplus::Color(220, 124, 92, 255),
                Gdiplus::Color(210, 80, 200, 220),
                Gdiplus::Color(210, 235, 100, 170),
                Gdiplus::Color(210, 240, 186, 78)
            };
            for (int row = 0; row < rows; ++row)
            {
                for (int col = 0; col < cols; ++col)
                {
                    const float cx = (float)g_app.screenPreviewRect.left + pad +
                                     (col + 0.5f) * cellW;
                    const float cy = (float)g_app.screenPreviewRect.top +
                                     topPad + (row + 0.5f) * cellH;
                    Gdiplus::SolidBrush brush(colors[(row + col) % 4]);
                    if (pattern == 1)
                    {
                        g.FillEllipse(&brush, cx - r * 1.5f, cy - r * 0.8f,
                                      r * 3.0f, r * 1.6f);
                    }
                    else if (pattern == 2)
                    {
                        g.FillRectangle(&brush, cx - r, cy - r, r * 2, r * 2);
                    }
                    else if (pattern == 3)
                    {
                        Gdiplus::PointF pts[4] = {
                            Gdiplus::PointF(cx, cy - r * 1.4f),
                            Gdiplus::PointF(cx + r * 1.4f, cy),
                            Gdiplus::PointF(cx, cy + r * 1.4f),
                            Gdiplus::PointF(cx - r * 1.4f, cy)
                        };
                        g.FillPolygon(&brush, pts, 4);
                    }
                    else if (pattern == 4)
                    {
                        g.FillRectangle(&brush, cx - r * 1.8f, cy - r * 0.35f,
                                        r * 3.6f, r * 0.7f);
                    }
                    else if (pattern == 5)
                    {
                        g.FillRectangle(&brush, cx - r * 1.6f, cy - r * 0.35f,
                                        r * 3.2f, r * 0.7f);
                        g.FillRectangle(&brush, cx - r * 0.35f, cy - r * 1.6f,
                                        r * 0.7f, r * 3.2f);
                    }
                    else
                    {
                        g.FillEllipse(&brush, cx - r, cy - r, r * 2, r * 2);
                    }
                }
            }

            const wchar_t* names[] = { L"Round", L"Ellipse", L"Square",
                                       L"Diamond", L"Line", L"Cross" };
            wchar_t caption[64];
            swprintf(caption, 64, L"%d LPI  \u00B7  %s",
                     (int)std::lround(g_app.settings.ht.frequency),
                     names[pattern]);
            SelectObject(dc, g_app.fontBadge);
            SetTextColor(dc, RGB(160, 154, 188));
            RECT cap = { g_app.screenPreviewRect.left + Dp(10),
                         g_app.screenPreviewRect.bottom - Dp(20),
                         g_app.screenPreviewRect.right - Dp(10),
                         g_app.screenPreviewRect.bottom - Dp(4) };
            DrawTextW(dc, caption, -1, &cap,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }

        if (g_app.pageDividerX > 0)
        {
            RECT div = { g_app.pageDividerX, Dp(8), g_app.pageDividerX + 1,
                         rc.bottom - Dp(8) };
            FillRect(dc, &div, g_app.brushLine);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    {
        HDC dc = (HDC)wParam;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetPropW((HWND)lParam, L"MMSection") ? kMuted : kInk);
        return (LRESULT)g_app.brushCard;
    }
    case WM_COMMAND:
    case WM_HSCROLL:
    case WM_DRAWITEM:
    case WM_NOTIFY:
        return SendMessageW(GetParent(hwnd), msg, wParam, lParam);
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// UI creation
// ---------------------------------------------------------------------------

HWND CreateStatic(HWND parent, const wchar_t* text, HFONT font, int id,
                  DWORD extraStyle = 0)
{
    HWND h = CreateWindowExW(0, L"STATIC", text,
                             WS_CHILD | WS_VISIBLE | extraStyle, 0, 0, 10, 10,
                             parent, (HMENU)(INT_PTR)id, g_app.instance, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

HWND CreateOwnerButton(HWND parent, const wchar_t* text, int id)
{
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                             parent, (HMENU)(INT_PTR)id, g_app.instance, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_app.fontButton, TRUE);
    return h;
}

HWND CreateChip(HWND parent, const wchar_t* text, int id)
{
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                             parent, (HMENU)(INT_PTR)id, g_app.instance, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_app.fontBadge, TRUE);
    return h;
}

HWND CreateSwitch(HWND parent, const wchar_t* text, int id)
{
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                             parent, (HMENU)(INT_PTR)id, g_app.instance, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_app.fontLabel, TRUE);
    return h;
}

void AddSlider(HWND parent, PageKind page, int col, int row,
               const wchar_t* label, double* value, double minValue,
               double maxValue, double step, int decimals,
               const wchar_t* suffix, bool swatch = false)
{
    Slider& s = g_app.sliders.emplace_back();
    s.page = page;
    s.col = col;
    s.row = row;
    s.value = value;
    s.minValue = minValue;
    s.maxValue = maxValue;
    s.step = step;
    s.decimals = decimals;
    s.suffix = suffix ? suffix : L"";
    s.labelText = label;
    s.swatch = swatch;
    s.id = IDC_SLIDER_BASE + (int)g_app.sliders.size();

    s.track = CreateWindowExW(0, kSliderClass, L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                              parent, (HMENU)(INT_PTR)s.id, g_app.instance,
                              nullptr);
    SetWindowLongPtrW(s.track, GWLP_USERDATA, (LONG_PTR)&s);
}

void CreateUi(HWND hwnd)
{
    g_app.headerButtons.push_back(
        { CreateOwnerButton(hwnd, L"Open image", IDC_OPEN), IDC_OPEN,
          L"Open image", ButtonStyle::Secondary, {} });
    g_app.headerButtons.push_back(
        { CreateOwnerButton(hwnd, L"Export PNG", IDC_EXPORT), IDC_EXPORT,
          L"Export PNG", ButtonStyle::Primary, {} });
    g_app.headerButtons.push_back(
        { CreateOwnerButton(hwnd, L"Batch", IDC_BATCH), IDC_BATCH, L"Batch",
          ButtonStyle::Secondary, {} });
    g_app.headerButtons.push_back(
        { CreateOwnerButton(hwnd, L"About", IDC_ABOUT), IDC_ABOUT, L"About",
          ButtonStyle::Secondary, {} });

    g_app.tabPaper = CreateOwnerButton(hwnd, L"Paper Cutout", IDC_TAB_PAPER);
    g_app.tabHalftone = CreateOwnerButton(hwnd, L"Halftone Screen", IDC_TAB_HALFTONE);
    g_app.chainCheck = CreateSwitch(hwnd, L"Chain Halftone \u2192 Paper", IDC_CHAIN);
    g_app.resetButton = CreateOwnerButton(hwnd, L"Reset", IDC_RESET);

    g_app.paperPage = CreateWindowExW(0, kPageClass, L"",
                                      WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                      0, 0, 10, 10, hwnd, nullptr,
                                      g_app.instance, nullptr);
    g_app.halftonePage = CreateWindowExW(0, kPageClass, L"",
                                         WS_CHILD | WS_CLIPSIBLINGS |
                                             WS_CLIPCHILDREN,
                                         0, 0, 10, 10, hwnd, nullptr,
                                         g_app.instance, nullptr);

    g_app.sectionPaper0 =
        CreateStatic(g_app.paperPage, L"LAYOUT && FRAGMENTS", g_app.fontSection,
                     IDC_SECTION_BASE + 0);
    SetPropW(g_app.sectionPaper0, L"MMSection", (HANDLE)1);
    g_app.sectionPaper1 =
        CreateStatic(g_app.paperPage, L"EDGES && BACKDROP", g_app.fontSection,
                     IDC_SECTION_BASE + 1);
    SetPropW(g_app.sectionPaper1, L"MMSection", (HANDLE)1);
    g_app.sectionPaper2 =
        CreateStatic(g_app.paperPage, L"PAPER COLOUR", g_app.fontSection,
                     IDC_SECTION_BASE + 2);
    SetPropW(g_app.sectionPaper2, L"MMSection", (HANDLE)1);
    g_app.sectionHt0 =
        CreateStatic(g_app.halftonePage, L"SCREEN && INK", g_app.fontSection,
                     IDC_SECTION_BASE + 3);
    SetPropW(g_app.sectionHt0, L"MMSection", (HANDLE)1);
    g_app.sectionHt1 =
        CreateStatic(g_app.halftonePage, L"PLATE ANGLES && OFFSETS",
                     g_app.fontSection, IDC_SECTION_BASE + 4);
    SetPropW(g_app.sectionHt1, L"MMSection", (HANDLE)1);
    g_app.sectionHt2 =
        CreateStatic(g_app.halftonePage, L"INK CHANNELS", g_app.fontSection,
                     IDC_SECTION_BASE + 5);
    SetPropW(g_app.sectionHt2, L"MMSection", (HANDLE)1);

    PaperSettings& p = g_app.settings.paper;
    AddSlider(g_app.paperPage, PageKind::Paper, 0, 0, L"Columns", &p.columns, 1, 40, 1, 0, L"");
    AddSlider(g_app.paperPage, PageKind::Paper, 0, 1, L"Rows", &p.rows, 1, 40, 1, 0, L"");
    AddSlider(g_app.paperPage, PageKind::Paper, 0, 2, L"Missing Pieces", &p.missing, 0, 100, 1, 0, L"%");
    AddSlider(g_app.paperPage, PageKind::Paper, 0, 3, L"Seed", &p.seed, 0, 99999, 1, 0, L"");
    AddSlider(g_app.paperPage, PageKind::Paper, 0, 4, L"Rotation", &p.rotation, 0, 45, 1, 0, L"\u00B0");
    AddSlider(g_app.paperPage, PageKind::Paper, 0, 5, L"Size Variation", &p.sizeVar, 0, 1, 0.01, 2, L"");
    AddSlider(g_app.paperPage, PageKind::Paper, 0, 6, L"Width Variation", &p.widthVar, 0, 1, 0.01, 2, L"");
    AddSlider(g_app.paperPage, PageKind::Paper, 0, 7, L"Height Variation", &p.heightVar, 0, 1, 0.01, 2, L"");
    AddSlider(g_app.paperPage, PageKind::Paper, 0, 8, L"UV Shift", &p.uvShift, 0, 150, 1, 0, L" px");
    AddSlider(g_app.paperPage, PageKind::Paper, 0, 9, L"Color Variation", &p.colorVar, 0, 1, 0.01, 2, L"");
    AddSlider(g_app.paperPage, PageKind::Paper, 0, 10, L"Tear Size", &p.tear, 0, 120, 1, 0, L" px");
    AddSlider(g_app.paperPage, PageKind::Paper, 1, 0, L"Chunk Size", &p.chunk, 0, 240, 1, 0, L" px");
    AddSlider(g_app.paperPage, PageKind::Paper, 1, 1, L"Roughness", &p.roughness, 0, 1, 0.01, 2, L"");
    AddSlider(g_app.paperPage, PageKind::Paper, 1, 2, L"White Edge", &p.white, 0, 60, 1, 0, L" px");
    AddSlider(g_app.paperPage, PageKind::Paper, 1, 3, L"Unevenness", &p.unevenness, 0, 1, 0.01, 2, L"");
    AddSlider(g_app.paperPage, PageKind::Paper, 1, 4, L"Shadow", &p.shadow, 0, 1, 0.01, 2, L"");
    AddSlider(g_app.paperPage, PageKind::Paper, 1, 5, L"Background R", &p.bgR, 0, 1, 0.01, 2, L"", true);
    AddSlider(g_app.paperPage, PageKind::Paper, 1, 6, L"Background G", &p.bgG, 0, 1, 0.01, 2, L"", true);
    AddSlider(g_app.paperPage, PageKind::Paper, 1, 7, L"Background B", &p.bgB, 0, 1, 0.01, 2, L"", true);
    g_app.bgImageCheck =
        CreateSwitch(g_app.paperPage, L"Show source behind gaps", IDC_BGIMAGE);

    HalftoneSettings& h = g_app.settings.ht;
    const wchar_t* patterns[] = { L"Round", L"Ellipse", L"Square",
                                  L"Diamond", L"Line", L"Cross" };
    for (int i = 0; i < 6; ++i)
        g_app.patternChips[i] =
            CreateChip(g_app.halftonePage, patterns[i], IDC_PATTERN_BASE + i);

    AddSlider(g_app.halftonePage, PageKind::Halftone, 0, 1, L"Frequency", &h.frequency, 4, 400, 1, 0, L" lpi");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 0, 2, L"Roughness", &h.roughness, 0, 1, 0.01, 2, L"");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 0, 3, L"Fuzziness", &h.fuzziness, 0, 1, 0.01, 2, L"");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 0, 4, L"Paper Fiber", &h.fiber, 0, 1, 0.01, 2, L"");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 0, 5, L"Ink Texture", &h.inkTexture, 0, 1, 0.01, 2, L"");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 0, 6, L"Ink Density", &h.density, 0, 2, 0.01, 2, L"");

    g_app.channelC = CreateChip(g_app.halftonePage, L"C", IDC_CHAN_C);
    g_app.channelM = CreateChip(g_app.halftonePage, L"M", IDC_CHAN_M);
    g_app.channelY = CreateChip(g_app.halftonePage, L"Y", IDC_CHAN_Y);
    g_app.channelK = CreateChip(g_app.halftonePage, L"K", IDC_CHAN_K);

    AddSlider(g_app.halftonePage, PageKind::Halftone, 1, 0, L"Cyan Angle", &h.angle[0], -180, 180, 1, 0, L"\u00B0");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 1, 1, L"Magenta Angle", &h.angle[1], -180, 180, 1, 0, L"\u00B0");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 1, 2, L"Yellow Angle", &h.angle[2], -180, 180, 1, 0, L"\u00B0");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 1, 3, L"Black Angle", &h.angle[3], -180, 180, 1, 0, L"\u00B0");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 1, 4, L"Offset X Cyan", &h.offsetX[0], -200, 200, 1, 0, L" px");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 1, 5, L"Offset Y Cyan", &h.offsetY[0], -200, 200, 1, 0, L" px");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 1, 6, L"Offset X Magenta", &h.offsetX[1], -200, 200, 1, 0, L" px");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 1, 7, L"Offset Y Magenta", &h.offsetY[1], -200, 200, 1, 0, L" px");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 1, 8, L"Offset X Yellow", &h.offsetX[2], -200, 200, 1, 0, L" px");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 1, 9, L"Offset Y Yellow", &h.offsetY[2], -200, 200, 1, 0, L" px");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 1, 10, L"Offset X Black", &h.offsetX[3], -200, 200, 1, 0, L" px");
    AddSlider(g_app.halftonePage, PageKind::Halftone, 1, 11, L"Offset Y Black", &h.offsetY[3], -200, 200, 1, 0, L" px");

    g_app.originalCheck = CreateSwitch(hwnd, L"View original", IDC_ORIGINAL);

    if (g_app.iconSmall)
    {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_app.iconSmall);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_app.iconLarge);
    }

    ApplyFonts();
}

void LayoutPageControls(PageKind page)
{
    HWND pageHwnd =
        page == PageKind::Paper ? g_app.paperPage : g_app.halftonePage;
    RECT rc;
    GetClientRect(pageHwnd, &rc);
    const int w = rc.right;
    const int h = rc.bottom;

    const int pad = Dp(10);
    const int gap = Dp(16);
    const int colW = std::max(Dp(120), (w - 2 * pad - gap) / 2);
    const int x0 = pad;
    const int x1 = pad + colW + gap;
    const int headerY = Dp(5);
    const int rowStart = Dp(24);
    const int rows = page == PageKind::Paper ? 11 : 12;
    const int available = std::max(Dp(200), h - rowStart - Dp(2));
    const int rowH = ClampI(available / rows, Dp(36), Dp(48));
    g_app.pageDividerX = x0 + colW + gap / 2;

    if (page == PageKind::Paper)
    {
        MoveWindow(g_app.sectionPaper0, x0, headerY, colW, Dp(15), TRUE);
        MoveWindow(g_app.sectionPaper1, x1, headerY, colW, Dp(15), TRUE);
        MoveWindow(g_app.sectionPaper2, x1, rowStart + 9 * rowH - Dp(2), colW,
                   Dp(15), TRUE);
    }
    else
    {
        MoveWindow(g_app.sectionHt0, x0, headerY, colW, Dp(15), TRUE);
        MoveWindow(g_app.sectionHt1, x1, headerY, colW, Dp(15), TRUE);
        MoveWindow(g_app.sectionHt2, x0, rowStart + 7 * rowH + Dp(22), colW,
                   Dp(15), TRUE);
    }

    for (const Slider& s : g_app.sliders)
    {
        if (s.page != page)
            continue;
        const int x = s.col == 0 ? x0 : x1;
        int y = rowStart + s.row * rowH;
        if (page == PageKind::Halftone && s.col == 0)
            y = rowStart + Dp(64) + (s.row - 1) * rowH;
        MoveWindow(s.track, x, y, colW, rowH - Dp(4), TRUE);
    }

    if (page == PageKind::Paper)
    {
        const int y = rowStart + 8 * rowH + Dp(2);
        MoveWindow(g_app.bgImageCheck, x1, y, colW, Dp(26), TRUE);
        g_app.paperSwatchRect = { x1, rowStart + 9 * rowH + Dp(16), x1 + colW,
                                  rowStart + 9 * rowH + Dp(96) };
    }
    else
    {
        const int chipGap = Dp(4);
        const int chipW = (colW - 2 * chipGap) / 3;
        const int chipH = Dp(24);
        for (int i = 0; i < 6; ++i)
        {
            const int cx = x0 + (i % 3) * (chipW + chipGap);
            const int cy = rowStart + (i / 3) * (chipH + chipGap);
            MoveWindow(g_app.patternChips[i], cx, cy, chipW, chipH, TRUE);
        }
        const int channelY = rowStart + 7 * rowH + Dp(42);
        const int channelW = (colW - 3 * chipGap) / 4;
        HWND channels[] = { g_app.channelC, g_app.channelM, g_app.channelY,
                            g_app.channelK };
        for (int i = 0; i < 4; ++i)
            MoveWindow(channels[i], x0 + i * (channelW + chipGap), channelY,
                       channelW, Dp(26), TRUE);
        g_app.screenPreviewRect = { x0, channelY + Dp(40), x0 + colW,
                                    h - Dp(8) };
    }
}

void LayoutUi()
{
    RECT client;
    GetClientRect(g_app.hwnd, &client);
    const int w = client.right;
    const int h = client.bottom;

    g_app.headerRect = { Dp(16), Dp(14), w - Dp(16), Dp(94) };
    const int top = Dp(108);
    const int bottom = h - Dp(40);
    int leftW = std::max(Dp(440), std::min(Dp(500), (int)(w * 0.40)));
    g_app.panelRect = { Dp(16), top, Dp(16) + leftW, bottom };
    g_app.previewCardRect = { g_app.panelRect.right + Dp(16), top, w - Dp(16),
                              bottom };
    g_app.statusRect = { Dp(18), h - Dp(30), w - Dp(18), h - Dp(8) };

    const int buttonW = Dp(112);
    const int buttonH = Dp(36);
    const int gap = Dp(8);
    int bx = g_app.headerRect.right - Dp(16) - buttonW;
    for (int i = (int)g_app.headerButtons.size() - 1; i >= 0; --i)
    {
        g_app.headerButtons[i].rect = { bx, Dp(28), bx + buttonW,
                                        Dp(28) + buttonH };
        bx -= buttonW + gap;
    }
    for (const ButtonInfo& b : g_app.headerButtons)
        MoveWindow(b.hwnd, b.rect.left, b.rect.top,
                   b.rect.right - b.rect.left, b.rect.bottom - b.rect.top, TRUE);

    g_app.segmentRect = { g_app.panelRect.left + Dp(14),
                          g_app.panelRect.top + Dp(14),
                          g_app.panelRect.right - Dp(14),
                          g_app.panelRect.top + Dp(58) };
    const int segPad = Dp(4);
    const int segW = (g_app.segmentRect.right - g_app.segmentRect.left -
                      2 * segPad) / 2;
    MoveWindow(g_app.tabPaper, g_app.segmentRect.left + segPad,
               g_app.segmentRect.top + segPad, segW,
               g_app.segmentRect.bottom - g_app.segmentRect.top - 2 * segPad,
               TRUE);
    MoveWindow(g_app.tabHalftone, g_app.segmentRect.left + segPad + segW,
               g_app.segmentRect.top + segPad, segW,
               g_app.segmentRect.bottom - g_app.segmentRect.top - 2 * segPad,
               TRUE);

    g_app.chainRect = { g_app.panelRect.left + Dp(16),
                        g_app.panelRect.top + Dp(68),
                        g_app.panelRect.right - Dp(96),
                        g_app.panelRect.top + Dp(98) };
    MoveWindow(g_app.chainCheck, g_app.chainRect.left, g_app.chainRect.top,
               g_app.chainRect.right - g_app.chainRect.left,
               g_app.chainRect.bottom - g_app.chainRect.top, TRUE);

    g_app.resetRect = { g_app.panelRect.right - Dp(86),
                        g_app.panelRect.top + Dp(68),
                        g_app.panelRect.right - Dp(16),
                        g_app.panelRect.top + Dp(98) };
    MoveWindow(g_app.resetButton, g_app.resetRect.left, g_app.resetRect.top,
               g_app.resetRect.right - g_app.resetRect.left,
               g_app.resetRect.bottom - g_app.resetRect.top, TRUE);

    const int pageX = g_app.panelRect.left + Dp(8);
    const int pageY = g_app.panelRect.top + Dp(108);
    const int pageW = g_app.panelRect.right - pageX - Dp(8);
    const int pageH = g_app.panelRect.bottom - pageY - Dp(8);
    MoveWindow(g_app.paperPage, pageX, pageY, pageW, pageH, TRUE);
    MoveWindow(g_app.halftonePage, pageX, pageY, pageW, pageH, TRUE);

    LayoutPageControls(PageKind::Paper);
    LayoutPageControls(PageKind::Halftone);

    g_app.previewHeader = { g_app.previewCardRect.left + Dp(20),
                            g_app.previewCardRect.top + Dp(14),
                            g_app.previewCardRect.right - Dp(20),
                            g_app.previewCardRect.top + Dp(62) };
    g_app.previewRect = { g_app.previewCardRect.left + Dp(12),
                          g_app.previewCardRect.top + Dp(68),
                          g_app.previewCardRect.right - Dp(12),
                          g_app.previewCardRect.bottom - Dp(46) };
    g_app.previewFooter = { g_app.previewCardRect.left + Dp(20),
                            g_app.previewCardRect.bottom - Dp(40),
                            g_app.previewCardRect.right - Dp(20),
                            g_app.previewCardRect.bottom - Dp(12) };

    g_app.originalRect = { g_app.previewHeader.right - Dp(132),
                           g_app.previewHeader.top + Dp(4),
                           g_app.previewHeader.right,
                           g_app.previewHeader.top + Dp(30) };
    MoveWindow(g_app.originalCheck, g_app.originalRect.left,
               g_app.originalRect.top,
               g_app.originalRect.right - g_app.originalRect.left,
               g_app.originalRect.bottom - g_app.originalRect.top, TRUE);

    ShowWindow(g_app.paperPage, g_app.activeTab == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.halftonePage, g_app.activeTab == 1 ? SW_SHOW : SW_HIDE);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void DrawImageInRect(HDC dc, Gdiplus::Bitmap* bitmap, const RECT& area)
{
    if (!bitmap)
        return;
    const int iw = (int)bitmap->GetWidth();
    const int ih = (int)bitmap->GetHeight();
    if (iw <= 0 || ih <= 0)
        return;

    const int pad = Dp(18);
    const int availW = std::max(1, (int)(area.right - area.left) - pad * 2);
    const int availH = std::max(1, (int)(area.bottom - area.top) - pad * 2);
    double scale = std::min((double)availW / iw, (double)availH / ih);
    scale = std::min(scale, 1.0);
    const int dw = std::max(1, (int)std::lround(iw * scale));
    const int dh = std::max(1, (int)std::lround(ih * scale));
    const int dx = area.left + ((area.right - area.left) - dw) / 2;
    const int dy = area.top + ((area.bottom - area.top) - dh) / 2;

    Gdiplus::Graphics graphics(dc);
    Gdiplus::GraphicsPath clip;
    Gdiplus::RectF clipRect((Gdiplus::REAL)(area.left + Dp(2)),
                            (Gdiplus::REAL)(area.top + Dp(2)),
                            (Gdiplus::REAL)(area.right - area.left - Dp(4)),
                            (Gdiplus::REAL)(area.bottom - area.top - Dp(4)));
    AddRoundRect(clip, clipRect, (Gdiplus::REAL)Dp(16));
    graphics.SetClip(&clip);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetInterpolationMode(scale < 1.0
                                      ? Gdiplus::InterpolationModeHighQualityBicubic
                                      : Gdiplus::InterpolationModeNearestNeighbor);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.DrawImage(bitmap, Gdiplus::RectF((Gdiplus::REAL)dx, (Gdiplus::REAL)dy,
                                              (Gdiplus::REAL)dw, (Gdiplus::REAL)dh),
                       0, 0, (Gdiplus::REAL)iw, (Gdiplus::REAL)ih,
                       Gdiplus::UnitPixel);
}

void DrawPreviewPanel(HDC dc)
{
    const RECT& card = g_app.previewCardRect;
    DrawCard(dc, card, Dp(24), kDark, RGB(55, 50, 84), true, true);

    SetBkMode(dc, TRANSPARENT);
    std::wstring fileName = L"Sample composition";
    if (!g_app.currentPath.empty())
        fileName = std::filesystem::path(g_app.currentPath).filename().wstring();
    if (g_app.fullImage.empty())
        fileName = L"No image loaded";

    SelectObject(dc, g_app.fontTab);
    SetTextColor(dc, RGB(245, 244, 255));
    RECT nameRect = g_app.previewHeader;
    nameRect.right -= Dp(140);
    DrawTextW(dc, fileName.c_str(), -1, &nameRect,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(dc, g_app.fontSmall);
    SetTextColor(dc, RGB(164, 160, 190));
    wchar_t info[128];
    if (g_app.fullImage.empty())
        swprintf(info, 128, L"Drop an image or use Open image");
    else
        swprintf(info, 128, L"%d \u00D7 %d px  \u00B7  %s", g_app.fullImage.width,
                 g_app.fullImage.height,
                 g_app.settings.chain ? L"Halftone + Paper"
                                      : (g_app.activeTab == 1 ? L"Halftone"
                                                              : L"Paper"));
    RECT infoRect = g_app.previewHeader;
    infoRect.top += Dp(24);
    infoRect.right -= Dp(140);
    DrawTextW(dc, info, -1, &infoRect,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT inner = g_app.previewRect;
    InflateRect(&inner, -Dp(2), -Dp(2));
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath clip;
        Gdiplus::RectF clipRect((Gdiplus::REAL)inner.left,
                                (Gdiplus::REAL)inner.top,
                                (Gdiplus::REAL)(inner.right - inner.left),
                                (Gdiplus::REAL)(inner.bottom - inner.top));
        AddRoundRect(clip, clipRect, (Gdiplus::REAL)Dp(18));
        Gdiplus::SolidBrush previewBrush(Gd(kPreview));
        g.FillPath(&previewBrush, &clip);
        g.SetClip(&clip);
        Gdiplus::Pen grid(Gdiplus::Color(50, 46, 44, 74), 1.0f);
        for (int x = inner.left; x < inner.right; x += Dp(44))
            g.DrawLine(&grid, x, inner.top, x, inner.bottom);
        for (int y = inner.top; y < inner.bottom; y += Dp(44))
            g.DrawLine(&grid, inner.left, y, inner.right, y);
        g.ResetClip();
        StrokeRounded(g, clipRect, (Gdiplus::REAL)Dp(18),
                      Gdiplus::Color(90, 76, 70, 112), 1.0f);
    }

    Gdiplus::Bitmap* bitmap =
        g_app.showOriginal ? g_app.sourceBitmap.get() : g_app.previewBitmap.get();
    if (bitmap)
    {
        DrawImageInRect(dc, bitmap, g_app.previewRect);
    }
    else
    {
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_app.fontBody);
        SetTextColor(dc, RGB(160, 156, 185));
        RECT area = g_app.previewRect;
        DrawTextW(dc, L"Drop an image here\nor click Open image", -1, &area,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    }

    const wchar_t* effect = g_app.settings.chain
                                ? L"HALFTONE + PAPER"
                                : (g_app.activeTab == 1 ? L"HALFTONE" : L"PAPER");
    RECT badge = { g_app.previewFooter.left, g_app.previewFooter.top,
                   g_app.previewFooter.left + Dp(132),
                   g_app.previewFooter.bottom };
    DrawPill(dc, badge, kAccent, RGB(255, 255, 255), effect);

    wchar_t timing[64];
    if (g_app.showOriginal)
        swprintf(timing, 64, L"ORIGINAL");
    else
        swprintf(timing, 64, L"%.0f ms", g_app.previewMs);
    SelectObject(dc, g_app.fontSmall);
    SetTextColor(dc, RGB(174, 170, 198));
    RECT tr = g_app.previewFooter;
    DrawTextW(dc, timing, -1, &tr,
              DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
}

void DrawHeaderPanel(HDC dc, int w)
{
    DrawCard(dc, g_app.headerRect, Dp(22), kCard, kLine, true, false);
    if (g_app.iconLarge)
    {
        DrawIconEx(dc, g_app.headerRect.left + Dp(20),
                   g_app.headerRect.top + Dp(16), g_app.iconLarge, Dp(48),
                   Dp(48), 0, nullptr, DI_NORMAL);
    }

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_app.fontTitle);
    SetTextColor(dc, kInk);
    RECT title = { g_app.headerRect.left + Dp(82),
                   g_app.headerRect.top + Dp(10), w - Dp(520),
                   g_app.headerRect.top + Dp(46) };
    DrawTextW(dc, L"Mixed Media", -1, &title,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SelectObject(dc, g_app.fontSubtitle);
    SetTextColor(dc, kMuted);
    RECT sub = { g_app.headerRect.left + Dp(84),
                 g_app.headerRect.top + Dp(43), w - Dp(500),
                 g_app.headerRect.top + Dp(66) };
    DrawTextW(dc, L"Studio  \u00B7  Paper Cutout + Halftone Screen", -1, &sub,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT accent = { g_app.headerRect.left + Dp(84),
                    g_app.headerRect.bottom - Dp(11),
                    g_app.headerRect.left + Dp(196),
                    g_app.headerRect.bottom - Dp(8) };
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    FillRounded(g, Gdiplus::RectF((Gdiplus::REAL)accent.left,
                                  (Gdiplus::REAL)accent.top,
                                  (Gdiplus::REAL)(accent.right - accent.left),
                                  (Gdiplus::REAL)(accent.bottom - accent.top)),
                (Gdiplus::REAL)Dp(2), Gd(kAccent));
}

void DrawStatusBar(HDC dc, int w)
{
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_app.fontSmall);

    std::wstring left;
    if (g_app.batchRunning)
    {
        left = g_app.batchStatus;
    }
    else if (g_app.fullImage.empty())
    {
        left = L"Open an image to begin  \u00B7  PNG, JPEG, BMP or TIFF";
    }
    else
    {
        wchar_t buf[256];
        swprintf(buf, 256, L"%d \u00D7 %d  \u00B7  %s  \u00B7  preview %.0f ms",
                 g_app.fullImage.width, g_app.fullImage.height,
                 g_app.settings.chain
                     ? L"Halftone + Paper"
                     : (g_app.activeTab == 1 ? L"Halftone" : L"Paper"),
                 g_app.previewMs);
        left = buf;
        if (!g_app.statusMessage.empty())
            left += L"  \u00B7  " + g_app.statusMessage;
    }

    SetTextColor(dc, kMuted);
    RECT leftRect = { g_app.statusRect.left, g_app.statusRect.top,
                      g_app.statusRect.right - Dp(230), g_app.statusRect.bottom };
    DrawTextW(dc, left.c_str(), -1, &leftRect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    const bool ofx = IsOfxInstalled();
    SetTextColor(dc, ofx ? RGB(47, 128, 90) : kMuted);
    RECT rightRect = { g_app.statusRect.right - Dp(232), g_app.statusRect.top,
                       g_app.statusRect.right - Dp(18), g_app.statusRect.bottom };
    DrawTextW(dc, ofx ? L"Resolve / Fusion OFX installed"
                      : L"Resolve / Fusion OFX not installed",
              -1, &rightRect,
              DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT dot = { g_app.statusRect.right - Dp(11),
                 g_app.statusRect.top + Dp(6),
                 g_app.statusRect.right - Dp(5), g_app.statusRect.top + Dp(12) };
    FillSolid(dc, dot, ofx ? RGB(47, 160, 108) : RGB(190, 160, 120));
    (void)w;
}

void DrawAll(HDC dc, int w, int h)
{
    {
        Gdiplus::Graphics bg(dc);
        Gdiplus::LinearGradientBrush gradient(
            Gdiplus::RectF(0, 0, (Gdiplus::REAL)w, (Gdiplus::REAL)h), Gd(kBgTop),
            Gd(kBgBottom), Gdiplus::LinearGradientModeVertical);
        bg.FillRectangle(&gradient, 0, 0, w, h);
    }

    DrawHeaderPanel(dc, w);
    DrawCard(dc, g_app.panelRect, Dp(24), kCard, kLine, true, false);
    DrawPreviewPanel(dc);

    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::RectF seg((Gdiplus::REAL)g_app.segmentRect.left,
                           (Gdiplus::REAL)g_app.segmentRect.top,
                           (Gdiplus::REAL)(g_app.segmentRect.right -
                                           g_app.segmentRect.left),
                           (Gdiplus::REAL)(g_app.segmentRect.bottom -
                                           g_app.segmentRect.top));
        FillRounded(g, seg, (Gdiplus::REAL)Dp(12), Gd(kCardAlt));
    }

    RECT divider = { g_app.panelRect.left + Dp(16),
                     g_app.panelRect.top + Dp(104),
                     g_app.panelRect.right - Dp(16),
                     g_app.panelRect.top + Dp(105) };
    FillRect(dc, &divider, g_app.brushLine);

    DrawStatusBar(dc, w);
}

void DrawOwnerButton(const DRAWITEMSTRUCT* dis, const std::wstring& text,
                     ButtonStyle style)
{
    RECT rc = dis->rcItem;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool compact = (dis->CtlID == IDC_RESET);

    COLORREF fill = kCard;
    COLORREF border = kLine;
    COLORREF textColor = kInk;
    if (style == ButtonStyle::Primary)
    {
        fill = disabled ? RGB(176, 164, 220)
                        : (pressed ? kAccentDark : kAccent);
        border = fill;
        textColor = RGB(255, 255, 255);
    }
    else if (style == ButtonStyle::Ghost)
    {
        fill = kCard;
        border = kCard;
        textColor = pressed ? kAccentDark : kMuted;
    }
    else
    {
        fill = pressed ? kCardAlt : kCard;
        border = kLine;
        textColor = kInk;
    }

    Gdiplus::Graphics g((HDC)dis->hDC);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    if (style == ButtonStyle::Primary && !pressed && !disabled)
    {
        Gdiplus::RectF shadow((Gdiplus::REAL)rc.left,
                              (Gdiplus::REAL)rc.top + Dp(2),
                              (Gdiplus::REAL)(rc.right - rc.left),
                              (Gdiplus::REAL)(rc.bottom - rc.top));
        FillRounded(g, shadow, (Gdiplus::REAL)Dp(12),
                    Gdiplus::Color(36, 124, 92, 255));
    }
    if (pressed)
        OffsetRect(&rc, 0, Dp(1));
    Gdiplus::RectF r((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                     (Gdiplus::REAL)(rc.right - rc.left),
                     (Gdiplus::REAL)(rc.bottom - rc.top));
    FillRounded(g, r, (Gdiplus::REAL)Dp(compact ? 9 : 12), Gd(fill));
    if (style != ButtonStyle::Ghost)
        StrokeRounded(g, r, (Gdiplus::REAL)Dp(compact ? 9 : 12), Gd(border),
                      1.0f);

    SetBkMode((HDC)dis->hDC, TRANSPARENT);
    SelectObject((HDC)dis->hDC, g_app.fontButton);
    SetTextColor((HDC)dis->hDC, textColor);
    DrawTextW((HDC)dis->hDC, text.c_str(), -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawSegment(const DRAWITEMSTRUCT* dis, const std::wstring& text,
                 bool active)
{
    RECT rc = dis->rcItem;
    Gdiplus::Graphics g((HDC)dis->hDC);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF r((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                     (Gdiplus::REAL)(rc.right - rc.left),
                     (Gdiplus::REAL)(rc.bottom - rc.top));
    FillRounded(g, r, (Gdiplus::REAL)Dp(10),
                Gd(active ? kDark : kCardAlt));
    SetBkMode((HDC)dis->hDC, TRANSPARENT);
    SelectObject((HDC)dis->hDC, g_app.fontTab);
    SetTextColor((HDC)dis->hDC, active ? RGB(255, 255, 255) : kMuted);
    DrawTextW((HDC)dis->hDC, text.c_str(), -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawSwitch(const DRAWITEMSTRUCT* dis, bool on, const wchar_t* text,
                bool dark)
{
    RECT rc = dis->rcItem;
    FillSolid((HDC)dis->hDC, rc, dark ? kDark : kCard);
    SetBkMode((HDC)dis->hDC, TRANSPARENT);
    SelectObject((HDC)dis->hDC, g_app.fontLabel);
    SetTextColor((HDC)dis->hDC, dark ? RGB(228, 226, 242) : kInk);
    RECT textRect = rc;
    textRect.right -= Dp(48);
    DrawTextW((HDC)dis->hDC, text, -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const int sw = Dp(38);
    const int sh = Dp(20);
    RECT sr = { rc.right - sw - Dp(2), rc.top + (rc.bottom - rc.top - sh) / 2,
                rc.right - Dp(2), rc.top + (rc.bottom - rc.top + sh) / 2 };
    Gdiplus::Graphics g((HDC)dis->hDC);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF track((Gdiplus::REAL)sr.left, (Gdiplus::REAL)sr.top,
                         (Gdiplus::REAL)(sr.right - sr.left),
                         (Gdiplus::REAL)(sr.bottom - sr.top));
    const Gdiplus::Color trackColor =
        on ? Gd(kAccent)
           : Gdiplus::Color(255, dark ? 62 : 214, dark ? 58 : 220,
                            dark ? 92 : 234);
    FillRounded(g, track, (Gdiplus::REAL)sh / 2.0f, trackColor);
    const float thumbR = (float)Dp(8);
    const float thumbX = on ? (float)sr.right - thumbR - 1.0f
                            : (float)sr.left + thumbR + 1.0f;
    const float thumbY = (float)sr.top + (float)sh / 2.0f;
    Gdiplus::SolidBrush thumbShadow(Gdiplus::Color(45, 20, 15, 40));
    g.FillEllipse(&thumbShadow, thumbX - thumbR + 1, thumbY - thumbR + 2,
                  thumbR * 2, thumbR * 2);
    Gdiplus::SolidBrush thumbBrush(Gdiplus::Color(255, 255, 255, 255));
    g.FillEllipse(&thumbBrush, thumbX - thumbR, thumbY - thumbR, thumbR * 2,
                  thumbR * 2);
}

void DrawChip(const DRAWITEMSTRUCT* dis, const wchar_t* text, bool active,
              COLORREF dotColor, bool hasDot)
{
    RECT rc = dis->rcItem;
    FillSolid((HDC)dis->hDC, rc, kCard);
    Gdiplus::Graphics g((HDC)dis->hDC);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF r((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                     (Gdiplus::REAL)(rc.right - rc.left),
                     (Gdiplus::REAL)(rc.bottom - rc.top));
    FillRounded(g, r, (Gdiplus::REAL)Dp(8),
                Gd(active ? kAccent : kCardAlt));
    SetBkMode((HDC)dis->hDC, TRANSPARENT);
    SelectObject((HDC)dis->hDC, g_app.fontBadge);
    SetTextColor((HDC)dis->hDC, active ? RGB(255, 255, 255) : kMuted);
    RECT tr = rc;
    if (hasDot)
    {
        const int d = Dp(6);
        RECT dot = { rc.left + Dp(8), rc.top + (rc.bottom - rc.top - d) / 2,
                     rc.left + Dp(8) + d,
                     rc.top + (rc.bottom - rc.top + d) / 2 };
        Gdiplus::RectF dr((Gdiplus::REAL)dot.left, (Gdiplus::REAL)dot.top,
                          (Gdiplus::REAL)(dot.right - dot.left),
                          (Gdiplus::REAL)(dot.bottom - dot.top));
        Gdiplus::SolidBrush dotBrush(Gd(dotColor));
        g.FillEllipse(&dotBrush, dr);
        tr.left += Dp(18);
    }
    DrawTextW((HDC)dis->hDC, text, -1, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void ResetSettings()
{
    g_app.settings = Settings{};
    SyncSliders();
    UpdateControls();
    g_app.statusMessage = L"settings reset";
    SchedulePreview();
}

void ExportPng()
{
    if (g_app.previewSource.empty())
    {
        MessageBoxW(g_app.hwnd, L"Open an image first.", kAppName,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring path;
    if (!PickImageFile(g_app.hwnd, true, path))
        return;
    if (LowerExtension(path) != L".png")
        path += L".png";

    g_app.statusMessage = L"rendering full resolution...";
    InvalidateRect(g_app.hwnd, &g_app.statusRect, FALSE);
    UpdateWindow(g_app.hwnd);
    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    ImageRGBA out;
    RenderChain(g_app.fullImage, g_app.settings, g_app.settings.chain,
                g_app.activeTab == 1, out);
    std::wstring error;
    const bool ok = SavePngFile(path.c_str(), out, &error);
    SetCursor(oldCursor);

    if (!ok)
    {
        g_app.statusMessage.clear();
        std::wstring message = L"Could not export the PNG:\n\n";
        message += path;
        message += L"\n\n";
        message += error;
        ShowError(g_app.hwnd, message);
        return;
    }

    std::filesystem::path p(path);
    g_app.lastDir = p.parent_path().wstring();
    g_app.statusMessage = L"saved " + p.filename().wstring();
    InvalidateRect(g_app.hwnd, &g_app.statusRect, FALSE);
}

void PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            g_app.batchCancel = true;
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void SetBatchUi(bool running)
{
    g_app.batchRunning = running;
    EnableWindow(g_app.headerButtons[0].hwnd, !running);
    EnableWindow(g_app.headerButtons[1].hwnd, !running);
    EnableWindow(g_app.headerButtons[3].hwnd, !running);
    EnableWindow(g_app.chainCheck, !running);
    EnableWindow(g_app.resetButton, !running);
    EnableWindow(g_app.tabPaper, !running);
    EnableWindow(g_app.tabHalftone, !running);
    EnableWindow(g_app.paperPage, !running);
    EnableWindow(g_app.halftonePage, !running);
    EnableWindow(g_app.originalCheck, !running);
    g_app.headerButtons[2].text = running ? L"Cancel" : L"Batch";
    InvalidateRect(g_app.headerButtons[2].hwnd, nullptr, TRUE);
}

void RunBatch()
{
    if (g_app.batchRunning)
    {
        g_app.batchCancel = true;
        return;
    }
    if (g_app.fullImage.empty())
    {
        MessageBoxW(g_app.hwnd, L"Open an image first so the look is set up.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring inputDir;
    std::wstring outputDir;
    if (!PickFolder(g_app.hwnd, L"Choose the folder of source images", inputDir))
        return;
    if (!PickFolder(g_app.hwnd, L"Choose the output folder", outputDir))
        return;

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(inputDir, ec))
    {
        if (ec)
            break;
        if (entry.is_regular_file(ec) && IsImageExtension(entry.path()))
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty())
    {
        MessageBoxW(g_app.hwnd,
                    L"No PNG, JPEG, BMP or TIFF images were found in that "
                    L"folder.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring question =
        L"Apply the current look to " + std::to_wstring(files.size()) +
        L" image(s)?\n\nOutput folder:\n" + outputDir +
        L"\n\nFiles will be written as name_mixedmedia.png.";
    if (MessageBoxW(g_app.hwnd, question.c_str(), L"Batch render",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    std::filesystem::create_directories(outputDir, ec);

    const Settings snapshot = g_app.settings;
    const bool chain = snapshot.chain;
    const bool halftoneOnly = g_app.activeTab == 1;

    g_app.batchCancel = false;
    g_app.statusMessage.clear();
    SetBatchUi(true);
    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    int done = 0;
    int failed = 0;
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (g_app.batchCancel)
            break;

        const std::filesystem::path& input = files[i];
        std::wstring name = input.stem().wstring() + L"_mixedmedia.png";
        const std::filesystem::path output = std::filesystem::path(outputDir) / name;

        g_app.batchStatus = L"Batch " + std::to_wstring(i + 1) + L"/" +
                            std::to_wstring(files.size()) + L"  \u00B7  " +
                            input.filename().wstring();
        InvalidateRect(g_app.hwnd, &g_app.statusRect, FALSE);
        UpdateWindow(g_app.hwnd);

        ImageRGBA full;
        std::wstring error;
        if (!LoadImageFile(input.c_str(), full, 0, 0, nullptr, &error))
        {
            ++failed;
            continue;
        }
        ImageRGBA out;
        if (!RenderChain(full, snapshot, chain, halftoneOnly, out))
        {
            ++failed;
            continue;
        }
        if (!SavePngFile(output.c_str(), out, &error))
        {
            ++failed;
            continue;
        }
        ++done;
        PumpMessages();
    }

    SetCursor(oldCursor);
    SetBatchUi(false);
    g_app.batchStatus.clear();
    RefreshStatus();

    std::wstring summary = L"Batch finished.\n\nRendered: " +
                           std::to_wstring(done) + L"\nFailed: " +
                           std::to_wstring(failed);
    if (g_app.batchCancel)
        summary += L"\n\nCancelled.";
    MessageBoxW(g_app.hwnd, summary.c_str(), L"Batch render",
                MB_OK | (failed ? MB_ICONWARNING : MB_ICONINFORMATION));
}

void ShowAbout()
{
    std::wstring text =
        L"Mixed Media Studio " + std::wstring(kVersion) +
        L"\nPaper Cutout + Halftone Screen\n\n"
        L"Standalone Windows app built on the Mixed Media OpenFX renderer.\n\n"
        L"DaVinci Resolve and Fusion load the OpenFX plugin directly.\n"
        L"After Effects, Premiere Pro and Filmora do not support OpenFX.\n"
        L"Use Export PNG or Batch here, then import the PNGs into those\n"
        L"editors. For video, export a PNG sequence from your editor, batch\n"
        L"it here, and import the rendered sequence back.\n\n"
        L"Plugin log: %LOCALAPPDATA%\\MixedMediaOFX\\mixedmedia.log\n"
        L"MIT licensed. OpenFX headers are BSD-3-Clause.\n"
        L"Not affiliated with Effect.app or Blackmagic Design.";
    MessageBoxW(g_app.hwnd, text.c_str(), L"About Mixed Media Studio",
                MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
// Main window procedure
// ---------------------------------------------------------------------------

void OnSliderChanged(HWND track)
{
    for (Slider& s : g_app.sliders)
    {
        if (s.track != track)
            continue;
        if (s.swatch && g_app.paperPage)
            InvalidateRect(g_app.paperPage, &g_app.paperSwatchRect, FALSE);
        if (s.page == PageKind::Halftone && g_app.halftonePage)
            InvalidateRect(g_app.halftonePage, &g_app.screenPreviewRect, FALSE);
        SchedulePreview();
        return;
    }
}

void OnCommand(WPARAM wParam, LPARAM lParam)
{
    const int id = LOWORD(wParam);
    const int code = HIWORD(wParam);
    HWND control = (HWND)lParam;
    (void)control;

    if (g_app.batchRunning && id != IDC_BATCH)
        return;

    switch (id)
    {
    case IDC_OPEN:
        if (code == BN_CLICKED)
        {
            std::wstring path;
            if (PickImageFile(g_app.hwnd, false, path))
                LoadImageIntoApp(path, true);
        }
        break;
    case IDC_EXPORT:
        if (code == BN_CLICKED)
            ExportPng();
        break;
    case IDC_BATCH:
        if (code == BN_CLICKED)
            RunBatch();
        break;
    case IDC_ABOUT:
        if (code == BN_CLICKED)
            ShowAbout();
        break;
    case IDC_RESET:
        if (code == BN_CLICKED)
            ResetSettings();
        break;
    case IDC_TAB_PAPER:
        if (code == BN_CLICKED)
        {
            g_app.activeTab = 0;
            ShowWindow(g_app.paperPage, SW_SHOW);
            ShowWindow(g_app.halftonePage, SW_HIDE);
            InvalidateRect(g_app.tabPaper, nullptr, FALSE);
            InvalidateRect(g_app.tabHalftone, nullptr, FALSE);
            SchedulePreview();
        }
        break;
    case IDC_TAB_HALFTONE:
        if (code == BN_CLICKED)
        {
            g_app.activeTab = 1;
            ShowWindow(g_app.paperPage, SW_HIDE);
            ShowWindow(g_app.halftonePage, SW_SHOW);
            InvalidateRect(g_app.tabPaper, nullptr, FALSE);
            InvalidateRect(g_app.tabHalftone, nullptr, FALSE);
            SchedulePreview();
        }
        break;
    case IDC_CHAIN:
        if (code == BN_CLICKED)
        {
            g_app.settings.chain = !g_app.settings.chain;
            InvalidateRect(g_app.chainCheck, nullptr, FALSE);
            SchedulePreview();
        }
        break;
    case IDC_ORIGINAL:
        if (code == BN_CLICKED)
        {
            g_app.showOriginal = !g_app.showOriginal;
            InvalidateRect(g_app.originalCheck, nullptr, FALSE);
            InvalidatePreview();
            RefreshStatus();
        }
        break;
    case IDC_BGIMAGE:
        if (code == BN_CLICKED)
        {
            g_app.settings.paper.bgImage = !g_app.settings.paper.bgImage;
            InvalidateRect(g_app.bgImageCheck, nullptr, FALSE);
            SchedulePreview();
        }
        break;
    case IDC_CHAN_C:
        if (code == BN_CLICKED)
        {
            g_app.settings.ht.useC = !g_app.settings.ht.useC;
            InvalidateRect(g_app.channelC, nullptr, FALSE);
            if (g_app.halftonePage)
                InvalidateRect(g_app.halftonePage, &g_app.screenPreviewRect,
                               FALSE);
            SchedulePreview();
        }
        break;
    case IDC_CHAN_M:
        if (code == BN_CLICKED)
        {
            g_app.settings.ht.useM = !g_app.settings.ht.useM;
            InvalidateRect(g_app.channelM, nullptr, FALSE);
            if (g_app.halftonePage)
                InvalidateRect(g_app.halftonePage, &g_app.screenPreviewRect,
                               FALSE);
            SchedulePreview();
        }
        break;
    case IDC_CHAN_Y:
        if (code == BN_CLICKED)
        {
            g_app.settings.ht.useY = !g_app.settings.ht.useY;
            InvalidateRect(g_app.channelY, nullptr, FALSE);
            if (g_app.halftonePage)
                InvalidateRect(g_app.halftonePage, &g_app.screenPreviewRect,
                               FALSE);
            SchedulePreview();
        }
        break;
    case IDC_CHAN_K:
        if (code == BN_CLICKED)
        {
            g_app.settings.ht.useK = !g_app.settings.ht.useK;
            InvalidateRect(g_app.channelK, nullptr, FALSE);
            if (g_app.halftonePage)
                InvalidateRect(g_app.halftonePage, &g_app.screenPreviewRect,
                               FALSE);
            SchedulePreview();
        }
        break;
    default:
        if (id >= IDC_PATTERN_BASE && id < IDC_PATTERN_BASE + 6 &&
            code == BN_CLICKED)
        {
            g_app.settings.ht.pattern = (double)(id - IDC_PATTERN_BASE);
            for (HWND chip : g_app.patternChips)
                if (chip)
                    InvalidateRect(chip, nullptr, FALSE);
            if (g_app.halftonePage)
                InvalidateRect(g_app.halftonePage, &g_app.screenPreviewRect,
                               FALSE);
            SchedulePreview();
        }
        break;
    }
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        g_app.hwnd = hwnd;
        g_app.iconLarge =
            (HICON)LoadImageW(g_app.instance, MAKEINTRESOURCEW(IDI_APP),
                              IMAGE_ICON, Dp(48), Dp(48), LR_DEFAULTCOLOR);
        g_app.iconSmall =
            (HICON)LoadImageW(g_app.instance, MAKEINTRESOURCEW(IDI_APP),
                              IMAGE_ICON, Dp(16), Dp(16), LR_DEFAULTCOLOR);
        RecreateFonts();
        CreateBrushes();
        CreateUi(hwnd);
        LayoutUi();
        DragAcceptFiles(hwnd, TRUE);
        if (!g_app.initialPath.empty())
            LoadImageIntoApp(g_app.initialPath, true);
        else
        {
            ImageRGBA sample, preview;
            if (MakeSampleImage(sample))
            {
                ScaleImage(sample, kPreviewMaxW, kPreviewMaxH, preview, nullptr);
                SetSourceImage(std::move(sample), std::move(preview), L"");
            }
        }
        return 0;

    case WM_SIZE:
        if (g_app.hwnd)
        {
            LayoutUi();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (!g_app.previewSource.empty())
                SchedulePreview();
        }
        return 0;

    case WM_DPICHANGED:
        g_app.dpi = HIWORD(wParam);
        RecreateFonts();
        ApplyFonts();
        {
            RECT* suggested = (RECT*)lParam;
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutUi();
        InvalidateRect(hwnd, nullptr, TRUE);
        if (!g_app.previewSource.empty())
            SchedulePreview();
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* info = (MINMAXINFO*)lParam;
        info->ptMinTrackSize.x = Dp(1080);
        info->ptMinTrackSize.y = Dp(800);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        const int w = client.right;
        const int h = client.bottom;

        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
        HGDIOBJ old = SelectObject(mem, bmp);
        DrawAll(mem, w, h);
        BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_HSCROLL:
        OnSliderChanged((HWND)lParam);
        return 0;

    case WM_COMMAND:
        OnCommand(wParam, lParam);
        return 0;

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
        if (dis->CtlType != ODT_BUTTON)
            return FALSE;

        if (dis->CtlID == IDC_OPEN || dis->CtlID == IDC_EXPORT ||
            dis->CtlID == IDC_BATCH || dis->CtlID == IDC_ABOUT)
        {
            for (const ButtonInfo& b : g_app.headerButtons)
            {
                if (b.id == (int)dis->CtlID)
                {
                    DrawOwnerButton(dis, b.text, b.style);
                    return TRUE;
                }
            }
        }
        if (dis->CtlID == IDC_RESET)
        {
            DrawOwnerButton(dis, L"Reset", ButtonStyle::Secondary);
            return TRUE;
        }
        if (dis->CtlID == IDC_TAB_PAPER)
        {
            DrawSegment(dis, L"Paper Cutout", g_app.activeTab == 0);
            return TRUE;
        }
        if (dis->CtlID == IDC_TAB_HALFTONE)
        {
            DrawSegment(dis, L"Halftone Screen", g_app.activeTab == 1);
            return TRUE;
        }
        if (dis->CtlID == IDC_CHAIN)
        {
            DrawSwitch(dis, g_app.settings.chain, L"Chain Halftone \u2192 Paper",
                       false);
            return TRUE;
        }
        if (dis->CtlID == IDC_ORIGINAL)
        {
            DrawSwitch(dis, g_app.showOriginal, L"View original", true);
            return TRUE;
        }
        if (dis->CtlID == IDC_BGIMAGE)
        {
            DrawSwitch(dis, g_app.settings.paper.bgImage,
                       L"Show source behind gaps", false);
            return TRUE;
        }
        if (dis->CtlID == IDC_CHAN_C)
        {
            DrawChip(dis, L"C", g_app.settings.ht.useC, kCyan, true);
            return TRUE;
        }
        if (dis->CtlID == IDC_CHAN_M)
        {
            DrawChip(dis, L"M", g_app.settings.ht.useM, kMagenta, true);
            return TRUE;
        }
        if (dis->CtlID == IDC_CHAN_Y)
        {
            DrawChip(dis, L"Y", g_app.settings.ht.useY, kGold, true);
            return TRUE;
        }
        if (dis->CtlID == IDC_CHAN_K)
        {
            DrawChip(dis, L"K", g_app.settings.ht.useK, kInk, true);
            return TRUE;
        }
        if (dis->CtlID >= IDC_PATTERN_BASE && dis->CtlID < IDC_PATTERN_BASE + 6)
        {
            const int pattern = (int)dis->CtlID - IDC_PATTERN_BASE;
            const wchar_t* names[] = { L"Round", L"Ellipse", L"Square",
                                       L"Diamond", L"Line", L"Cross" };
            DrawChip(dis, names[pattern],
                     (int)std::lround(g_app.settings.ht.pattern) == pattern,
                     kAccent, false);
            return TRUE;
        }
        return FALSE;
    }

    case WM_DROPFILES:
    {
        HDROP drop = (HDROP)wParam;
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFileW(drop, 0, path, MAX_PATH))
        {
            std::filesystem::path p(path);
            if (IsImageExtension(p))
                LoadImageIntoApp(path, true);
            else
                MessageBoxW(hwnd, L"Drop a PNG, JPEG, BMP or TIFF image.",
                            kAppName, MB_OK | MB_ICONINFORMATION);
        }
        DragFinish(drop);
        return 0;
    }

    case WM_TIMER:
        if (wParam == kPreviewTimer)
        {
            KillTimer(hwnd, kPreviewTimer);
            RenderPreviewNow();
        }
        return 0;

    case WM_CLOSE:
        if (g_app.batchRunning)
        {
            g_app.batchCancel = true;
            return 0;
        }
        SaveSettings();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        DragAcceptFiles(hwnd, FALSE);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// Headless modes and entry point
// ---------------------------------------------------------------------------

int RunHeadless(int argc, wchar_t** argv)
{
    bool selftest = false;
    std::wstring input;
    std::wstring output;
    bool chain = true;
    bool halftoneOnly = false;

    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if (arg == L"--selftest")
        {
            selftest = true;
            if (i + 1 < argc)
                output = argv[++i];
        }
        else if (arg == L"--render")
        {
            if (i + 2 < argc)
            {
                input = argv[++i];
                output = argv[++i];
            }
        }
        else if (arg == L"--paper-only")
        {
            chain = false;
            halftoneOnly = false;
        }
        else if (arg == L"--halftone-only")
        {
            chain = false;
            halftoneOnly = true;
        }
        else if (arg == L"--both")
        {
            chain = true;
        }
    }

    if (output.empty())
        return 2;

    ImageRGBA source, preview;
    if (selftest)
    {
        if (!MakeSampleImage(source))
            return 3;
    }
    else
    {
        if (!LoadImageFile(input.c_str(), source, 0, 0, nullptr, nullptr))
            return 4;
    }

    Settings defaults;
    ImageRGBA out;
    if (!RenderChain(source, defaults, chain, halftoneOnly, out))
        return 5;
    if (!SavePngFile(output.c_str(), out, nullptr))
        return 6;
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_app.instance = instance;
    g_app.dpi = GetDpiForSystem();

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            std::wstring arg = argv[i];
            if (arg == L"--selftest" || arg == L"--render")
                g_app.initialPath = L"__headless__";
            else if (g_app.initialPath.empty() && arg.size() > 1 &&
                     arg[0] != L'-')
                g_app.initialPath = arg;
        }
    }

    if (g_app.initialPath == L"__headless__")
    {
        int code = 1;
        if (SUCCEEDED(OleInitialize(nullptr)))
        {
            Gdiplus::GdiplusStartupInput input;
            ULONG_PTR token = 0;
            if (Gdiplus::GdiplusStartup(&token, &input, nullptr) ==
                Gdiplus::Ok)
            {
                code = RunHeadless(argc, argv);
                Gdiplus::GdiplusShutdown(token);
            }
            OleUninitialize();
        }
        if (argv)
            LocalFree(argv);
        return code;
    }

    const HRESULT oleHr = OleInitialize(nullptr);
    Gdiplus::GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, nullptr) != Gdiplus::Ok)
    {
        if (SUCCEEDED(oleHr))
            OleUninitialize();
        if (argv)
            LocalFree(argv);
        return 1;
    }

    LoadSettings();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kMainClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc))
        return 1;

    wc.lpfnWndProc = PageProc;
    wc.lpszClassName = kPageClass;
    wc.hIcon = nullptr;
    wc.hIconSm = nullptr;
    wc.hCursor = nullptr;
    if (!RegisterClassExW(&wc))
        return 1;

    wc.lpfnWndProc = SliderProc;
    wc.lpszClassName = kSliderClass;
    if (!RegisterClassExW(&wc))
        return 1;

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    int width = Dp(1320), height = Dp(820);
    if (g_app.hasSavedWindowRect)
    {
        RECT saved = g_app.savedWindowRect;
        if (MonitorFromRect(&saved, MONITOR_DEFAULTTONULL) != nullptr)
        {
            x = saved.left;
            y = saved.top;
            width = saved.right - saved.left;
            height = saved.bottom - saved.top;
        }
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_CONTROLPARENT, kMainClass, kAppName,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, x, y, width,
        height, nullptr, nullptr, instance, nullptr);
    if (!hwnd)
        return 1;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    HACCEL accel = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDR_ACCEL));
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (accel && TranslateAcceleratorW(hwnd, accel, &msg))
            continue;
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    DestroyBrushes();
    if (g_app.iconLarge)
        DestroyIcon(g_app.iconLarge);
    if (g_app.iconSmall)
        DestroyIcon(g_app.iconSmall);
    Gdiplus::GdiplusShutdown(gdiToken);
    if (SUCCEEDED(oleHr))
        OleUninitialize();
    if (argv)
        LocalFree(argv);
    return (int)msg.wParam;
}
