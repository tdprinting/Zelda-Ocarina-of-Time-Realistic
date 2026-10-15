#pragma once

#include <cstdint>
#include <string>

// In-game updater: checks the project's latest GitHub Release and, on Windows,
// downloads it and swaps it in over the running install, then restarts.
namespace Updater {

enum class State {
    Idle,
    Checking,
    UpToDate,
    UpdateAvailable,
    Downloading,
    ReadyToInstall,
    Error,
};

struct Status {
    State state = State::Idle;
    uint32_t latestBuild = 0;
    std::string message;
    float progress = 0.0f; // 0..1 while downloading
};

uint32_t CurrentBuild();
bool IsSupported();
Status GetStatus();

// Each runs on a background thread and returns immediately.
void CheckForUpdate();
void DownloadUpdate();

// Hands the downloaded package to a helper that waits for the game to close,
// unpacks it over the install folder and relaunches. Then closes the game.
void InstallAndRestart();

} // namespace Updater
