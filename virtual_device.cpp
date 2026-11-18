#include "virtual_device.h"
#include "language.h"
#include "audio.h"
#include <winioctl.h>
#include "virtual_mic_protocol.h"

#include <cfgmgr32.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <Aclapi.h>
#include <sddl.h>
#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

namespace micpatch {
namespace {
constexpr wchar_t hardwareId[] = L"ROOT\\MICPATCHMIC";
constexpr wchar_t helperName[] = L"MicPatchDriverInstall.exe";
std::atomic<bool> installing{};

struct Devices {
    HDEVINFO handle = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    ~Devices() { if (handle != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(handle); }
};

std::filesystem::path applicationDirectory() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

bool fileExists(const std::filesystem::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

bool matches(HDEVINFO devices, SP_DEVINFO_DATA& item) {
    DWORD bytes{}, type{};
    SetupDiGetDeviceRegistryPropertyW(devices, &item, SPDRP_HARDWAREID, &type, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !bytes || bytes > 65536 || bytes % sizeof(wchar_t)) return false;
    std::vector<wchar_t> list(bytes / sizeof(wchar_t));
    if (!SetupDiGetDeviceRegistryPropertyW(devices, &item, SPDRP_HARDWAREID, &type,
            reinterpret_cast<PBYTE>(list.data()), bytes, nullptr) || type != REG_MULTI_SZ) return false;
    for (auto it = list.begin(); it != list.end() && *it;) {
        const auto end = std::find(it, list.end(), L'\0');
        if (end == list.end()) break;
        if (_wcsicmp(&*it, hardwareId) == 0) return true;
        it = end + 1;
    }
    return false;
}

#ifdef MPMIC_PINNED_PACKAGE
bool trustedSid(PSID sid) {
    if (!sid) return false;
    if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) return true;
    PSID installer{};
    if (!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &installer)) return false;
    const bool trusted = EqualSid(sid, installer) != FALSE;
    LocalFree(installer);
    return trusted;
}

bool trustedAcl(const std::filesystem::path& item, bool directory) {
    PSID owner{};
    PACL acl{};
    PSECURITY_DESCRIPTOR security{};
    const auto error = GetNamedSecurityInfoW(item.c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &security);
    bool trusted = error == ERROR_SUCCESS && acl && trustedSid(owner);
    const ACCESS_MASK mutation = DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL |
        (directory ? FILE_DELETE_CHILD | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY : FILE_WRITE_DATA | FILE_APPEND_DATA);
    for (DWORD i = 0; trusted && i < acl->AceCount; ++i) {
        void* raw{};
        if (!GetAce(acl, i, &raw)) { trusted = false; break; }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            if ((ace->Mask & mutation) && !trustedSid(&ace->SidStart)) trusted = false;
        } else if (header->AceType != ACCESS_DENIED_ACE_TYPE) {
            trusted = false;
        }
    }
    if (security) LocalFree(security);
    return trusted;
}

bool protectedDeployment(const std::filesystem::path& directory) {
    PWSTR known{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX64, KF_FLAG_DEFAULT_PATH, nullptr, &known))) return false;
    const auto programFiles = std::filesystem::path(known);
    CoTaskMemFree(known);
    const auto expected = programFiles / L"MicPatch";
    if (_wcsicmp(directory.c_str(), expected.c_str()) != 0) return false;
    for (const auto& path : {programFiles, expected, expected / L"driver-package"}) {
        const auto attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !trustedAcl(path, true)) return false;
    }
    const auto executable = expected / helperName;
    return fileExists(executable) && trustedAcl(executable, false);
}
#endif

std::wstring noInstallReason() {
    if (installing) return tr(L"安装程序正在运行。", L"The installer is already running.");
    const auto directory = applicationDirectory();
    if (directory.empty()) return tr(L"无法定位应用目录。", L"Could not locate the application directory.");
    const auto package = directory / L"driver-package";
    if (!fileExists(package / L"MicPatchMic.inf") || !fileExists(package / L"MicPatchMic.cat") ||
        !fileExists(package / L"MicPatchMic.sys")) return tr(L"未随应用提供正式签名的驱动包，暂时不能一键安装。", L"No signed release driver package is bundled. One-click installation is unavailable.");
#ifndef MPMIC_PINNED_PACKAGE
    return tr(L"此版本未绑定正式发布驱动包，不能安装手动放入的文件。", L"This build is not tied to a release driver package and cannot install manually supplied files.");
#else
    if (!protectedDeployment(directory)) return tr(L"安装功能仅在正式部署到 Program Files\\MicPatch 后可用。", L"Installation is only available from a protected Program Files\MicPatch deployment.");
    if (!fileExists(directory / helperName)) return tr(L"缺少驱动安装助手。", L"The driver installer is missing.");
    return {};
#endif
}
}

VirtualDeviceStatus virtualDeviceStatus() {
    Devices devices;
    if (devices.handle == INVALID_HANDLE_VALUE)
        return {std::wstring(tr(L"无法查询 Windows 设备列表。错误码：", L"Could not query Windows devices. Error: ")) + std::to_wstring(GetLastError())};
    std::vector<std::wstring> instances;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA item{};
        item.cbSize = sizeof(item);
        if (!SetupDiEnumDeviceInfo(devices.handle, index, &item)) {
            const auto error = GetLastError();
            if (error == ERROR_NO_MORE_ITEMS) break;
            return {std::wstring(tr(L"设备枚举失败。错误码：", L"Device enumeration failed. Error: ")) + std::to_wstring(error)};
        }
        if (!matches(devices.handle, item)) continue;
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        if (!SetupDiGetDeviceInstanceIdW(devices.handle, &item, instance, MAX_DEVICE_ID_LEN, nullptr))
            return {std::wstring(tr(L"发现 MicPatch 设备，但无法读取实例 ID。错误码：", L"MicPatch device found, but its instance ID could not be read. Error: ")) + std::to_wstring(GetLastError())};
        instances.emplace_back(instance);
    }
    if (instances.empty()) {
        auto reason = noInstallReason();
        if (reason.empty()) return {tr(L"未发现 MicPatch 虚拟麦克风设备。\n可使用随此版本发布的驱动包安装。", L"No MicPatch Virtual Microphone found.\nYou can install the driver bundled with this release."), true};
        return {std::wstring(tr(L"未发现 MicPatch 虚拟麦克风设备。\n", L"No MicPatch Virtual Microphone found.\n")) + reason};
    }
    if (instances.size() != 1)
        return {std::wstring(tr(L"发现多个 MicPatch 设备实例（含历史实例）：", L"Multiple MicPatch device instances found (including inactive records): ")) + std::to_wstring(instances.size()) +
                tr(L"。请在设备管理器检查，不会重复安装。", L". Check Device Manager. No duplicate will be installed.")};
    auto& instance = instances.front();
    const auto suffix = std::wstring(tr(L"\n设备实例：", L"\nDevice instance: ")) + instance;
    DEVINST node{};
    auto result = CM_Locate_DevNodeW(&node, instance.data(), CM_LOCATE_DEVNODE_NORMAL);
    if (result == CR_NO_SUCH_DEVNODE) return {std::wstring(tr(L"已有 MicPatch 设备记录，但设备当前未出现。不会重复安装。", L"A MicPatch device record exists, but the device is not present. No duplicate will be installed.")) + suffix};
    if (result != CR_SUCCESS) return {std::wstring(tr(L"无法定位 MicPatch 设备。配置管理器代码：", L"Could not locate the MicPatch device. Configuration Manager code: ")) + std::to_wstring(result) + suffix};
    ULONG flags{}, problem{};
    result = CM_Get_DevNode_Status(&flags, &problem, node, 0);
    if (result != CR_SUCCESS) return {std::wstring(tr(L"无法读取设备状态。配置管理器代码：", L"Could not read device status. Configuration Manager code: ")) + std::to_wstring(result) + suffix};
    if ((flags & DN_HAS_PROBLEM) || !(flags & DN_STARTED))
        return {std::wstring(tr(L"MicPatch 设备已存在，但驱动未正常启动。PnP 问题码：", L"The MicPatch device exists, but its driver did not start. PnP problem code: ")) + std::to_wstring(problem) + suffix};
    GUID interfaceId = MPMIC_INTERFACE_GUID;
    ULONG length{};
    result = CM_Get_Device_Interface_List_SizeW(&length, &interfaceId, instance.data(), CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (result != CR_SUCCESS) return {std::wstring(tr(L"无法查询 MicPatch 供数接口。配置管理器代码：", L"Could not query the MicPatch PCM interface. Configuration Manager code: ")) + std::to_wstring(result) + suffix};
    if (length < 2) return {std::wstring(tr(L"MicPatch 设备已启动，但专用供数接口未启用。", L"The MicPatch device is started, but its PCM interface is disabled.")) + suffix};
    std::vector<wchar_t> paths(length);
    result = CM_Get_Device_Interface_ListW(&interfaceId, instance.data(), paths.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (result != CR_SUCCESS) return {std::wstring(tr(L"无法读取 MicPatch 供数接口。配置管理器代码：", L"Could not read the MicPatch PCM interface. Configuration Manager code: ")) + std::to_wstring(result) + suffix};
    const auto end = std::find(paths.begin(), paths.end(), L'\0');
    if (!paths.front() || end == paths.end()) return {std::wstring(tr(L"专用供数接口未启用。", L"The PCM interface is not enabled.")) + suffix};
    if (end + 1 == paths.end() || *(end + 1)) return {std::wstring(tr(L"MicPatch 有多个供数接口，无法确认唯一的直连目标。", L"Multiple PCM interfaces found. A unique direct output could not be identified.")) + suffix};
    std::wstring name;
    const auto capture = virtualCaptureName(instance, name);
    if (FAILED(capture)) return {std::wstring(tr(L"无法枚举 Windows 录音端点。错误码：", L"Could not enumerate Windows recording endpoints. Error: ")) + std::to_wstring(static_cast<unsigned long>(capture)) + suffix};
    if (capture == S_FALSE) return {std::wstring(tr(L"供数接口已启用，但未找到同一设备的活动录音端点。请检查 Windows 声音设置。", L"The PCM interface is enabled, but no active recording endpoint belongs to this device. Check Windows sound settings.")) + suffix};
    return {std::wstring(tr(L"已检测到 MicPatch 虚拟麦克风。\n在通话软件中选择：", L"MicPatch Virtual Microphone detected.\nSelect this input in your calling app: ")) + name + suffix};
}

bool launchVirtualInstaller(HWND parent) {
    if (!virtualDeviceStatus().canInstall || installing.exchange(true)) return false;
    const auto directory = applicationDirectory();
    const auto executable = directory / helperName;
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    launch.hwnd = parent;
    launch.lpVerb = L"runas";
    launch.lpFile = executable.c_str();
    launch.lpDirectory = directory.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch) || !launch.hProcess) {
        const auto error = GetLastError();
        installing = false;
        if (error != ERROR_CANCELLED) MessageBoxW(parent, (std::wstring(tr(L"无法启动安装助手。错误码：", L"Could not start the installer. Error: ")) + std::to_wstring(error)).c_str(),
                                                 tr(L"虚拟麦克风", L"Virtual Microphone"), MB_OK | MB_ICONERROR);
        return false;
    }
    try {
        std::thread([parent, process = launch.hProcess] {
            WaitForSingleObject(process, INFINITE);
            CloseHandle(process);
            installing = false;
            PostMessageW(parent, WM_APP + 42, 0, 0);
        }).detach();
    } catch (...) {
        CloseHandle(launch.hProcess);
        installing = false;
        return false;
    }
    return true;
}
}
