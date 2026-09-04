#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

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

DWORD findProcessId() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) fail("Could not enumerate processes: " + win32Error(GetLastError()));
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD processId = 0;
    if (Process32FirstW(snapshot.value, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kProcessName) == 0) {
                if (processId) fail("More than one Frostpunk.exe process is running.");
                processId = entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot.value, &entry));
    }
    if (!processId) fail("Frostpunk.exe is not running.");
    return processId;
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
        fail("Could not open Frostpunk.exe for hashing: " + win32Error(GetLastError()));
    }

    // Keep the 1 MiB streaming buffer off the default 1 MiB Windows thread stack.
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

std::uintptr_t remoteModuleBase(DWORD processId, const wchar_t* moduleName) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                            processId));
    if (!snapshot) fail("Could not enumerate target modules: " + win32Error(GetLastError()));
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

bool remoteModuleLoaded(DWORD processId, const std::filesystem::path& dllPath) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                            processId));
    if (!snapshot) return false;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot.value, &module)) {
        do {
            std::error_code error;
            if (std::filesystem::equivalent(module.szExePath, dllPath, error) && !error) {
                return true;
            }
        } while (Module32NextW(snapshot.value, &module));
    }
    return false;
}

void injectLibrary(DWORD processId, HANDLE process, const std::filesystem::path& dllPath) {
    if (remoteModuleLoaded(processId, dllPath)) {
        std::cout << "FrostMenuMod.dll is already loaded.\n";
        return;
    }

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

    HMODULE localKernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto localLoadLibrary = reinterpret_cast<std::uintptr_t>(
        GetProcAddress(localKernel32, "LoadLibraryW"));
    const auto remoteKernel32 = remoteModuleBase(processId, L"kernel32.dll");
    if (!localKernel32 || !localLoadLibrary || !remoteKernel32) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        fail("Could not resolve LoadLibraryW in the target process.");
    }
    const auto loadLibraryRva = localLoadLibrary -
        reinterpret_cast<std::uintptr_t>(localKernel32);
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

    Sleep(250);
    if (!remoteModuleLoaded(processId, dllPath)) {
        fail("LoadLibraryW returned, but FrostMenuMod.dll is not present in the process.");
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::filesystem::path dllPath;
        if (argc >= 2) {
            dllPath = std::filesystem::absolute(argv[1]);
        } else {
            std::wstring executable(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                                    static_cast<DWORD>(executable.size()));
            executable.resize(length);
            dllPath = std::filesystem::path(executable).parent_path() / L"FrostMenuMod.dll";
        }
        dllPath = std::filesystem::weakly_canonical(dllPath);
        if (!std::filesystem::is_regular_file(dllPath)) {
            fail("FrostMenuMod.dll was not found next to the injector.");
        }

        const DWORD processId = findProcessId();
        Handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                       PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                                       PROCESS_CREATE_THREAD,
                                   FALSE, processId));
        if (!process) fail("OpenProcess failed: " + win32Error(GetLastError()));

        const auto imagePath = processImagePath(process.value);
        const std::string actualSha256 = sha256(imagePath);
        if (actualSha256 != kExpectedSha256) {
            fail("Unsupported Frostpunk.exe. SHA-256 is " + actualSha256 +
                 "; this mod only supports " + kExpectedSha256 + ".");
        }

        std::wcout << L"Verified Frostpunk build: " << imagePath << L"\n";
        injectLibrary(processId, process.value, dllPath);
        std::cout << "FrostMenuMod.dll loaded into PID " << processId << ".\n";
        std::cout << "Return to the main menu and look for the MULTIPLAYER item.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
