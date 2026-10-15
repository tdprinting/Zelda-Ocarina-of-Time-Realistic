#include "Updater.h"

#include <atomic>
#include <mutex>
#include <thread>

#include <ship/Context.h>
#include <ship/window/Window.h>

#ifndef ZOOT_BUILD_NUMBER
#define ZOOT_BUILD_NUMBER 0
#endif

#define UPDATER_REPO "tdprinting/Zelda-Ocarina-of-Time-Realistic"
#define UPDATER_ASSET_NAME "zoot-realistic-windows-x64.zip"

namespace Updater {

static std::mutex sStatusMutex;
static Status sStatus;
static std::atomic<bool> sBusy = false;
static std::string sDownloadUrl;

static void SetStatus(State state, const std::string& message, float progress = 0.0f) {
    std::lock_guard<std::mutex> lock(sStatusMutex);
    sStatus.state = state;
    sStatus.message = message;
    sStatus.progress = progress;
}

uint32_t CurrentBuild() {
    return ZOOT_BUILD_NUMBER;
}

Status GetStatus() {
    std::lock_guard<std::mutex> lock(sStatusMutex);
    return sStatus;
}

} // namespace Updater

#ifdef _WIN32

#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace Updater {

static std::wstring Widen(const std::string& s) {
    if (s.empty()) {
        return L"";
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

// GETs url and either returns the body in outBody or streams it to outFile.
static bool HttpGet(const std::string& url, std::string* outBody, const std::filesystem::path* outFile,
                    const std::function<void(uint64_t, uint64_t)>& onProgress, std::string& error) {
    std::wstring wurl = Widen(url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        error = "Bad URL";
        return false;
    }

    HINTERNET session = WinHttpOpen(L"ZeldaOoTRealistic-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    auto cleanup = [&]() {
        if (request) {
            WinHttpCloseHandle(request);
        }
        if (connect) {
            WinHttpCloseHandle(connect);
        }
        if (session) {
            WinHttpCloseHandle(session);
        }
    };

    const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
    if (!request || !WinHttpSendRequest(request, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        error = "Could not reach GitHub (error " + std::to_string(GetLastError()) + ")";
        cleanup();
        return false;
    }

    DWORD statusCode = 0;
    DWORD size = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200) {
        error = statusCode == 404 ? "No release has been published yet"
                                  : "GitHub returned HTTP " + std::to_string(statusCode);
        cleanup();
        return false;
    }

    DWORD contentLength = 0;
    size = sizeof(contentLength);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &contentLength, &size, WINHTTP_NO_HEADER_INDEX);

    std::ofstream file;
    if (outFile != nullptr) {
        file.open(*outFile, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "Could not write the download";
            cleanup();
            return false;
        }
    }

    uint64_t received = 0;
    std::string buffer;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            error = "Download interrupted";
            cleanup();
            return false;
        }
        if (available == 0) {
            break;
        }
        buffer.resize(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read)) {
            error = "Download interrupted";
            cleanup();
            return false;
        }
        if (outFile != nullptr) {
            file.write(buffer.data(), read);
        } else {
            outBody->append(buffer.data(), read);
        }
        received += read;
        if (onProgress) {
            onProgress(received, contentLength);
        }
    }

    cleanup();
    return true;
}

static std::filesystem::path DownloadPath() {
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    return std::filesystem::path(temp) / L"zoot-realistic-update.zip";
}

static std::filesystem::path ExecutablePath() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe);
}

bool IsSupported() {
    return true;
}

void CheckForUpdate() {
    if (sBusy.exchange(true)) {
        return;
    }
    SetStatus(State::Checking, "Checking GitHub for the newest build...");
    std::thread([]() {
        std::string body;
        std::string error;
        if (!HttpGet("https://api.github.com/repos/" UPDATER_REPO "/releases/latest", &body, nullptr, nullptr, error)) {
            SetStatus(State::Error, error);
            sBusy = false;
            return;
        }

        auto json = nlohmann::json::parse(body, nullptr, false);
        if (json.is_discarded() || !json.contains("tag_name")) {
            SetStatus(State::Error, "Could not read the release info from GitHub");
            sBusy = false;
            return;
        }

        // Releases are tagged build-<CI run number>.
        std::string tag = json["tag_name"].get<std::string>();
        uint32_t latest = 0;
        size_t dash = tag.find_last_of('-');
        try {
            latest = (uint32_t)std::stoul(dash == std::string::npos ? tag : tag.substr(dash + 1));
        } catch (...) { latest = 0; }

        std::string url;
        if (json.contains("assets") && json["assets"].is_array()) {
            for (auto& asset : json["assets"]) {
                if (asset.value("name", "") == UPDATER_ASSET_NAME) {
                    url = asset.value("browser_download_url", "");
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(sStatusMutex);
            sStatus.latestBuild = latest;
        }
        if (url.empty()) {
            SetStatus(State::Error, "The newest release has no Windows download");
        } else if (latest == CurrentBuild()) {
            SetStatus(State::UpToDate, "You have the newest build");
        } else {
            sDownloadUrl = url;
            SetStatus(State::UpdateAvailable, "Build " + std::to_string(latest) + " is available");
        }
        sBusy = false;
    }).detach();
}

void DownloadUpdate() {
    if (sDownloadUrl.empty() || sBusy.exchange(true)) {
        return;
    }
    SetStatus(State::Downloading, "Downloading...", 0.0f);
    std::thread([]() {
        std::filesystem::path dest = DownloadPath();
        std::string error;
        bool ok = HttpGet(
            sDownloadUrl, nullptr, &dest,
            [](uint64_t received, uint64_t total) {
                float progress = total > 0 ? (float)received / (float)total : 0.0f;
                SetStatus(State::Downloading, "Downloading...", progress);
            },
            error);
        if (ok) {
            SetStatus(State::ReadyToInstall, "Downloaded. Restart to install.", 1.0f);
        } else {
            SetStatus(State::Error, error);
        }
        sBusy = false;
    }).detach();
}

// Waits for the game to exit, unpacks the package over the install folder
// (saves, config and oot.o2r are not in the package, so they are left alone)
// and starts the game again.
static const char* kInstallScript = R"PS(
param([int]$GamePid, [string]$Zip, [string]$Dest, [string]$Exe)
$ErrorActionPreference = 'Stop'
try { Wait-Process -Id $GamePid -Timeout 60 -ErrorAction SilentlyContinue } catch {}
Start-Sleep -Milliseconds 500
$tmp = Join-Path $env:TEMP ('zoot-update-' + [guid]::NewGuid())
try {
    Expand-Archive -LiteralPath $Zip -DestinationPath $tmp -Force
    $root = $tmp
    $items = @(Get-ChildItem -LiteralPath $tmp)
    if ($items.Count -eq 1 -and $items[0].PSIsContainer) { $root = $items[0].FullName }
    Copy-Item -Path (Join-Path $root '*') -Destination $Dest -Recurse -Force
    Remove-Item -LiteralPath $Zip -Force
} catch {
    Add-Type -AssemblyName PresentationFramework
    [System.Windows.MessageBox]::Show("The update could not be installed: $_", 'Zelda OoT Realistic') | Out-Null
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
Start-Process -FilePath $Exe -WorkingDirectory $Dest
)PS";

void InstallAndRestart() {
    if (GetStatus().state != State::ReadyToInstall) {
        return;
    }

    std::filesystem::path script = DownloadPath().parent_path() / L"zoot-realistic-update.ps1";
    {
        std::ofstream out(script, std::ios::trunc);
        out << kInstallScript;
    }

    std::filesystem::path exe = ExecutablePath();
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
                       script.wstring() + L"\" -GamePid " + std::to_wstring(GetCurrentProcessId()) + L" -Zip \"" +
                       DownloadPath().wstring() + L"\" -Dest \"" + exe.parent_path().wstring() + L"\" -Exe \"" +
                       exe.wstring() + L"\"";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        SetStatus(State::Error, "Could not start the installer (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    Ship::Context::GetRawInstance()->GetWindow()->Close();
}

} // namespace Updater

#else

namespace Updater {

bool IsSupported() {
    return false;
}

void CheckForUpdate() {
    SetStatus(State::Error, "Updating from the game is only available on Windows for now");
}

void DownloadUpdate() {
}

void InstallAndRestart() {
}

} // namespace Updater

#endif
