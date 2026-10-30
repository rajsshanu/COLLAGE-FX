#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace mm {
namespace {

std::once_flag gOnce;
std::wstring gPath;

void initPath()
{
    wchar_t local[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    std::wstring dir;
    if (n > 0 && n < MAX_PATH)
        dir.assign(local, n);
    else
        dir = L".";
    dir += L"\\MixedMediaOFX";
    CreateDirectoryW(dir.c_str(), nullptr);
    gPath = dir + L"\\mixedmedia.log";
}

} // namespace

void logMessage(const char* format, ...)
{
    std::call_once(gOnce, initPath);
    if (gPath.empty() || !format)
        return;

    char body[2048] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(body, sizeof(body), format, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[2304] = {};
    std::snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u  %s\r\n",
                  (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                  (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                  (unsigned)st.wMilliseconds, body);

    // A process-level named mutex avoids interleaved lines without adding a
    // static initialisation-order dependency.
    static HANDLE fileMutex = CreateMutexW(nullptr, FALSE, L"Local\\MixedMediaOFX_Log");
    if (fileMutex)
        WaitForSingleObject(fileMutex, 50);

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, gPath.c_str(), L"ab") == 0 && fp)
    {
        std::fwrite(line, 1, std::strlen(line), fp);
        std::fclose(fp);
    }
    if (fileMutex)
        ReleaseMutex(fileMutex);
}

} // namespace mm
