#pragma once

#include <windows.h>
#include <string>

namespace micpatch {
struct VirtualDeviceStatus {
    std::wstring detail;
    bool canInstall{};
};
VirtualDeviceStatus virtualDeviceStatus();
bool launchVirtualInstaller(HWND parent);
}
