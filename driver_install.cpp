// MSVC, native x64/ARM64: cl /std:c++17 /utf-8 /MT /EHsc driver_install.cpp /link /SUBSYSTEM:WINDOWS /DEPENDENTLOADFLAG:0x800
// Define each MICPATCH_RELEASE_*_SHA256 as a quoted hex string from trusted release artifacts.
// The EXE must itself be deployed by a trusted installer before an elevated caller runs it.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <ntsecapi.h>
#include <cfgmgr32.h>
#include <bcrypt.h>
#include <setupapi.h>
#include <newdev.h>
#include <wintrust.h>
#include <softpub.h>
#include <initguid.h>
#include <devpkey.h>
#include <iterator>
#include <new>
#include <string>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Setupapi.lib")
#pragma comment(lib, "Newdev.lib")
#pragma comment(lib, "Wintrust.lib")
#pragma comment(lib, "User32.lib")

#ifndef MICPATCH_RELEASE_INF_SHA256
#define MICPATCH_RELEASE_INF_SHA256 ""
#endif
#ifndef MICPATCH_RELEASE_CAT_SHA256
#define MICPATCH_RELEASE_CAT_SHA256 ""
#endif
#ifndef MICPATCH_RELEASE_SYS_SHA256
#define MICPATCH_RELEASE_SYS_SHA256 ""
#endif

namespace {
constexpr wchar_t kId[] = L"ROOT\\MICPATCHMIC";
constexpr wchar_t kInstance[] = L"ROOT\\MICPATCHMIC\\0000";
constexpr wchar_t kService[] = L"MicPatchMic";
constexpr GUID kMedia = {0x4d36e96c, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
constexpr const wchar_t* kFiles[] = {L"MicPatchMic.inf", L"MicPatchMic.cat", L"MicPatchMic.sys"};
constexpr const char* kPins[] = {MICPATCH_RELEASE_INF_SHA256, MICPATCH_RELEASE_CAT_SHA256,
                                  MICPATCH_RELEASE_SYS_SHA256};

struct Failure { const wchar_t* stage; DWORD code; };
[[noreturn]] void Fail(const wchar_t* stage, DWORD code) {
    throw Failure{stage, code ? code : ERROR_GEN_FAILURE};
}
[[noreturn]] void Last(const wchar_t* stage) { Fail(stage, GetLastError()); }

struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
};
struct DeviceSet { HDEVINFO value; ~DeviceSet() { if (value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); } };
struct InfFile { HINF value; ~InfFile() { if (value != INVALID_HANDLE_VALUE) SetupCloseInfFile(value); } };
struct LocalOwner { HLOCAL value; ~LocalOwner() { if (value) LocalFree(value); } };

bool Equal(const wchar_t* a, const wchar_t* b) {
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

// Be conservative: an unknown allow principal or an ACE type we cannot interpret
// cannot be trusted to keep this pathname immutable to a non-administrator.
class PathGuard {
    PSID installer_ = nullptr;
    std::vector<Handle> dirs_;
    bool Trusted(PSID s) const {
        return IsValidSid(s) && (IsWellKnownSid(s, WinLocalSystemSid) ||
            IsWellKnownSid(s, WinBuiltinAdministratorsSid) || EqualSid(s, installer_));
    }
    void CheckAcl(HANDLE h, bool root, const wchar_t* stage) const {
        PSID owner = nullptr;
        PACL acl = nullptr;
        PSECURITY_DESCRIPTOR sd = nullptr;
        const DWORD result = GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION |
                                             DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &sd);
        LocalOwner freeSd{sd};
        if (result != ERROR_SUCCESS) Fail(stage, result);
        if (!owner || !Trusted(owner) || !acl || !IsValidAcl(acl)) Fail(stage, ERROR_ACCESS_DENIED);
        DWORD mutation = FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE |
                         WRITE_DAC | WRITE_OWNER | GENERIC_ALL | GENERIC_WRITE;
        // The root may allow creating unrelated top-level names; it must not allow
        // replacing existing children. Each used child is locked and checked below.
        if (!root) mutation |= FILE_WRITE_DATA | FILE_APPEND_DATA;
        for (DWORD i = 0; i < acl->AceCount; ++i) {
            void* entry = nullptr;
            if (!GetAce(acl, i, &entry)) Last(stage);
            const auto& header = *static_cast<ACE_HEADER*>(entry);
            if (header.AceFlags & INHERIT_ONLY_ACE) continue;
            if (header.AceType == ACCESS_DENIED_ACE_TYPE) continue;
            if (header.AceType != ACCESS_ALLOWED_ACE_TYPE) Fail(stage, ERROR_ACCESS_DENIED);
            const auto& ace = *static_cast<ACCESS_ALLOWED_ACE*>(entry);
            if ((ace.Mask & mutation) && !Trusted(const_cast<DWORD*>(&ace.SidStart)))
                Fail(stage, ERROR_ACCESS_DENIED);
        }
    }
public:
    PathGuard() {
        if (!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",
                                    &installer_)) Last(L"初始化可信文件所有者");
    }
    ~PathGuard() { LocalFree(installer_); }
    Handle Open(const std::wstring& path, bool dir, const wchar_t* stage, bool root = false) const {
        Handle h(CreateFileW(path.c_str(), dir ? (FILE_READ_ATTRIBUTES | READ_CONTROL) : GENERIC_READ,
                            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | (dir ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
        if (h.value == INVALID_HANDLE_VALUE) Last(stage);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h.value, &info)) Last(stage);
        if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) ||
            (bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != dir) ||
            (!dir && info.nNumberOfLinks != 1) || GetFileType(h.value) != FILE_TYPE_DISK)
            Fail(stage, ERROR_ACCESS_DENIED);
        wchar_t resolved[MAX_PATH + 4]{};
        const DWORD n = GetFinalPathNameByHandleW(h.value, resolved, DWORD(std::size(resolved)),
                                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!n) Last(stage);
        if (n >= std::size(resolved) || !Equal(resolved, (L"\\\\?\\" + path).c_str()))
            Fail(stage, ERROR_ACCESS_DENIED);
        CheckAcl(h.value, root, stage);
        return h;
    }
    void LockDirectories(const std::wstring& path) {
        constexpr auto stage = L"检查安装目录安全性（须为受保护的本地 NTFS 路径）";
        if (path.size() < 3 || path[1] != L':' || path[2] != L'\\' ||
            path.find_first_of(L":/", 2) != std::wstring::npos) Fail(stage, ERROR_BAD_PATHNAME);
        const std::wstring root = path.substr(0, 3);
        wchar_t fs[32]{};
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) Fail(stage, ERROR_BAD_PATHNAME);
        if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs, DWORD(std::size(fs))))
            Last(stage);
        if (!Equal(fs, L"NTFS")) Fail(stage, ERROR_NOT_SUPPORTED);
        dirs_.push_back(Open(root, true, stage, true));
        for (size_t pos = 3; pos < path.size();) {
            const size_t end = path.find(L'\\', pos);
            dirs_.push_back(Open(path.substr(0, end), true, stage));
            if (end == std::wstring::npos) break;
            pos = end + 1;
        }
    }
};

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
void CheckPinDefinitions() {
    for (const char* hex : kPins) {
        if (std::char_traits<char>::length(hex) != 64)
            Fail(L"此助手未配置官方签名发布包的三个 SHA-256 指纹", ERROR_NOT_SUPPORTED);
        for (size_t i = 0; i < 64; ++i)
            if (Hex(hex[i]) < 0) Fail(L"发布包指纹配置格式无效", ERROR_INVALID_DATA);
    }
}
void CheckHash(HANDLE file, const char* hex, const wchar_t* stage) {
    struct Algorithm { BCRYPT_ALG_HANDLE h{}; ~Algorithm() { if (h) BCryptCloseAlgorithmProvider(h, 0); } } alg;
    struct Hash { BCRYPT_HASH_HANDLE h{}; ~Hash() { if (h) BCryptDestroyHash(h); } } hash;
    auto check = [stage](NTSTATUS status) { if (status < 0) Fail(stage, LsaNtStatusToWinError(status)); };
    check(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    check(BCryptCreateHash(alg.h, &hash.h, nullptr, 0, nullptr, 0, 0));
    BYTE data[16384];
    DWORD n = 0;
    do {
        if (!ReadFile(file, data, DWORD(sizeof(data)), &n, nullptr)) Last(stage);
        if (n) check(BCryptHashData(hash.h, data, n, 0));
    } while (n);
    BYTE digest[32];
    check(BCryptFinishHash(hash.h, digest, DWORD(sizeof(digest)), 0));
    for (size_t i = 0; i < std::size(digest); ++i)
        if (digest[i] != (Hex(hex[2*i]) * 16 + Hex(hex[2*i + 1]))) Fail(stage, ERROR_INVALID_DATA);
}

void CheckContents(const std::wstring& dir) {
    constexpr auto stage = L"检查驱动包仅含固定 INF、CAT、SYS 三个文件";
    WIN32_FIND_DATAW data{};
    const HANDLE search = FindFirstFileW((dir + L"\\*").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) Last(stage);
    DWORD error = ERROR_SUCCESS;
    for (;;) {
        bool known = Equal(data.cFileName, L".") || Equal(data.cFileName, L"..");
        for (const auto* file : kFiles) known = known || Equal(data.cFileName, file);
        if (!known) { error = ERROR_INVALID_DATA; break; }
        if (!FindNextFileW(search, &data)) {
            const DWORD last = GetLastError();
            if (last != ERROR_NO_MORE_FILES) error = last;
            break;
        }
    }
    FindClose(search);
    if (error) Fail(stage, error);
}

void CheckInf(const std::wstring& path) {
    constexpr auto stage = L"验证 INF 与 MicPatchMic.INX 的设备 ID、MEDIA 类和型号匹配";
    GUID guid{};
    wchar_t name[MAX_PATH]{};
    if (!SetupDiGetINFClassW(path.c_str(), &guid, name, DWORD(std::size(name)), nullptr)) Last(stage);
    if (!IsEqualGUID(guid, kMedia) || !Equal(name, L"MEDIA")) Fail(stage, ERROR_INVALID_DATA);
    InfFile inf{SetupOpenInfFileW(path.c_str(), L"MEDIA", INF_STYLE_WIN4, nullptr)};
    if (inf.value == INVALID_HANDLE_VALUE) Last(stage);
    auto field = [stage](INFCONTEXT& ctx, DWORD index, const wchar_t* expected) {
        wchar_t value[MAX_PATH]{};
        if (!SetupGetStringFieldW(&ctx, index, value, DWORD(std::size(value)), nullptr)) Last(stage);
        if (!Equal(value, expected)) Fail(stage, ERROR_INVALID_DATA);
    };
    INFCONTEXT cat{}, manufacturer{}, model{};
    if (!SetupFindFirstLineW(inf.value, L"Version", L"CatalogFile", &cat)) Last(stage);
    field(cat, 1, kFiles[1]);
    if (SetupGetLineCountW(inf.value, L"Manufacturer") != 1) Fail(stage, ERROR_INVALID_DATA);
    if (!SetupFindFirstLineW(inf.value, L"Manufacturer", nullptr, &manufacturer)) Last(stage);
    field(manufacturer, 1, L"MicPatch");
    wchar_t models[MAX_PATH]{};
    if (!SetupDiGetActualModelsSectionW(&manufacturer, nullptr, models, DWORD(std::size(models)), nullptr, nullptr))
        Last(stage);
    if (SetupGetLineCountW(inf.value, models) != 1) Fail(stage, ERROR_INVALID_DATA);
    if (!SetupFindFirstLineW(inf.value, models, nullptr, &model)) Last(stage);
    if (SetupGetFieldCount(&model) != 2) Fail(stage, ERROR_INVALID_DATA);
    field(model, 1, kService);
    field(model, 2, kId);
}

void VerifySignatures(const std::wstring& inf, const std::wstring& cat) {
    constexpr auto stage = L"Windows 验证 INF 驱动包签名";
    SP_INF_SIGNER_INFO_W signer{};
    signer.cbSize = sizeof(signer);
    if (!SetupVerifyInfFileW(inf.c_str(), nullptr, &signer)) Last(stage);
    const auto* leaf = wcsrchr(signer.CatalogFile, L'\\');
    if (!Equal(leaf ? leaf + 1 : signer.CatalogFile, kFiles[1]) || !signer.DigitalSigner[0] ||
        (signer.SignerScore & SIGNERSCORE_SIGNED_MASK) != 0)
        Fail(stage, ERROR_INVALID_DATA);

    // Explicitly verify the pinned catalog itself without UI or fetching revocation data.
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = cat.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &policy, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &policy, &trust);
    if (result != ERROR_SUCCESS) Fail(L"Windows 验证固定 CAT 的发布签名及本地吊销信息", DWORD(result));
}

bool FindExisting() {
    constexpr auto stage = L"枚举 ROOT\\MICPATCHMIC（含非当前存在的设备）";
    DeviceSet set{SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES)};
    if (set.value == INVALID_HANDLE_VALUE) Last(stage);
    DWORD matches = 0;
    bool present = false;
    bool expected = false;
    for (DWORD i = 0;; ++i) {
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof(dev);
        if (!SetupDiEnumDeviceInfo(set.value, i, &dev)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            Last(stage);
        }
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        if (!SetupDiGetDeviceInstanceIdW(set.value, &dev, instance, DWORD(std::size(instance)), nullptr)) Last(stage);
        const wchar_t* end = wcsrchr(instance, L'\\');
        const bool rootName = end && CompareStringOrdinal(instance, int(end - instance), kId, -1, TRUE) == CSTR_EQUAL;
        DWORD type = 0, size = 0;
        bool exact = false;
        if (!SetupDiGetDeviceRegistryPropertyW(set.value, &dev, SPDRP_HARDWAREID, &type, nullptr, 0, &size)) {
            const DWORD error = GetLastError();
            if (error != ERROR_INSUFFICIENT_BUFFER && error != ERROR_INVALID_DATA) Fail(stage, error);
        }
        if (size) {
            if (size % sizeof(wchar_t)) Fail(stage, ERROR_INVALID_DATA);
            std::vector<wchar_t> ids(size / sizeof(wchar_t) + 2, 0);
            if (!SetupDiGetDeviceRegistryPropertyW(set.value, &dev, SPDRP_HARDWAREID, &type,
                                                   reinterpret_cast<BYTE*>(ids.data()), size, nullptr)) Last(stage);
            if (type != REG_MULTI_SZ) Fail(stage, ERROR_INVALID_DATA);
            for (const wchar_t* id = ids.data(); *id; id += wcslen(id) + 1) exact = exact || Equal(id, kId);
        }
        if (!exact && !rootName) continue;
        if (++matches > 1) Fail(stage, ERROR_DUPLICATE_FOUND);
        DEVPROP_BOOLEAN here{};
        DEVPROPTYPE propType = 0;
        if (!SetupDiGetDevicePropertyW(set.value, &dev, &DEVPKEY_Device_IsPresent, &propType,
                                       reinterpret_cast<BYTE*>(&here), sizeof(here), nullptr, 0)) Last(stage);
        if (propType != DEVPROP_TYPE_BOOLEAN) Fail(stage, ERROR_INVALID_DATA);
        present = here == DEVPROP_TRUE;
        if (present && exact && IsEqualGUID(dev.ClassGuid, kMedia)) {
            wchar_t service[MAX_PATH]{};
            expected = SetupDiGetDeviceRegistryPropertyW(set.value, &dev, SPDRP_SERVICE, &type,
                reinterpret_cast<BYTE*>(service), sizeof(service) - sizeof(wchar_t), nullptr) &&
                type == REG_SZ && Equal(service, kService);
        }
    }
    if (!matches) return false;
    if (!present) Fail(L"已存在离线实例；为避免重复，不创建或移除设备", ERROR_DEVICE_NOT_CONNECTED);
    if (!expected) Fail(L"已存在实例，但未绑定预期的 MEDIA 驱动服务；不修改原实例", ERROR_ALREADY_EXISTS);
    return true;
}

void ShowError(Failure failure, Failure rollback = {}, BOOL rollbackReboot = FALSE) {
    wchar_t detail[512]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, failure.code,
                   0, detail, DWORD(std::size(detail)), nullptr);
    wchar_t text[1024]{};
    swprintf_s(text, L"阶段：%ls\nWin32/API：%lu (0x%08lX)\n%ls", failure.stage,
               failure.code, failure.code, detail);
    std::wstring message(text);
    if (rollback.code) {
        swprintf_s(text, L"\n回滚阶段：%ls\nWin32/API：%lu (0x%08lX)", rollback.stage,
                   rollback.code, rollback.code);
        message += text;
    }
    if (rollbackReboot) message += L"\n移除本次创建的实例需手动重启；安装仍失败。";
    MessageBoxW(nullptr, message.c_str(), L"MicPatch 虚拟麦克风", MB_OK | MB_ICONERROR);
}

bool OwnDriverBound(HDEVINFO set, SP_DEVINFO_DATA& dev, const wchar_t* published) {
    wchar_t name[MAX_PATH]{};
    DEVPROPTYPE type = 0;
    if (!SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_Device_DriverInfPath, &type,
                                   reinterpret_cast<BYTE*>(name), sizeof(name) - sizeof(wchar_t), nullptr, 0))
        return false;
    return type == DEVPROP_TYPE_STRING && Equal(name, published);
}

int Install() {
    CheckPinDefinitions(); // Default build stops here, before PnP calls or source I/O.
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) Last(L"限制运行时 DLL 搜索路径");
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) Last(L"读取管理员令牌");
    Handle closeToken(token);
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    if (!GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes)) Last(L"读取管理员令牌");
    if (!elevation.TokenIsElevated) Fail(L"须从可信部署位置以管理员身份运行", ERROR_ELEVATION_REQUIRED);

    wchar_t exe[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (!n) Last(L"定位安装助手");
    if (n >= std::size(exe)) Fail(L"定位安装助手", ERROR_FILENAME_EXCED_RANGE);
    const std::wstring exePath(exe);
    const auto slash = exePath.rfind(L'\\');
    if (slash == std::wstring::npos) Fail(L"定位安装助手", ERROR_BAD_PATHNAME);
    const std::wstring dir = exePath.substr(0, slash) + L"\\driver-package";
    if (dir.size() + 1 + wcslen(kFiles[0]) >= MAX_PATH) Fail(L"定位驱动包", ERROR_FILENAME_EXCED_RANGE);
    PathGuard guard;
    guard.LockDirectories(dir);
    auto lockedExe = guard.Open(exePath, false, L"检查安装助手 EXE 的部署权限");
    CheckContents(dir);
    std::wstring paths[3];
    std::vector<Handle> files;
    for (size_t i = 0; i < std::size(kFiles); ++i) {
        paths[i] = dir + L"\\" + kFiles[i];
        files.push_back(guard.Open(paths[i], false, kFiles[i]));
        CheckHash(files.back().value, kPins[i], kFiles[i]);
    }
    CheckInf(paths[0]);
    VerifySignatures(paths[0], paths[1]);
    if (FindExisting()) return 0;

    wchar_t published[MAX_PATH]{};
    wchar_t* publishedLeaf = nullptr;
    if (!SetupCopyOEMInfW(paths[0].c_str(), nullptr, SPOST_NONE, 0, published,
                          DWORD(std::size(published)), nullptr, &publishedLeaf)) Last(L"Windows 验证并暂存驱动包");
    if (!publishedLeaf || !*publishedLeaf) Fail(L"确认暂存的 OEM INF", ERROR_INVALID_DATA);
    wchar_t stored[MAX_PATH]{};
    if (!SetupGetInfDriverStoreLocationW(published, nullptr, nullptr, stored, DWORD(std::size(stored)), nullptr))
        Last(L"确认驱动包已进入 Windows 驱动存储");
    if (FindExisting()) return 0;

    DeviceSet set{SetupDiCreateDeviceInfoList(&kMedia, nullptr)};
    if (set.value == INVALID_HANDLE_VALUE) Last(L"创建 MEDIA 设备信息集");
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof(dev);
    // A fixed instance ID gives an additional OS-level collision check; never take ownership of an existing instance.
    if (!SetupDiCreateDeviceInfoW(set.value, kInstance, &kMedia, L"MicPatch Virtual Microphone",
                                  nullptr, 0, &dev)) Last(L"创建唯一 ROOT\\MICPATCHMIC 实例");
    constexpr wchar_t hwIds[] = L"ROOT\\MICPATCHMIC\0"; // REG_MULTI_SZ: explicit + implicit NUL.
    if (!SetupDiSetDeviceRegistryPropertyW(set.value, &dev, SPDRP_HARDWAREID,
                                           reinterpret_cast<const BYTE*>(hwIds), sizeof(hwIds)))
        Last(L"设置精确硬件 ID");
    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set.value, &dev)) Last(L"注册本次新建的根设备");

    BOOL reboot = FALSE;
    // Full path limits the requested candidate to the pinned package. No FORCE:
    // NewDev applies Windows ranking and will not replace a better match.
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kId, paths[0].c_str(), 0, &reboot)) {
        const DWORD error = GetLastError();
        // PnP might have already bound the staged package after registration.
        if (error != ERROR_NO_MORE_ITEMS || !OwnDriverBound(set.value, dev, publishedLeaf)) {
            BOOL removalReboot = FALSE;
            Failure rollback{};
            if (!DiUninstallDevice(nullptr, set.value, &dev, 0, &removalReboot))
                rollback = {L"移除本次新建的实例", GetLastError()};
            ShowError({L"按 Windows 标准排名绑定驱动", error}, rollback, removalReboot);
            return 1;
        }
    }
    if (reboot) {
        MessageBoxW(nullptr, L"驱动已安装。请在方便时手动重启 Windows。",
                    L"MicPatch 虚拟麦克风", MB_OK | MB_ICONINFORMATION);
        return ERROR_SUCCESS_REBOOT_REQUIRED;
    }
    return 0;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    try {
        if (commandLine && commandLine[0]) Fail(L"此助手不接受任何命令行参数", ERROR_INVALID_PARAMETER);
        return Install();
    } catch (Failure failure) {
        ShowError(failure);
    } catch (const std::bad_alloc&) {
        ShowError({L"分配安装助手内存", ERROR_NOT_ENOUGH_MEMORY});
    } catch (...) {
        ShowError({L"安装助手发生意外错误", ERROR_GEN_FAILURE});
    }
    return 1;
}
