#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#include <tlhelp32.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kProcessName[] = L"Frostpunk.exe";
constexpr char kExpectedSha256[] =
    "719c6e016bcdb1021a6401b1624aebdcfb03c6782a763d2fbd8c2805f9dae9d4";

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string win32Error(DWORD error) {
    LPSTR buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string message = size && buffer ? std::string(buffer, size)
                                         : "Win32 error " + std::to_string(error);
    if (buffer) LocalFree(buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
        message.pop_back();
    }
    return message;
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), count);
    return result;
}

std::filesystem::path launcherDirectory() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (!length) fail("Could not resolve the launcher directory.");
    executable.resize(length);
    return std::filesystem::path(executable).parent_path();
}

std::filesystem::path configuredGamePath() {
    const auto config = launcherDirectory() / L"multiplayer.ini";
    wchar_t savedPath[32768]{};
    GetPrivateProfileStringW(L"Game", L"Path", L"", savedPath, 32768, config.c_str());
    if (std::filesystem::is_regular_file(savedPath)) return savedPath;
    std::wstring configured(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"FROSTPUNK_EXE", configured.data(), static_cast<DWORD>(configured.size()));
    if (length && length < configured.size()) {
        configured.resize(length);
        if (std::filesystem::is_regular_file(configured)) return configured;
    }

    const std::array candidates{
        std::filesystem::path(L"D:\\Frostpunk\\Frostpunk.exe"),
        std::filesystem::path(
            L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Frostpunk\\Frostpunk.exe"),
        std::filesystem::path(
            L"C:\\Program Files\\Steam\\steamapps\\common\\Frostpunk\\Frostpunk.exe"),
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"Frostpunk.exe\0Frostpunk.exe\0\0";
    dialog.lpstrFile = savedPath;
    dialog.nMaxFile = 32768;
    dialog.lpstrTitle = L"Выберите установленный Frostpunk.exe";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) fail("Game selection cancelled.");
    WritePrivateProfileStringW(L"Game", L"Path", savedPath, config.c_str());
    return savedPath;
}

std::string sha256(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        fail("BCryptOpenAlgorithmProvider failed.");
    }

    DWORD objectLength = 0;
    DWORD resultLength = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                          &resultLength, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        fail("BCryptGetProperty failed.");
    }

    std::vector<std::uint8_t> hashObject(objectLength);
    if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength,
                         nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        fail("BCryptCreateHash failed.");
    }

    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        fail("Could not open Frostpunk.exe: " + win32Error(GetLastError()));
    }

    std::vector<std::uint8_t> buffer(1024 * 1024);
    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &bytesRead, nullptr)) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            fail("Could not hash Frostpunk.exe: " + win32Error(GetLastError()));
        }
        if (!bytesRead) break;
        if (BCryptHashData(hash, buffer.data(), bytesRead, 0) < 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            fail("BCryptHashData failed.");
        }
    }

    std::array<std::uint8_t, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        fail("BCryptFinishHash failed.");
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (const auto byte : digest) text << std::setw(2) << static_cast<unsigned>(byte);
    return text.str();
}

DWORD findRunningGame() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) fail("Could not enumerate processes: " + win32Error(GetLastError()));
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD result = 0;
    if (Process32FirstW(snapshot.value, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kProcessName) == 0) {
                if (result) fail("More than one Frostpunk.exe process is running.");
                result = entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot.value, &entry));
    }
    return result;
}

std::filesystem::path processImagePath(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
        fail("Could not read the Frostpunk path: " + win32Error(GetLastError()));
    }
    path.resize(size);
    return path;
}

std::uintptr_t remoteModuleBase(DWORD processId, const wchar_t* moduleName) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                            processId));
    if (!snapshot) return 0;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot.value, &module)) {
        do {
            if (_wcsicmp(module.szModule, moduleName) == 0) {
                return reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
            }
        } while (Module32NextW(snapshot.value, &module));
    }
    return 0;
}

bool remoteModuleLoaded(DWORD processId, const std::filesystem::path& path) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                            processId));
    if (!snapshot) return false;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot.value, &module)) {
        do {
            std::error_code error;
            if (std::filesystem::equivalent(module.szExePath, path, error) && !error) {
                return true;
            }
        } while (Module32NextW(snapshot.value, &module));
    }
    return false;
}

void waitForKernel32(DWORD processId, HANDLE process) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            fail("Frostpunk exited before the mod could be loaded.");
        }
        if (remoteModuleBase(processId, L"kernel32.dll")) return;
        Sleep(25);
    }
    fail("Timed out while waiting for Frostpunk to initialize.");
}

void injectLibrary(DWORD processId, HANDLE process,
                   const std::filesystem::path& dllPath) {
    if (remoteModuleLoaded(processId, dllPath)) return;

    const std::wstring path = dllPath.wstring();
    const SIZE_T byteCount = (path.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, byteCount,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) fail("VirtualAllocEx failed: " + win32Error(GetLastError()));

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, path.c_str(), byteCount, &written) ||
        written != byteCount) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        fail("WriteProcessMemory failed: " + win32Error(GetLastError()));
    }

    const HMODULE localKernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto localLoadLibrary = reinterpret_cast<std::uintptr_t>(
        GetProcAddress(localKernel32, "LoadLibraryW"));
    const auto remoteKernel32 = remoteModuleBase(processId, L"kernel32.dll");
    if (!localKernel32 || !localLoadLibrary || !remoteKernel32) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        fail("Could not resolve LoadLibraryW in Frostpunk.");
    }
    const auto loadLibraryRva =
        localLoadLibrary - reinterpret_cast<std::uintptr_t>(localKernel32);
    auto remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        remoteKernel32 + loadLibraryRva);

    Handle thread(CreateRemoteThread(process, nullptr, 0, remoteLoadLibrary,
                                     remotePath, 0, nullptr));
    if (!thread) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        fail("CreateRemoteThread failed: " + win32Error(GetLastError()));
    }
    const DWORD wait = WaitForSingleObject(thread.value, 30000);
    if (wait != WAIT_OBJECT_0) {
        // The loader may still be reading the remote path. Never free it until
        // the thread finishes; the process will reclaim it on exit if timed out.
        fail("FrostMenuMod.dll loading did not finish within 30 seconds. Check the game before retrying.");
    }
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

    Sleep(200);
    if (!remoteModuleLoaded(processId, dllPath)) {
        fail("FrostMenuMod.dll did not appear in the Frostpunk process.");
    }
}

void verifyBuild(const std::filesystem::path& gamePath) {
    const std::string actual = sha256(gamePath);
    if (actual != kExpectedSha256) {
        fail("Unsupported Frostpunk.exe. SHA-256 is " + actual +
             "; expected " + kExpectedSha256 + ".");
    }
}

void launchOrAttach(const std::filesystem::path& dllPath, bool newInstance,
                    DWORD requestedProcessId) {
    DWORD processId = requestedProcessId ? requestedProcessId :
        (newInstance ? 0 : findRunningGame());
    if (processId) {
        Handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                       PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                                       PROCESS_CREATE_THREAD | SYNCHRONIZE,
                                   FALSE, processId));
        if (!process) fail("OpenProcess failed: " + win32Error(GetLastError()));
        verifyBuild(processImagePath(process.value));
        waitForKernel32(processId, process.value);
        injectLibrary(processId, process.value, dllPath);
        return;
    }

    const auto gamePath = std::filesystem::weakly_canonical(configuredGamePath());
    verifyBuild(gamePath);
    const std::wstring workingDirectory = gamePath.parent_path().wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    if (!CreateProcessW(gamePath.c_str(), nullptr, nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP, nullptr, workingDirectory.c_str(),
                        &startup, &information)) {
        fail("Could not start Frostpunk: " + win32Error(GetLastError()));
    }
    Handle process(information.hProcess);
    Handle mainThread(information.hThread);
    processId = information.dwProcessId;
    waitForKernel32(processId, process.value);
    injectLibrary(processId, process.value, dllPath);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        bool newInstance = false;
        DWORD requestedProcessId = 0;
        int count = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments) fail("Could not parse launcher arguments.");
        // Copy before parsing so exceptions cannot leak the argv allocation.
        std::vector<std::wstring> args(arguments + 1, arguments + count);
        LocalFree(arguments);
        for (std::size_t index = 0; index < args.size(); ++index) {
            if (args[index] == L"--new-instance") {
                newInstance = true;
            } else if (args[index] == L"--pid" && index + 1 < args.size()) {
                const auto& text = args[++index];
                if (text.empty() || text.find_first_not_of(L"0123456789") !=
                    std::wstring::npos) fail("PID must contain digits only.");
                const auto value = std::stoull(text);
                if (!value || value > MAXDWORD) fail("PID is outside the valid range.");
                requestedProcessId = static_cast<DWORD>(value);
            } else {
                fail("Usage: FrostpunkMultiplayerLauncher.exe [--new-instance | --pid PID]");
            }
        }
        if (newInstance && requestedProcessId) {
            fail("Choose --new-instance or --pid, not both.");
        }
        const auto directory = launcherDirectory();
        const auto dllPath = std::filesystem::weakly_canonical(
            directory / L"FrostMenuMod.dll");
        if (!std::filesystem::is_regular_file(dllPath)) {
            fail("FrostMenuMod.dll must be next to FrostpunkMultiplayerLauncher.exe.");
        }
        launchOrAttach(dllPath, newInstance, requestedProcessId);
        return 0;
    } catch (const std::exception& error) {
        const std::wstring message = widen(error.what());
        MessageBoxW(nullptr, message.c_str(), L"Frostpunk Multiplayer Launcher",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
}
