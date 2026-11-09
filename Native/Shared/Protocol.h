#pragma once
#include <windows.h>
#include <stddef.h>

constexpr DWORD kProtocolMagic = 0x45535452;
constexpr DWORD kMaxExclusions = 64;
constexpr DWORD kPathChars = MAX_PATH;
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\EnergyStar.Control";

enum class PipeAction : DWORD { Status = 1, Configure = 2, Foreground = 3 };
enum class PowerMode : DWORD { Automatic = 0, AlwaysOn = 1, Off = 2 };

struct PipeMessage {
    DWORD magic = kProtocolMagic;
    PipeAction action = PipeAction::Status;
    PowerMode mode = PowerMode::Automatic;
    DWORD logging = 0;
    DWORD onBattery = 0;
    DWORD enabled = 0;
    DWORD processCount = 0;
    DWORD foregroundPid = 0;
    DWORD exclusionCount = 0;
    wchar_t exclusions[kMaxExclusions][kPathChars]{};
};

constexpr DWORD kHeaderSize = static_cast<DWORD>(offsetof(PipeMessage, exclusions));
