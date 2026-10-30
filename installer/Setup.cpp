// Mixed Media Studio Setup - self-contained Windows installer.
//
// The installer embeds the standalone Studio app and the native OpenFX plugin.
// It installs the Studio app for every editor, and optionally the OFX plugin
// for DaVinci Resolve / Fusion. After Effects, Premiere Pro and Filmora do not
// load OpenFX; the Studio app is the cross-editor path.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <uxtheme.h>
#include <gdiplus.h>
#include <objidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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
const wchar_t* kSetupName = L"Mixed Media Studio Setup";
const wchar_t* kVersion = L"1.0.0";
const wchar_t* kWindowClass = L"MixedMediaStudio.Setup";

const COLORREF kBgTop = RGB(232, 238, 248);
const COLORREF kBgBottom = RGB(204, 216, 234);
const COLORREF kCard = RGB(249, 250, 254);
const COLORREF kCardAlt = RGB(237, 241, 249);
const COLORREF kInk = RGB(28, 27, 45);
const COLORREF kMuted = RGB(112, 118, 140);
const COLORREF kLine = RGB(216, 222, 234);
const COLORREF kAccent = RGB(124, 92, 255);
const COLORREF kAccentDark = RGB(94, 63, 220);
const COLORREF kAccentSoft = RGB(235, 231, 255);
const COLORREF kDark = RGB(30, 28, 49);
const COLORREF kGreen = RGB(64, 196, 140);

enum ControlId : int
{
    IDC_PATH = 2000,
    IDC_BROWSE,
    IDC_OFX,
    IDC_DESKTOP,
    IDC_LAUNCH,
    IDC_INSTALL,
    IDC_CANCEL,
    IDC_REMOVE,
    IDC_FINISH
};

enum class Page
{
    Welcome,
    Progress,
    Done,
    UninstallConfirm,
    UninstallProgress,
    UninstallDone
};

struct App
{
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    Page page = Page::Welcome;
    int dpi = 96;
    int progress = 0;
    std::wstring status;
    std::wstring installDir;
    std::wstring lastError;
    bool installOfx = true;
    bool desktopShortcut = false;
    bool launchAfter = true;
    bool ofxInstalled = false;
    bool busy = false;
    bool silent = false;
    bool uninstallMode = false;
    bool scheduleDelete = false;

    HWND pathEdit = nullptr;
    HWND browseBtn = nullptr;
    HWND ofxSwitch = nullptr;
    HWND desktopSwitch = nullptr;
    HWND launchSwitch = nullptr;
    HWND installBtn = nullptr;
    HWND cancelBtn = nullptr;
    HWND removeBtn = nullptr;
    HWND finishBtn = nullptr;

    HFONT fontTitle = nullptr;
    HFONT fontHeading = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontButton = nullptr;
    HFONT fontSwitch = nullptr;
    HFONT fontSection = nullptr;

    HBRUSH editBrush = nullptr;

    RECT headerCard = {};
    RECT mainCard = {};
    RECT pathRect = {};
    RECT infoRect = {};
    RECT progressRect = {};
};

App g;

int Dp(int value)
{
    return MulDiv(value, g.dpi, 96);
}

Gdiplus::Color Gd(COLORREF c, BYTE alpha = 255)
{
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

std::wstring FormatError(DWORD code)
{
    wchar_t buf[96];
    swprintf(buf, 96, L"Windows error 0x%08X", (unsigned int)code);
    return buf;
}

std::wstring SelfPath()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring ProgramFilesDir()
{
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramW6432", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        n = GetEnvironmentVariableW(L"ProgramFiles", path, MAX_PATH);
    if (n == 0)
        return L"C:\\Program Files";
    return path;
}

std::wstring DefaultInstallDir()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MixedMediaStudio",
                      0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
    {
        wchar_t buf[MAX_PATH] = {};
        DWORD size = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"InstallLocation", nullptr, &type,
                             (BYTE*)buf, &size) == ERROR_SUCCESS &&
            type == REG_SZ && buf[0])
        {
            RegCloseKey(key);
            return buf;
        }
        RegCloseKey(key);
    }
    return ProgramFilesDir() + L"\\Mixed Media Studio";
}

std::wstring CommonOfxDir()
{
    return ProgramFilesDir() + L"\\Common Files\\OFX\\Plugins";
}

std::wstring OfxBundleDir()
{
    return CommonOfxDir() + L"\\MixedMedia.ofx.bundle";
}

std::wstring OfxPluginPath()
{
    return OfxBundleDir() + L"\\Contents\\Win64\\MixedMedia.ofx";
}

bool EnsureDirectory(const std::wstring& path, std::wstring* error)
{
    int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS ||
        result == ERROR_FILE_EXISTS)
        return true;
    if (error)
        *error = L"Could not create the folder:\n" + path + L"\n\n" +
                 FormatError((DWORD)result);
    return false;
}

DWORD ResourceSize(int id)
{
    HRSRC res = FindResourceW(g.instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res)
        return 0;
    return SizeofResource(g.instance, res);
}

bool WriteResourceToFile(int id, const std::wstring& path, std::wstring* error)
{
    HRSRC res = FindResourceW(g.instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res)
    {
        if (error)
            *error = L"An embedded installer file is missing.";
        return false;
    }
    DWORD size = SizeofResource(g.instance, res);
    HGLOBAL handle = LoadResource(g.instance, res);
    void* data = handle ? LockResource(handle) : nullptr;
    if (!data)
    {
        if (error)
            *error = L"Could not read an embedded installer file.";
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        if (error)
            *error = L"Could not write:\n" + path + L"\n\n" +
                     FormatError(GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(file, data, size, &written, nullptr);
    CloseHandle(file);
    if (!ok || written != size)
    {
        if (error)
            *error = L"Could not finish writing:\n" + path;
        return false;
    }
    return true;
}

bool WriteTextResource(int id, const std::wstring& path)
{
    return ResourceSize(id) > 0 && WriteResourceToFile(id, path, nullptr);
}

bool IsProcessRunning(const wchar_t* exeName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, exeName) == 0)
            {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool ResolveOrFusionRunning()
{
    return IsProcessRunning(L"Resolve.exe") || IsProcessRunning(L"Fusion.exe");
}

void ClearResolveCache()
{
    wchar_t appData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, appData)))
    {
        std::wstring cache = appData;
        cache += L"\\Blackmagic Design\\DaVinci Resolve\\Support\\OFXPluginCacheV2.xml";
        DeleteFileW(cache.c_str());
    }
}

bool CreateShortcut(const std::wstring& linkPath, const std::wstring& target,
                    const std::wstring& workDir, const std::wstring& description,
                    const std::wstring& icon)
{
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))))
        return false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetDescription(description.c_str());
    link->SetIconLocation(icon.c_str(), 0);
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file)))
        return false;
    return SUCCEEDED(file->Save(linkPath.c_str(), TRUE));
}

std::wstring KnownFolder(int csidl)
{
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT,
                                   path)))
        return path;
    return L"";
}

void CreateShortcuts()
{
    const std::wstring app = g.installDir + L"\\MixedMediaStudio.exe";
    const std::wstring uninstall = g.installDir + L"\\Uninstall.exe";
    const std::wstring programs = KnownFolder(CSIDL_COMMON_PROGRAMS);
    if (!programs.empty())
    {
        CreateShortcut(programs + L"\\Mixed Media Studio.lnk", app, g.installDir,
                       L"Mixed Media Studio", app);
        CreateShortcut(programs + L"\\Uninstall Mixed Media Studio.lnk",
                       uninstall, g.installDir,
                       L"Uninstall Mixed Media Studio", app);
    }
    if (g.desktopShortcut)
    {
        const std::wstring desktop =
            KnownFolder(CSIDL_COMMON_DESKTOPDIRECTORY);
        if (!desktop.empty())
            CreateShortcut(desktop + L"\\Mixed Media Studio.lnk", app,
                           g.installDir, L"Mixed Media Studio", app);
    }
}

void RemoveShortcuts()
{
    const wchar_t* names[] = { L"\\Mixed Media Studio.lnk",
                               L"\\Uninstall Mixed Media Studio.lnk" };
    const int folders[] = { CSIDL_COMMON_PROGRAMS, CSIDL_PROGRAMS,
                            CSIDL_COMMON_DESKTOPDIRECTORY,
                            CSIDL_DESKTOPDIRECTORY };
    for (int folder : folders)
    {
        const std::wstring dir = KnownFolder(folder);
        if (dir.empty())
            continue;
        for (const wchar_t* name : names)
            DeleteFileW((dir + name).c_str());
    }
}

void RegisterInstall(std::wstring* error)
{
    const std::wstring app = g.installDir + L"\\MixedMediaStudio.exe";
    const std::wstring uninstall = g.installDir + L"\\Uninstall.exe";
    const std::wstring keyPath =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MixedMediaStudio";

    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0,
                                     nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY,
                                     nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS)
    {
        if (error)
            *error = L"Could not register the application.\n\n" +
                     FormatError((DWORD)status);
        return;
    }

    auto setString = [&](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                       (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    };
    auto setDword = [&](const wchar_t* name, DWORD value) {
        RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value,
                       sizeof(value));
    };

    setString(L"DisplayName", kAppName);
    setString(L"DisplayVersion", kVersion);
    setString(L"Publisher", L"Mixed Media");
    setString(L"InstallLocation", g.installDir);
    setString(L"DisplayIcon", app);
    setString(L"UninstallString", L"\"" + uninstall + L"\" /uninstall");
    setString(L"QuietUninstallString",
              L"\"" + uninstall + L"\" /uninstall /silent");
    setDword(L"NoModify", 1);
    setDword(L"NoRepair", 1);
    const DWORD estimated =
        (ResourceSize(IDR_STUDIO) + ResourceSize(IDR_PLUGIN)) / 1024 + 32;
    setDword(L"EstimatedSize", estimated);
    RegCloseKey(key);

    HKEY appKey = nullptr;
    status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\MixedMediaStudio",
                             0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY,
                             nullptr, &appKey, nullptr);
    if (status == ERROR_SUCCESS)
    {
        RegSetValueExW(appKey, L"InstallDir", 0, REG_SZ,
                       (const BYTE*)g.installDir.c_str(),
                       (DWORD)((g.installDir.size() + 1) * sizeof(wchar_t)));
        DWORD ofx = g.ofxInstalled ? 1 : 0;
        RegSetValueExW(appKey, L"OfxInstalled", 0, REG_DWORD,
                       (const BYTE*)&ofx, sizeof(ofx));
        RegCloseKey(appKey);
    }
}

void RemoveRegistry()
{
    RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MixedMediaStudio");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\MixedMediaStudio");
}

bool InstallOfx(std::wstring* error)
{
    const std::wstring win64 = OfxBundleDir() + L"\\Contents\\Win64";
    if (!EnsureDirectory(win64, error))
        return false;
    return WriteResourceToFile(IDR_PLUGIN, OfxPluginPath(), error);
}

void RemoveOfx()
{
    std::wstring from = OfxBundleDir();
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_SILENT;
    SHFileOperationW(&op);
}

void SetProgress(int percent, const wchar_t* status)
{
    if (g.silent)
        return;
    g.progress = std::max(0, std::min(100, percent));
    g.status = status ? status : L"";
    if (g.hwnd)
    {
        InvalidateRect(g.hwnd, &g.mainCard, FALSE);
        UpdateWindow(g.hwnd);
    }
}

bool InstallAll(bool silent, std::wstring* error)
{
    g.busy = !silent;
    g.progress = 0;
    g.ofxInstalled = false;
    if (error)
        error->clear();

    auto fail = [&](const std::wstring& message) {
        if (error)
            *error = message;
        g.busy = false;
        return false;
    };

    if (IsProcessRunning(L"MixedMediaStudio.exe"))
        return fail(L"Close Mixed Media Studio before installing or updating it.");

    SetProgress(5, L"Creating the installation folder...");
    if (!EnsureDirectory(g.installDir, error))
    {
        g.busy = false;
        return false;
    }

    SetProgress(18, L"Installing Mixed Media Studio...");
    if (!WriteResourceToFile(IDR_STUDIO,
                             g.installDir + L"\\MixedMediaStudio.exe", error))
    {
        g.busy = false;
        return false;
    }

    SetProgress(32, L"Writing documentation...");
    WriteTextResource(IDR_README, g.installDir + L"\\README.txt");
    WriteTextResource(IDR_LICENSE, g.installDir + L"\\LICENSE");
    WriteTextResource(IDR_NOTICES, g.installDir + L"\\THIRD_PARTY_NOTICES.md");

    SetProgress(44, L"Installing the uninstaller...");
    std::wstring self = SelfPath();
    std::wstring uninstaller = g.installDir + L"\\Uninstall.exe";
    if (_wcsicmp(self.c_str(), uninstaller.c_str()) != 0)
    {
        if (!CopyFileW(self.c_str(), uninstaller.c_str(), FALSE))
            return fail(L"Could not copy the uninstaller.\n\n" +
                        FormatError(GetLastError()));
    }

    if (g.installOfx)
    {
        SetProgress(58, L"Installing the OpenFX plugin...");
        std::wstring ofxError;
        g.ofxInstalled = InstallOfx(&ofxError);
    }
    else
    {
        g.ofxInstalled = false;
    }

    SetProgress(72, L"Creating shortcuts...");
    CreateShortcuts();

    SetProgress(86, L"Registering the application...");
    RegisterInstall(error);

    SetProgress(94, L"Clearing the Resolve plugin cache...");
    ClearResolveCache();

    SetProgress(100, L"Installation complete.");
    g.busy = false;
    return true;
}

void ScheduleSelfDelete()
{
    wchar_t cmd[2048];
    swprintf(cmd, 2048,
             L"cmd.exe /c timeout /t 3 /nobreak >nul & "
             L"del /f /q \"%s\\Uninstall.exe\" >nul 2>&1 & "
             L"rmdir \"%s\" >nul 2>&1",
             g.installDir.c_str(), g.installDir.c_str());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

bool UninstallAll(bool silent, std::wstring* error)
{
    if (error)
        error->clear();

    // If the registry entry is missing, uninstall the folder that contains the
    // running Uninstall.exe rather than falling back to Program Files.
    const std::wstring self = SelfPath();
    const size_t slash = self.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
        const std::wstring selfDir = self.substr(0, slash);
        if (GetFileAttributesW(
                (selfDir + L"\\MixedMediaStudio.exe").c_str()) !=
            INVALID_FILE_ATTRIBUTES)
            g.installDir = selfDir;
    }

    if (ResolveOrFusionRunning() && !silent)
    {
        if (error)
            *error = L"Close DaVinci Resolve and Fusion before uninstalling "
                     L"Mixed Media.";
        return false;
    }
    if (IsProcessRunning(L"MixedMediaStudio.exe"))
    {
        if (error)
            *error = L"Close Mixed Media Studio before uninstalling it.";
        return false;
    }

    g.busy = !silent;
    SetProgress(10, L"Removing the OpenFX plugin...");
    RemoveOfx();

    SetProgress(30, L"Removing shortcuts...");
    RemoveShortcuts();

    SetProgress(50, L"Removing registry entries...");
    RemoveRegistry();

    SetProgress(65, L"Clearing the Resolve plugin cache...");
    ClearResolveCache();

    SetProgress(82, L"Removing application files...");
    DeleteFileW((g.installDir + L"\\MixedMediaStudio.exe").c_str());
    DeleteFileW((g.installDir + L"\\README.txt").c_str());
    DeleteFileW((g.installDir + L"\\LICENSE").c_str());
    DeleteFileW((g.installDir + L"\\THIRD_PARTY_NOTICES.md").c_str());

    SetProgress(100, L"Removal complete.");
    g.busy = false;
    g.scheduleDelete = true;
    if (silent)
        ScheduleSelfDelete();
    return true;
}

bool BrowseForFolder(HWND owner, std::wstring& path)
{
    BROWSEINFOW info{};
    info.hwndOwner = owner;
    info.lpszTitle = L"Choose the installation folder";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE |
                   BIF_USENEWUI;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&info);
    if (!item)
        return false;
    wchar_t buf[MAX_PATH] = {};
    const bool ok = SHGetPathFromIDListW(item, buf) != FALSE;
    CoTaskMemFree(item);
    if (!ok)
        return false;
    path = buf;
    return true;
}

// ---------------------------------------------------------------------------
// Drawing helpers
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

void FillRounded(Gdiplus::Graphics& gr, const Gdiplus::RectF& r, float radius,
                 const Gdiplus::Color& color)
{
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, r, radius);
    Gdiplus::SolidBrush brush(color);
    gr.FillPath(&brush, &path);
}

void StrokeRounded(Gdiplus::Graphics& gr, const Gdiplus::RectF& r, float radius,
                   const Gdiplus::Color& color, float width)
{
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, r, radius);
    Gdiplus::Pen pen(color, width);
    gr.DrawPath(&pen, &path);
}

void FillSolid(HDC dc, const RECT& rc, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
}

void DrawCard(HDC dc, const RECT& rc, int radius, COLORREF fill,
              COLORREF border)
{
    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    for (int i = Dp(9); i >= 2; i -= 2)
    {
        Gdiplus::RectF shadow((Gdiplus::REAL)rc.left - i / 2,
                              (Gdiplus::REAL)rc.top + i / 3,
                              (Gdiplus::REAL)(rc.right - rc.left + i),
                              (Gdiplus::REAL)(rc.bottom - rc.top + i));
        FillRounded(gr, shadow, (Gdiplus::REAL)radius + i / 2,
                    Gdiplus::Color(9, 44, 42, 72));
    }
    Gdiplus::RectF r((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                     (Gdiplus::REAL)(rc.right - rc.left),
                     (Gdiplus::REAL)(rc.bottom - rc.top));
    FillRounded(gr, r, (Gdiplus::REAL)radius, Gd(fill));
    StrokeRounded(gr, r, (Gdiplus::REAL)radius, Gd(border), 1.0f);
}

void DrawButton(const DRAWITEMSTRUCT* dis, const wchar_t* text, bool primary)
{
    RECT rc = dis->rcItem;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const COLORREF fill =
        primary ? (disabled ? RGB(176, 164, 220)
                            : (pressed ? kAccentDark : kAccent))
                : (pressed ? kCardAlt : kCard);
    const COLORREF border = primary ? fill : kLine;
    const COLORREF textColor = primary ? RGB(255, 255, 255) : kInk;

    Gdiplus::Graphics gr((HDC)dis->hDC);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    if (primary && !pressed && !disabled)
    {
        Gdiplus::RectF shadow((Gdiplus::REAL)rc.left,
                              (Gdiplus::REAL)rc.top + Dp(2),
                              (Gdiplus::REAL)(rc.right - rc.left),
                              (Gdiplus::REAL)(rc.bottom - rc.top));
        FillRounded(gr, shadow, (Gdiplus::REAL)Dp(12),
                    Gdiplus::Color(36, 124, 92, 255));
    }
    if (pressed)
        OffsetRect(&rc, 0, Dp(1));
    Gdiplus::RectF r((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                     (Gdiplus::REAL)(rc.right - rc.left),
                     (Gdiplus::REAL)(rc.bottom - rc.top));
    FillRounded(gr, r, (Gdiplus::REAL)Dp(12), Gd(fill));
    StrokeRounded(gr, r, (Gdiplus::REAL)Dp(12), Gd(border), 1.0f);
    gr.Flush();

    SetBkMode((HDC)dis->hDC, TRANSPARENT);
    SelectObject((HDC)dis->hDC, g.fontButton);
    SetTextColor((HDC)dis->hDC, textColor);
    DrawTextW((HDC)dis->hDC, text, -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawSwitch(const DRAWITEMSTRUCT* dis, const wchar_t* text, bool on,
                bool darkBackground)
{
    RECT rc = dis->rcItem;
    FillSolid((HDC)dis->hDC, rc, darkBackground ? kDark : kCard);
    SetBkMode((HDC)dis->hDC, TRANSPARENT);
    SelectObject((HDC)dis->hDC, g.fontSwitch);
    SetTextColor((HDC)dis->hDC,
                 darkBackground ? RGB(232, 230, 245) : kInk);
    RECT textRect = rc;
    textRect.right -= Dp(50);
    DrawTextW((HDC)dis->hDC, text, -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const int sw = Dp(38);
    const int sh = Dp(20);
    RECT sr = { rc.right - sw - Dp(2),
                rc.top + (rc.bottom - rc.top - sh) / 2,
                rc.right - Dp(2),
                rc.top + (rc.bottom - rc.top + sh) / 2 };
    Gdiplus::Graphics gr((HDC)dis->hDC);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF track((Gdiplus::REAL)sr.left, (Gdiplus::REAL)sr.top,
                         (Gdiplus::REAL)(sr.right - sr.left),
                         (Gdiplus::REAL)(sr.bottom - sr.top));
    const Gdiplus::Color trackColor =
        on ? Gd(kAccent)
           : Gdiplus::Color(255, darkBackground ? 62 : 214,
                            darkBackground ? 58 : 220,
                            darkBackground ? 92 : 234);
    FillRounded(gr, track, (Gdiplus::REAL)sh / 2.0f, trackColor);
    const float thumbR = (float)Dp(8);
    const float thumbX = on ? (float)sr.right - thumbR - 1.0f
                            : (float)sr.left + thumbR + 1.0f;
    const float thumbY = (float)sr.top + (float)sh / 2.0f;
    Gdiplus::SolidBrush shadow(Gdiplus::Color(45, 20, 15, 40));
    gr.FillEllipse(&shadow, thumbX - thumbR + 1, thumbY - thumbR + 2,
                   thumbR * 2, thumbR * 2);
    Gdiplus::SolidBrush thumb(Gdiplus::Color(255, 255, 255, 255));
    gr.FillEllipse(&thumb, thumbX - thumbR, thumbY - thumbR, thumbR * 2,
                   thumbR * 2);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

HFONT MakeFont(double points, int weight)
{
    return CreateFontW(-MulDiv((int)std::lround(points * 10.0), g.dpi, 720),
                       0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void CreateFonts()
{
    g.fontTitle = MakeFont(20.0, FW_SEMIBOLD);
    g.fontHeading = MakeFont(15.0, FW_SEMIBOLD);
    g.fontBody = MakeFont(9.5, FW_NORMAL);
    g.fontSmall = MakeFont(8.0, FW_NORMAL);
    g.fontButton = MakeFont(9.5, FW_SEMIBOLD);
    g.fontSwitch = MakeFont(9.0, FW_NORMAL);
    g.fontSection = MakeFont(7.5, FW_SEMIBOLD);
}

void ApplyFonts()
{
    SendMessageW(g.pathEdit, WM_SETFONT, (WPARAM)g.fontBody, TRUE);
}

HWND CreateSwitch(HWND parent, const wchar_t* text, int id)
{
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10,
                             10, parent, (HMENU)(INT_PTR)id, g.instance, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g.fontSwitch, TRUE);
    return h;
}

HWND CreateButton(HWND parent, const wchar_t* text, int id)
{
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10,
                             10, parent, (HMENU)(INT_PTR)id, g.instance, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g.fontButton, TRUE);
    return h;
}

void CreateUi(HWND hwnd)
{
    g.pathEdit = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_PATH, g.instance, nullptr);
    SendMessageW(g.pathEdit, WM_SETFONT, (WPARAM)g.fontBody, TRUE);

    g.browseBtn = CreateButton(hwnd, L"Browse", IDC_BROWSE);
    g.ofxSwitch = CreateSwitch(hwnd, L"Install the OpenFX plugin for Resolve / Fusion",
                               IDC_OFX);
    g.desktopSwitch = CreateSwitch(hwnd, L"Create a desktop shortcut",
                                   IDC_DESKTOP);
    g.launchSwitch = CreateSwitch(hwnd, L"Launch Mixed Media Studio", IDC_LAUNCH);
    g.installBtn = CreateButton(hwnd, L"Install", IDC_INSTALL);
    g.cancelBtn = CreateButton(hwnd, L"Cancel", IDC_CANCEL);
    g.removeBtn = CreateButton(hwnd, L"Remove", IDC_REMOVE);
    g.finishBtn = CreateButton(hwnd, L"Finish", IDC_FINISH);

    SetWindowTextW(g.pathEdit, g.installDir.c_str());
    g.editBrush = CreateSolidBrush(RGB(255, 255, 255));
    ApplyFonts();
}

void LayoutUi()
{
    RECT client;
    GetClientRect(g.hwnd, &client);
    const int w = client.right;
    const int h = client.bottom;

    g.headerCard = { Dp(16), Dp(14), w - Dp(16), Dp(96) };
    g.mainCard = { Dp(16), Dp(110), w - Dp(16), h - Dp(16) };

    const int x = g.mainCard.left + Dp(26);
    const int right = g.mainCard.right - Dp(26);
    const int cardTop = g.mainCard.top;
    g.pathRect = { x, cardTop + Dp(120), right - Dp(100),
                   cardTop + Dp(152) };
    g.infoRect = { x, cardTop + Dp(258), right, cardTop + Dp(360) };
    g.progressRect = { x, cardTop + Dp(126), right, cardTop + Dp(138) };

    MoveWindow(g.pathEdit, g.pathRect.left + Dp(12), g.pathRect.top + Dp(5),
               g.pathRect.right - g.pathRect.left - Dp(24), Dp(22), TRUE);
    MoveWindow(g.browseBtn, right - Dp(92), g.pathRect.top, Dp(92),
               g.pathRect.bottom - g.pathRect.top, TRUE);
    MoveWindow(g.ofxSwitch, x, cardTop + Dp(176), right - x, Dp(30), TRUE);
    MoveWindow(g.desktopSwitch, x, cardTop + Dp(212), right - x, Dp(30), TRUE);
    MoveWindow(g.launchSwitch, x, cardTop + Dp(190), right - x, Dp(30), TRUE);

    const int buttonY = g.mainCard.bottom - Dp(58);
    MoveWindow(g.installBtn, right - Dp(150), buttonY, Dp(150), Dp(42), TRUE);
    MoveWindow(g.cancelBtn, right - Dp(150) - Dp(108), buttonY, Dp(100),
               Dp(42), TRUE);
    MoveWindow(g.removeBtn, right - Dp(150), buttonY, Dp(150), Dp(42), TRUE);
    MoveWindow(g.finishBtn, right - Dp(150), buttonY, Dp(150), Dp(42), TRUE);
}

void ShowPage(Page page)
{
    g.page = page;
    const bool welcome = page == Page::Welcome;
    const bool done = page == Page::Done;
    const bool uninstallConfirm = page == Page::UninstallConfirm;
    const bool uninstallDone = page == Page::UninstallDone;

    ShowWindow(g.pathEdit, welcome ? SW_SHOW : SW_HIDE);
    ShowWindow(g.browseBtn, welcome ? SW_SHOW : SW_HIDE);
    ShowWindow(g.ofxSwitch, welcome ? SW_SHOW : SW_HIDE);
    ShowWindow(g.desktopSwitch, welcome ? SW_SHOW : SW_HIDE);
    ShowWindow(g.launchSwitch, done ? SW_SHOW : SW_HIDE);
    ShowWindow(g.installBtn, welcome ? SW_SHOW : SW_HIDE);
    ShowWindow(g.cancelBtn, (welcome || uninstallConfirm) ? SW_SHOW : SW_HIDE);
    ShowWindow(g.removeBtn, uninstallConfirm ? SW_SHOW : SW_HIDE);
    ShowWindow(g.finishBtn, (done || uninstallDone) ? SW_SHOW : SW_HIDE);

    SetWindowTextW(g.finishBtn, uninstallDone ? L"Close" : L"Finish");
    LayoutUi();
    InvalidateRect(g.hwnd, &g.mainCard, FALSE);
}

void DrawHeader(HDC dc)
{
    DrawCard(dc, g.headerCard, Dp(22), kCard, kLine);
    HICON icon = (HICON)LoadImageW(g.instance, MAKEINTRESOURCEW(IDI_APP),
                                   IMAGE_ICON, Dp(48), Dp(48),
                                   LR_DEFAULTCOLOR);
    if (icon)
    {
        DrawIconEx(dc, g.headerCard.left + Dp(20),
                   g.headerCard.top + Dp(16), icon, Dp(48), Dp(48), 0, nullptr,
                   DI_NORMAL);
        DestroyIcon(icon);
    }

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g.fontTitle);
    SetTextColor(dc, kInk);
    RECT title = { g.headerCard.left + Dp(82),
                   g.headerCard.top + Dp(10), g.headerCard.right - Dp(20),
                   g.headerCard.top + Dp(46) };
    DrawTextW(dc, L"Mixed Media Studio", -1, &title,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SelectObject(dc, g.fontBody);
    SetTextColor(dc, kMuted);
    RECT sub = { g.headerCard.left + Dp(84),
                 g.headerCard.top + Dp(43), g.headerCard.right - Dp(20),
                 g.headerCard.top + Dp(66) };
    DrawTextW(dc, L"Setup  \u00B7  Paper Cutout + Halftone Screen", -1, &sub,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    RECT accent = { g.headerCard.left + Dp(84),
                    g.headerCard.bottom - Dp(11),
                    g.headerCard.left + Dp(196),
                    g.headerCard.bottom - Dp(8) };
    FillRounded(gr, Gdiplus::RectF((Gdiplus::REAL)accent.left,
                                   (Gdiplus::REAL)accent.top,
                                   (Gdiplus::REAL)(accent.right - accent.left),
                                   (Gdiplus::REAL)(accent.bottom - accent.top)),
                (Gdiplus::REAL)Dp(2), Gd(kAccent));
}

void DrawInfoCard(HDC dc)
{
    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF r((Gdiplus::REAL)g.infoRect.left,
                     (Gdiplus::REAL)g.infoRect.top,
                     (Gdiplus::REAL)(g.infoRect.right - g.infoRect.left),
                     (Gdiplus::REAL)(g.infoRect.bottom - g.infoRect.top));
    FillRounded(gr, r, (Gdiplus::REAL)Dp(14), Gd(kAccentSoft));
    Gdiplus::RectF badge((Gdiplus::REAL)(g.infoRect.left + Dp(14)),
                         (Gdiplus::REAL)(g.infoRect.top + Dp(14)),
                         (Gdiplus::REAL)Dp(30), (Gdiplus::REAL)Dp(30));
    FillRounded(gr, badge, (Gdiplus::REAL)Dp(9), Gd(kAccent));
    gr.Flush();

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g.fontButton);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT badgeText = { (int)badge.X, (int)badge.Y, (int)(badge.X + badge.Width),
                       (int)(badge.Y + badge.Height) };
    DrawTextW(dc, L"i", -1, &badgeText,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, g.fontSection);
    SetTextColor(dc, kAccentDark);
    RECT heading = { g.infoRect.left + Dp(56), g.infoRect.top + Dp(12),
                     g.infoRect.right - Dp(14), g.infoRect.top + Dp(30) };
    DrawTextW(dc, L"WORKS WITH YOUR EDITOR", -1, &heading,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SelectObject(dc, g.fontSmall);
    SetTextColor(dc, kInk);
    RECT body = { g.infoRect.left + Dp(56), g.infoRect.top + Dp(32),
                  g.infoRect.right - Dp(16), g.infoRect.bottom - Dp(10) };
    DrawTextW(dc,
              L"DaVinci Resolve and Fusion load the OpenFX plugin directly. "
              L"After Effects, Premiere Pro and Filmora do not support OpenFX "
              L"- use Mixed Media Studio to export PNGs and import them into "
              L"your timeline.",
              -1, &body, DT_LEFT | DT_WORDBREAK);
}

void DrawProgress(HDC dc)
{
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g.fontHeading);
    SetTextColor(dc, kInk);
    RECT title = { g.mainCard.left + Dp(26), g.mainCard.top + Dp(20),
                   g.mainCard.right - Dp(26), g.mainCard.top + Dp(50) };
    DrawTextW(dc, g.page == Page::UninstallProgress
                      ? L"Removing Mixed Media Studio"
                      : L"Installing Mixed Media Studio",
              -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SelectObject(dc, g.fontBody);
    SetTextColor(dc, kMuted);
    RECT status = { g.mainCard.left + Dp(26), g.mainCard.top + Dp(64),
                    g.mainCard.right - Dp(26), g.mainCard.top + Dp(104) };
    DrawTextW(dc, g.status.c_str(), -1, &status, DT_LEFT | DT_WORDBREAK);

    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF track((Gdiplus::REAL)g.progressRect.left,
                         (Gdiplus::REAL)g.progressRect.top,
                         (Gdiplus::REAL)(g.progressRect.right -
                                         g.progressRect.left),
                         (Gdiplus::REAL)(g.progressRect.bottom -
                                         g.progressRect.top));
    FillRounded(gr, track, (Gdiplus::REAL)Dp(6), Gd(kCardAlt));
    if (g.progress > 0)
    {
        Gdiplus::RectF fill((Gdiplus::REAL)g.progressRect.left,
                            (Gdiplus::REAL)g.progressRect.top,
                            (Gdiplus::REAL)((g.progressRect.right -
                                             g.progressRect.left) *
                                            g.progress / 100),
                            (Gdiplus::REAL)(g.progressRect.bottom -
                                            g.progressRect.top));
        FillRounded(gr, fill, (Gdiplus::REAL)Dp(6), Gd(kAccent));
    }

    SelectObject(dc, g.fontSmall);
    SetTextColor(dc, kMuted);
    wchar_t percent[32];
    swprintf(percent, 32, L"%d%%", g.progress);
    RECT pr = { g.progressRect.left, g.progressRect.bottom + Dp(6),
                g.progressRect.right, g.progressRect.bottom + Dp(26) };
    DrawTextW(dc, percent, -1, &pr, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    // Step checklist.
    struct Step
    {
        const wchar_t* text;
        int at;
    };
    Step steps[] = {
        { L"Studio application", 34 },
        { L"OpenFX plugin for Resolve / Fusion", 58 },
        { L"Shortcuts and uninstaller", 72 },
        { L"Registry and Resolve cache", 94 }
    };
    int y = g.mainCard.top + Dp(190);
    for (const Step& step : steps)
    {
        const bool complete = g.progress >= step.at;
        RECT dot = { g.mainCard.left + Dp(26), y + Dp(4),
                     g.mainCard.left + Dp(38), y + Dp(16) };
        Gdiplus::Graphics gd(dc);
        gd.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::RectF dr((Gdiplus::REAL)dot.left, (Gdiplus::REAL)dot.top,
                          (Gdiplus::REAL)(dot.right - dot.left),
                          (Gdiplus::REAL)(dot.bottom - dot.top));
        const Gdiplus::Color dotColor =
            complete ? Gdiplus::Color(255, 64, 196, 140)
                     : Gdiplus::Color(255, 220, 224, 234);
        FillRounded(gd, dr, (Gdiplus::REAL)Dp(6), dotColor);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g.fontSmall);
        SetTextColor(dc, complete ? kInk : kMuted);
        RECT tr = { g.mainCard.left + Dp(48), y,
                    g.mainCard.right - Dp(26), y + Dp(22) };
        DrawTextW(dc, step.text, -1, &tr,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        y += Dp(30);
    }
}

void PaintUi(HDC dc, int w, int h)
{
    {
        Gdiplus::Graphics bg(dc);
        Gdiplus::LinearGradientBrush gradient(
            Gdiplus::RectF(0, 0, (Gdiplus::REAL)w, (Gdiplus::REAL)h),
            Gd(kBgTop), Gd(kBgBottom), Gdiplus::LinearGradientModeVertical);
        bg.FillRectangle(&gradient, 0, 0, w, h);
    }
    DrawHeader(dc);
    DrawCard(dc, g.mainCard, Dp(24), kCard, kLine);

    SetBkMode(dc, TRANSPARENT);
    if (g.page == Page::Welcome)
    {
        SelectObject(dc, g.fontHeading);
        SetTextColor(dc, kInk);
        RECT title = { g.mainCard.left + Dp(26), g.mainCard.top + Dp(20),
                       g.mainCard.right - Dp(26), g.mainCard.top + Dp(50) };
        DrawTextW(dc, L"Install Mixed Media Studio", -1, &title,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        SelectObject(dc, g.fontBody);
        SetTextColor(dc, kMuted);
        RECT body = { g.mainCard.left + Dp(26), g.mainCard.top + Dp(54),
                      g.mainCard.right - Dp(26), g.mainCard.top + Dp(92) };
        DrawTextW(dc,
                  L"The standalone studio for Paper Cutout and Halftone "
                  L"Screen, plus the native OpenFX plugin for DaVinci Resolve "
                  L"and Fusion.",
                  -1, &body, DT_LEFT | DT_WORDBREAK);

        SelectObject(dc, g.fontSection);
        SetTextColor(dc, kMuted);
        RECT label = { g.mainCard.left + Dp(26), g.mainCard.top + Dp(100),
                       g.mainCard.right - Dp(26), g.mainCard.top + Dp(116) };
        DrawTextW(dc, L"INSTALL LOCATION", -1, &label,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        // Rounded field behind the edit control.
        Gdiplus::Graphics gr(dc);
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::RectF field((Gdiplus::REAL)g.pathRect.left,
                             (Gdiplus::REAL)g.pathRect.top,
                             (Gdiplus::REAL)(g.pathRect.right -
                                             g.pathRect.left),
                             (Gdiplus::REAL)(g.pathRect.bottom -
                                             g.pathRect.top));
        FillRounded(gr, field, (Gdiplus::REAL)Dp(10),
                    Gdiplus::Color(255, 255, 255, 255));
        StrokeRounded(gr, field, (Gdiplus::REAL)Dp(10), Gd(kLine), 1.0f);
        gr.Flush();

        DrawInfoCard(dc);

        SelectObject(dc, g.fontSmall);
        SetTextColor(dc, kMuted);
        const double mb =
            (double)(ResourceSize(IDR_STUDIO) + ResourceSize(IDR_PLUGIN)) /
            (1024.0 * 1024.0);
        wchar_t footer[160];
        swprintf(footer, 160,
                 L"Approx. %.1f MB  \u00B7  Windows 10/11 x64  \u00B7  MIT licensed",
                 mb);
        RECT foot = { g.mainCard.left + Dp(26),
                      g.mainCard.bottom - Dp(82),
                      g.mainCard.right - Dp(188),
                      g.mainCard.bottom - Dp(62) };
        DrawTextW(dc, footer, -1, &foot,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    else if (g.page == Page::Progress ||
             g.page == Page::UninstallProgress)
    {
        DrawProgress(dc);
    }
    else if (g.page == Page::Done)
    {
        SelectObject(dc, g.fontHeading);
        SetTextColor(dc, kInk);
        RECT title = { g.mainCard.left + Dp(26), g.mainCard.top + Dp(20),
                       g.mainCard.right - Dp(26), g.mainCard.top + Dp(50) };
        DrawTextW(dc, L"Installation complete", -1, &title,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        SelectObject(dc, g.fontBody);
        SetTextColor(dc, kMuted);
        RECT body = { g.mainCard.left + Dp(26), g.mainCard.top + Dp(56),
                      g.mainCard.right - Dp(26), g.mainCard.top + Dp(96) };
        DrawTextW(dc,
                  g.ofxInstalled
                      ? L"Mixed Media Studio is installed, and the OpenFX "
                        L"plugin is ready for DaVinci Resolve and Fusion."
                      : L"Mixed Media Studio is installed. The OpenFX plugin "
                        L"was skipped or could not be copied.",
                  -1, &body, DT_LEFT | DT_WORDBREAK);

        SelectObject(dc, g.fontSection);
        SetTextColor(dc, kMuted);
        RECT appLabel = { g.mainCard.left + Dp(26),
                          g.mainCard.top + Dp(108),
                          g.mainCard.right - Dp(26),
                          g.mainCard.top + Dp(124) };
        DrawTextW(dc, L"APPLICATION", -1, &appLabel,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, g.fontSmall);
        SetTextColor(dc, kInk);
        RECT appValue = { g.mainCard.left + Dp(26),
                          g.mainCard.top + Dp(126),
                          g.mainCard.right - Dp(26),
                          g.mainCard.top + Dp(146) };
        DrawTextW(dc, (g.installDir + L"\\MixedMediaStudio.exe").c_str(), -1,
                  &appValue, DT_LEFT | DT_SINGLELINE | DT_VCENTER |
                                 DT_END_ELLIPSIS);

        SelectObject(dc, g.fontSection);
        SetTextColor(dc, kMuted);
        RECT ofxLabel = { g.mainCard.left + Dp(26),
                          g.mainCard.top + Dp(156),
                          g.mainCard.right - Dp(26),
                          g.mainCard.top + Dp(172) };
        DrawTextW(dc, L"OPENFX PLUGIN", -1, &ofxLabel,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, g.fontSmall);
        SetTextColor(dc, g.ofxInstalled ? RGB(47, 128, 90) : kMuted);
        RECT ofxValue = { g.mainCard.left + Dp(26),
                          g.mainCard.top + Dp(174),
                          g.mainCard.right - Dp(26),
                          g.mainCard.top + Dp(194) };
        DrawTextW(dc,
                  g.ofxInstalled
                      ? L"Installed for DaVinci Resolve / Fusion. Restart "
                        L"Resolve to load it."
                      : L"Not installed. Run Setup again with Resolve closed "
                        L"to add it.",
                  -1, &ofxValue,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    else if (g.page == Page::UninstallConfirm)
    {
        SelectObject(dc, g.fontHeading);
        SetTextColor(dc, kInk);
        RECT title = { g.mainCard.left + Dp(26), g.mainCard.top + Dp(20),
                       g.mainCard.right - Dp(26), g.mainCard.top + Dp(50) };
        DrawTextW(dc, L"Remove Mixed Media Studio?", -1, &title,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        SelectObject(dc, g.fontBody);
        SetTextColor(dc, kMuted);
        RECT body = { g.mainCard.left + Dp(26), g.mainCard.top + Dp(60),
                      g.mainCard.right - Dp(26), g.mainCard.top + Dp(140) };
        DrawTextW(dc,
                  L"This removes the Studio app, shortcuts, and the "
                  L"Resolve / Fusion OpenFX plugin. Exported PNGs and project "
                  L"files are not touched.",
                  -1, &body, DT_LEFT | DT_WORDBREAK);
    }
    else if (g.page == Page::UninstallDone)
    {
        SelectObject(dc, g.fontHeading);
        SetTextColor(dc, kInk);
        RECT title = { g.mainCard.left + Dp(26), g.mainCard.top + Dp(20),
                       g.mainCard.right - Dp(26), g.mainCard.top + Dp(50) };
        DrawTextW(dc, L"Mixed Media Studio has been removed.", -1, &title,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        SelectObject(dc, g.fontBody);
        SetTextColor(dc, kMuted);
        RECT body = { g.mainCard.left + Dp(26), g.mainCard.top + Dp(60),
                      g.mainCard.right - Dp(26), g.mainCard.top + Dp(100) };
        DrawTextW(dc, L"You can close this window.", -1, &body,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
}

void OnInstall()
{
    wchar_t buffer[1024] = {};
    GetWindowTextW(g.pathEdit, buffer, 1024);
    std::wstring dir = buffer;
    while (!dir.empty() &&
           (dir.back() == L' ' || dir.back() == L'\t' || dir.back() == L'\\'))
        dir.pop_back();
    if (dir.empty())
    {
        MessageBoxW(g.hwnd, L"Choose an installation folder.", kSetupName,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (PathIsRelativeW(dir.c_str()))
    {
        MessageBoxW(g.hwnd, L"Use a full path for the installation folder.",
                    kSetupName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    const DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        MessageBoxW(g.hwnd, L"That path is a file, not a folder.", kSetupName,
                    MB_OK | MB_ICONERROR);
        return;
    }

    g.installDir = dir;
    if (g.installOfx && ResolveOrFusionRunning())
    {
        if (MessageBoxW(g.hwnd,
                        L"DaVinci Resolve or Fusion is running. Setup can copy "
                        L"the OpenFX plugin, but the application must be "
                        L"restarted before it appears.\n\nContinue?",
                        kSetupName, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    ShowPage(Page::Progress);
    std::wstring error;
    if (!InstallAll(false, &error))
    {
        ShowPage(Page::Welcome);
        MessageBoxW(g.hwnd,
                    (L"Setup could not finish.\n\n" + error).c_str(),
                    kSetupName, MB_OK | MB_ICONERROR);
        return;
    }
    ShowPage(Page::Done);
}

void OnRemove()
{
    ShowPage(Page::UninstallProgress);
    std::wstring error;
    if (!UninstallAll(false, &error))
    {
        ShowPage(Page::UninstallConfirm);
        MessageBoxW(g.hwnd, error.c_str(), kSetupName, MB_OK | MB_ICONERROR);
        return;
    }
    ShowPage(Page::UninstallDone);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        g.hwnd = hwnd;
        g.installDir = DefaultInstallDir();
        CreateFonts();
        CreateUi(hwnd);
        LayoutUi();
        ShowPage(g.uninstallMode ? Page::UninstallConfirm : Page::Welcome);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintUi(mem, client.right, client.bottom);
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT:
    {
        HDC dc = (HDC)wParam;
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, kInk);
        return (LRESULT)g.editBrush;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
        if (dis->CtlType != ODT_BUTTON)
            return FALSE;
        switch (dis->CtlID)
        {
        case IDC_BROWSE:
            DrawButton(dis, L"Browse", false);
            return TRUE;
        case IDC_INSTALL:
            DrawButton(dis, L"Install", true);
            return TRUE;
        case IDC_CANCEL:
            DrawButton(dis, L"Cancel", false);
            return TRUE;
        case IDC_REMOVE:
            DrawButton(dis, L"Remove", true);
            return TRUE;
        case IDC_FINISH:
            DrawButton(dis, g.page == Page::UninstallDone ? L"Close" : L"Finish",
                       true);
            return TRUE;
        case IDC_OFX:
            DrawSwitch(dis, L"Install the OpenFX plugin for Resolve / Fusion",
                       g.installOfx, false);
            return TRUE;
        case IDC_DESKTOP:
            DrawSwitch(dis, L"Create a desktop shortcut",
                       g.desktopShortcut, false);
            return TRUE;
        case IDC_LAUNCH:
            DrawSwitch(dis, L"Launch Mixed Media Studio", g.launchAfter,
                       false);
            return TRUE;
        default:
            return FALSE;
        }
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (code != BN_CLICKED)
            break;
        if (g.busy)
            return 0;
        switch (id)
        {
        case IDC_BROWSE:
        {
            std::wstring path;
            if (BrowseForFolder(hwnd, path))
                SetWindowTextW(g.pathEdit, path.c_str());
            return 0;
        }
        case IDC_OFX:
            g.installOfx = !g.installOfx;
            InvalidateRect(g.ofxSwitch, nullptr, FALSE);
            return 0;
        case IDC_DESKTOP:
            g.desktopShortcut = !g.desktopShortcut;
            InvalidateRect(g.desktopSwitch, nullptr, FALSE);
            return 0;
        case IDC_LAUNCH:
            g.launchAfter = !g.launchAfter;
            InvalidateRect(g.launchSwitch, nullptr, FALSE);
            return 0;
        case IDC_INSTALL:
            OnInstall();
            return 0;
        case IDC_REMOVE:
            OnRemove();
            return 0;
        case IDC_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        case IDC_FINISH:
            if (g.page == Page::Done && g.launchAfter)
            {
                const std::wstring app =
                    g.installDir + L"\\MixedMediaStudio.exe";
                ShellExecuteW(nullptr, L"open", app.c_str(), nullptr,
                              g.installDir.c_str(), SW_SHOWNORMAL);
            }
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        return 0;
    }

    case WM_CLOSE:
        if (g.busy)
            return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g.scheduleDelete)
            ScheduleSelfDelete();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g.instance = instance;
    g.dpi = GetDpiForSystem();
    g.installDir = DefaultInstallDir();

    bool silent = false;
    bool uninstall = false;
    bool noOfx = false;
    bool noDesktop = false;
    bool noLaunch = false;
    std::wstring customDir;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            std::wstring arg = argv[i];
            std::wstring lower = arg;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](wchar_t c) { return (wchar_t)towlower(c); });
            if (lower == L"/s" || lower == L"/silent" || lower == L"-s")
                silent = true;
            else if (lower == L"/uninstall" || lower == L"-uninstall")
                uninstall = true;
            else if (lower == L"/noofx")
                noOfx = true;
            else if (lower == L"/nodesktop")
                noDesktop = true;
            else if (lower == L"/nolaunch")
                noLaunch = true;
            else if (lower.rfind(L"/d=", 0) == 0)
                customDir = arg.substr(3);
        }
        LocalFree(argv);
    }

    if (silent)
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        g.silent = true;
        g.uninstallMode = uninstall;
        g.installOfx = !noOfx;
        g.desktopShortcut = !noDesktop;
        g.launchAfter = !noLaunch;
        if (!customDir.empty())
            g.installDir = customDir;
        std::wstring error;
        int code = 1;
        if (uninstall)
            code = UninstallAll(true, &error) ? 0 : 1;
        else
            code = InstallAll(true, &error) ? 0 : 1;
        CoUninitialize();
        return code;
    }

    OleInitialize(nullptr);
    Gdiplus::GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, nullptr) != Gdiplus::Ok)
        return 1;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    g.uninstallMode = uninstall;
    g.installOfx = !noOfx;
    if (!customDir.empty())
        g.installDir = customDir;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc))
        return 1;

    const int width = Dp(760);
    const int height = Dp(620);
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const wchar_t* windowTitle =
        uninstall ? L"Mixed Media Studio Uninstall" : kSetupName;
    HWND hwnd = CreateWindowExW(
        WS_EX_CONTROLPARENT, kWindowClass, windowTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        (screenW - width) / 2, (screenH - height) / 2, width, height, nullptr,
        nullptr, instance, nullptr);
    if (!hwnd)
        return 1;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g.editBrush)
        DeleteObject(g.editBrush);
    HFONT fonts[] = { g.fontTitle, g.fontHeading, g.fontBody, g.fontSmall,
                      g.fontButton, g.fontSwitch, g.fontSection };
    for (HFONT font : fonts)
        if (font)
            DeleteObject(font);
    Gdiplus::GdiplusShutdown(gdiToken);
    OleUninitialize();
    return (int)msg.wParam;
}
