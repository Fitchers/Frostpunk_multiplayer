#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr char kMagic[8] = {'F', 'R', 'S', 'C', 'A', 'N', '1', '\0'};
constexpr std::uint32_t kStateVersion = 1;
constexpr SIZE_T kReadChunk = 1024 * 1024;

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) { other.value = nullptr; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

struct ScanState {
    DWORD pid = 0;
    std::uint64_t processStartTime = 0;
    std::int32_t value = 0;
    std::vector<std::uint64_t> addresses;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string win32Error(DWORD error) {
    LPSTR buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string message = size && buffer ? std::string(buffer, size)
                                         : "Win32 error " + std::to_string(error);
    if (buffer) LocalFree(buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) message.pop_back();
    return message;
}

std::wstring argumentValue(const std::vector<std::wstring>& args, const std::wstring& name) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == name) {
            if (i + 1 >= args.size()) fail("Missing value for an option.");
            return args[i + 1];
        }
    }
    fail("Required option is missing.");
}

std::optional<std::wstring> optionalArgumentValue(
    const std::vector<std::wstring>& args, const std::wstring& name) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == name) {
            if (i + 1 >= args.size()) fail("Missing value for an option.");
            return args[i + 1];
        }
    }
    return std::nullopt;
}

bool hasFlag(const std::vector<std::wstring>& args, const std::wstring& name) {
    return std::find(args.begin(), args.end(), name) != args.end();
}

std::int32_t parseInt32(const std::wstring& text) {
    std::size_t consumed = 0;
    const long long parsed = std::stoll(text, &consumed, 0);
    if (consumed != text.size() || parsed < std::numeric_limits<std::int32_t>::min() ||
        parsed > std::numeric_limits<std::int32_t>::max()) {
        fail("Value must be a signed 32-bit integer.");
    }
    return static_cast<std::int32_t>(parsed);
}

std::size_t parseSize(const std::wstring& text) {
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(text, &consumed, 0);
    if (consumed != text.size()) fail("Expected a non-negative integer.");
    return static_cast<std::size_t>(parsed);
}

std::uint64_t parseUint64(const std::wstring& text) {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(text, &consumed, 0);
    if (consumed != text.size()) fail("Expected an unsigned integer or hexadecimal address.");
    return static_cast<std::uint64_t>(parsed);
}

std::wstring normalizeProcessName(std::wstring name) {
    if (name.size() < 4 || _wcsicmp(name.c_str() + name.size() - 4, L".exe") != 0) {
        name += L".exe";
    }
    return name;
}

DWORD findProcessId(const std::wstring& requestedName) {
    const std::wstring processName = normalizeProcessName(requestedName);
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) fail("CreateToolhelp32Snapshot failed: " + win32Error(GetLastError()));

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD result = 0;
    if (Process32FirstW(snapshot.value, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName.c_str()) == 0) {
                if (result != 0) fail("More than one matching process is running; use --pid.");
                result = entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot.value, &entry));
    }
    if (result == 0) fail("Process was not found. Launch the game and load a city first.");
    return result;
}

Handle openProcessForRead(DWORD pid) {
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!process) fail("OpenProcess failed: " + win32Error(GetLastError()));
    return process;
}

std::uint64_t getProcessStartTime(HANDLE process) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        fail("GetProcessTimes failed: " + win32Error(GetLastError()));
    }
    ULARGE_INTEGER value{};
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    return value.QuadPart;
}

bool isReadable(const MEMORY_BASIC_INFORMATION& memory) {
    if (memory.State != MEM_COMMIT) return false;
    if (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD protection = memory.Protect & 0xff;
    return protection == PAGE_READONLY || protection == PAGE_READWRITE ||
           protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

std::vector<std::uint64_t> scanExactInt32(HANDLE process, std::int32_t target, bool unaligned) {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    auto cursor = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    std::vector<std::uint64_t> matches;
    std::vector<std::uint8_t> buffer(kReadChunk);

    while (cursor < maximum) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(cursor), &memory, sizeof(memory)) == 0) break;
        const auto regionBase = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto regionSize = static_cast<std::uintptr_t>(memory.RegionSize);
        const auto regionEnd = regionBase + regionSize;

        if (isReadable(memory)) {
            auto chunkBase = regionBase;
            while (chunkBase < regionEnd) {
                const SIZE_T remaining = static_cast<SIZE_T>(regionEnd - chunkBase);
                const SIZE_T requested = std::min(kReadChunk, remaining);
                SIZE_T bytesRead = 0;
                if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(chunkBase), buffer.data(),
                                      requested, &bytesRead) && bytesRead >= sizeof(target)) {
                    for (SIZE_T offset = 0; offset + sizeof(target) <= bytesRead; ++offset) {
                        const auto address = chunkBase + offset;
                        if (!unaligned && (address % alignof(std::int32_t)) != 0) continue;
                        std::int32_t candidate = 0;
                        std::memcpy(&candidate, buffer.data() + offset, sizeof(candidate));
                        if (candidate == target) matches.push_back(static_cast<std::uint64_t>(address));
                    }
                }
                if (requested <= sizeof(target) - 1 || chunkBase + requested >= regionEnd) break;
                chunkBase += requested - (sizeof(target) - 1);
            }
        }

        if (regionEnd <= cursor) break;
        cursor = regionEnd;
    }

    return matches;
}

template <typename T>
void writeScalar(std::ofstream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!output) fail("Failed while writing scan state.");
}

template <typename T>
T readScalar(std::ifstream& input) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) fail("Scan state is truncated.");
    return value;
}

void saveState(const std::filesystem::path& path, const ScanState& state) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) fail("Could not create scan state file.");
    output.write(kMagic, sizeof(kMagic));
    writeScalar(output, kStateVersion);
    writeScalar(output, state.pid);
    writeScalar(output, state.processStartTime);
    writeScalar(output, state.value);
    const auto count = static_cast<std::uint64_t>(state.addresses.size());
    writeScalar(output, count);
    if (!state.addresses.empty()) {
        output.write(reinterpret_cast<const char*>(state.addresses.data()),
                     static_cast<std::streamsize>(state.addresses.size() * sizeof(state.addresses[0])));
    }
    if (!output) fail("Failed while writing scan addresses.");
}

ScanState loadState(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) fail("Could not open scan state file.");
    char magic[sizeof(kMagic)]{};
    input.read(magic, sizeof(magic));
    if (!input || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) fail("Not a FrostProbe state file.");
    const auto version = readScalar<std::uint32_t>(input);
    if (version != kStateVersion) fail("Unsupported scan state version.");

    ScanState state;
    state.pid = readScalar<DWORD>(input);
    state.processStartTime = readScalar<std::uint64_t>(input);
    state.value = readScalar<std::int32_t>(input);
    const auto count = readScalar<std::uint64_t>(input);
    if (count > 100'000'000ULL) fail("Scan state candidate count is invalid.");
    state.addresses.resize(static_cast<std::size_t>(count));
    if (count != 0) {
        input.read(reinterpret_cast<char*>(state.addresses.data()),
                   static_cast<std::streamsize>(count * sizeof(state.addresses[0])));
        if (!input) fail("Scan state address list is truncated.");
    }
    return state;
}

void validateSameProcess(HANDLE process, const ScanState& state) {
    if (getProcessStartTime(process) != state.processStartTime) {
        fail("The original process has exited or the PID was reused. Start a new scan.");
    }
}

std::vector<std::uint64_t> filterExactInt32(
    HANDLE process, const std::vector<std::uint64_t>& addresses, std::int32_t target) {
    std::vector<std::uint64_t> matches;
    matches.reserve(addresses.size());
    for (const auto address : addresses) {
        std::int32_t candidate = 0;
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), &candidate,
                              sizeof(candidate), &bytesRead) &&
            bytesRead == sizeof(candidate) && candidate == target) {
            matches.push_back(address);
        }
    }
    return matches;
}

enum class ValueInterpretation {
    Int32,
    Float32,
};

float bitsToFloat(std::int32_t bits) {
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::vector<std::uint64_t> filterDecreased(
    HANDLE process, const ScanState& state, ValueInterpretation interpretation) {
    std::vector<std::uint64_t> matches;
    matches.reserve(state.addresses.size());
    const float baselineFloat = bitsToFloat(state.value);
    for (const auto address : state.addresses) {
        std::int32_t currentBits = 0;
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), &currentBits,
                               sizeof(currentBits), &bytesRead) || bytesRead != sizeof(currentBits)) {
            continue;
        }

        bool keep = false;
        if (interpretation == ValueInterpretation::Int32) {
            keep = currentBits >= 0 && currentBits < state.value;
        } else {
            const float currentFloat = bitsToFloat(currentBits);
            keep = std::isfinite(currentFloat) && currentFloat >= 0.0f && currentFloat < baselineFloat;
        }
        if (keep) matches.push_back(address);
    }
    return matches;
}

std::string memoryType(DWORD type) {
    if (type == MEM_IMAGE) return "image";
    if (type == MEM_MAPPED) return "mapped";
    if (type == MEM_PRIVATE) return "private";
    return "unknown";
}

void printCandidates(HANDLE process, const ScanState& state, std::size_t limit) {
    const auto count = std::min(limit, state.addresses.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto address = state.addresses[i];
        MEMORY_BASIC_INFORMATION memory{};
        const SIZE_T queried = VirtualQueryEx(
            process, reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory));
        std::cout << "0x" << std::hex << std::uppercase << address << std::dec;
        if (queried) {
            std::cout << "  " << memoryType(memory.Type)
                      << "  allocation=0x" << std::hex << std::uppercase
                      << reinterpret_cast<std::uintptr_t>(memory.AllocationBase) << std::dec;
        }
        std::cout << '\n';
    }
    if (state.addresses.size() > count) {
        std::cout << "... " << (state.addresses.size() - count) << " more\n";
    }
}

void printCurrentValues(HANDLE process, const ScanState& state, std::size_t limit) {
    const auto count = std::min(limit, state.addresses.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto address = state.addresses[i];
        std::int32_t bits = 0;
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), &bits,
                               sizeof(bits), &bytesRead) || bytesRead != sizeof(bits)) {
            continue;
        }
        std::cout << "0x" << std::hex << std::uppercase << address
                  << "  bits=0x" << static_cast<std::uint32_t>(bits)
                  << std::dec << "  i32=" << bits << "  f32=" << bitsToFloat(bits) << '\n';
    }
    if (state.addresses.size() > count) {
        std::cout << "... " << (state.addresses.size() - count) << " more\n";
    }
}

void printMemory(HANDLE process, std::uint64_t address, std::size_t requestedSize) {
    constexpr std::size_t kMaximumPeekSize = 64 * 1024;
    if (requestedSize == 0 || requestedSize > kMaximumPeekSize) {
        fail("Peek size must be between 1 and 65536 bytes.");
    }

    std::vector<std::uint8_t> bytes(requestedSize);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), bytes.data(),
                           bytes.size(), &bytesRead) || bytesRead == 0) {
        fail("ReadProcessMemory failed at target address: " + win32Error(GetLastError()));
    }
    bytes.resize(bytesRead);

    for (std::size_t row = 0; row < bytes.size(); row += 16) {
        const auto rowSize = std::min<std::size_t>(16, bytes.size() - row);
        std::cout << "0x" << std::hex << std::uppercase << std::setw(16)
                  << std::setfill('0') << (address + row) << "  ";
        for (std::size_t column = 0; column < 16; ++column) {
            if (column < rowSize) {
                std::cout << std::setw(2) << static_cast<unsigned>(bytes[row + column]);
            } else {
                std::cout << "  ";
            }
            std::cout << (column == 7 ? "  " : " ");
        }
        std::cout << " |";
        for (std::size_t column = 0; column < rowSize; ++column) {
            const auto value = bytes[row + column];
            std::cout << (value >= 0x20 && value <= 0x7e ? static_cast<char>(value) : '.');
        }
        std::cout << "|\n";
    }
    std::cout << std::dec << std::nouppercase << std::setfill(' ');
}

bool configureWatchpoints(HANDLE thread, const std::vector<std::uint64_t>& addresses) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(thread, &context)) return false;

    context.Dr0 = 0;
    context.Dr1 = 0;
    context.Dr2 = 0;
    context.Dr3 = 0;
    context.Dr6 = 0;
    context.Dr7 = 0;

    DWORD64* registers[] = {&context.Dr0, &context.Dr1, &context.Dr2, &context.Dr3};
    for (std::size_t slot = 0; slot < addresses.size(); ++slot) {
        *registers[slot] = addresses[slot];
        context.Dr7 |= (1ULL << (slot * 2));          // local enable
        context.Dr7 |= (1ULL << (16 + slot * 4));     // break on write
        context.Dr7 |= (3ULL << (18 + slot * 4));     // 4-byte length
    }
    return SetThreadContext(thread, &context) != FALSE;
}

void configureAllProcessThreads(DWORD pid, const std::vector<std::uint64_t>& addresses) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!snapshot) fail("Could not enumerate process threads: " + win32Error(GetLastError()));

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot.value, &entry)) return;
    do {
        if (entry.th32OwnerProcessID != pid) continue;
        Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                                 FALSE, entry.th32ThreadID));
        if (thread) configureWatchpoints(thread.value, addresses);
    } while (Thread32Next(snapshot.value, &entry));
}

struct ModuleLocation {
    std::wstring name;
    std::uint64_t base = 0;
};

std::optional<ModuleLocation> moduleForAddress(DWORD pid, std::uint64_t address) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) return std::nullopt;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (!Module32FirstW(snapshot.value, &module)) return std::nullopt;
    do {
        const auto base = reinterpret_cast<std::uint64_t>(module.modBaseAddr);
        const auto end = base + module.modBaseSize;
        if (address >= base && address < end) {
            return ModuleLocation{module.szModule, base};
        }
    } while (Module32NextW(snapshot.value, &module));
    return std::nullopt;
}

std::optional<ModuleLocation> moduleByName(DWORD pid, const std::wstring& requestedName) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) return std::nullopt;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (!Module32FirstW(snapshot.value, &module)) return std::nullopt;
    do {
        if (_wcsicmp(module.szModule, requestedName.c_str()) == 0) {
            return ModuleLocation{module.szModule, reinterpret_cast<std::uint64_t>(module.modBaseAddr)};
        }
    } while (Module32NextW(snapshot.value, &module));
    return std::nullopt;
}

template <typename T>
T readProcessScalar(HANDLE process, std::uint64_t address) {
    T value{};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), &value,
                           sizeof(value), &bytesRead) || bytesRead != sizeof(value)) {
        fail("ReadProcessMemory failed at target address: " + win32Error(GetLastError()));
    }
    return value;
}

bool configureExecutionBreakpoint(HANDLE thread, std::uint64_t address) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(thread, &context)) return false;
    context.Dr0 = address;
    context.Dr1 = 0;
    context.Dr2 = 0;
    context.Dr3 = 0;
    context.Dr6 = 0;
    context.Dr7 = 1; // local enable for DR0; execute, one-byte encoding is zero
    return SetThreadContext(thread, &context) != FALSE;
}

void configureExecutionBreakpointForAllThreads(DWORD pid, std::uint64_t address) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!snapshot) fail("Could not enumerate process threads: " + win32Error(GetLastError()));
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot.value, &entry)) return;
    do {
        if (entry.th32OwnerProcessID != pid) continue;
        Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                                 FALSE, entry.th32ThreadID));
        if (thread) configureExecutionBreakpoint(thread.value, address);
    } while (Thread32Next(snapshot.value, &entry));
}

void overrideR8OnCall(DWORD pid, std::uint64_t functionAddress,
                      std::uint64_t expectedRdx, std::int32_t expectedR8,
                      std::int32_t replacementR8) {
    auto process = openProcessForRead(pid);
    if (!DebugActiveProcess(pid)) {
        fail("DebugActiveProcess failed: " + win32Error(GetLastError()));
    }
    DebugSetProcessKillOnExit(FALSE);

    const ULONGLONG deadline = GetTickCount64() + 5ULL * 60ULL * 1000ULL;
    while (GetTickCount64() < deadline) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, 1000)) {
            if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
            configureAllProcessThreads(pid, {});
            DebugActiveProcessStop(pid);
            fail("WaitForDebugEvent failed: " + win32Error(GetLastError()));
        }

        DWORD continueStatus = DBG_CONTINUE;
        bool replaced = false;
        switch (event.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT:
                configureExecutionBreakpoint(event.u.CreateProcessInfo.hThread, functionAddress);
                if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
                if (event.u.CreateProcessInfo.hThread) CloseHandle(event.u.CreateProcessInfo.hThread);
                if (event.u.CreateProcessInfo.hProcess) CloseHandle(event.u.CreateProcessInfo.hProcess);
                break;
            case CREATE_THREAD_DEBUG_EVENT:
                configureExecutionBreakpoint(event.u.CreateThread.hThread, functionAddress);
                if (event.u.CreateThread.hThread) CloseHandle(event.u.CreateThread.hThread);
                break;
            case LOAD_DLL_DEBUG_EVENT:
                if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
                break;
            case EXCEPTION_DEBUG_EVENT: {
                const DWORD code = event.u.Exception.ExceptionRecord.ExceptionCode;
                if (code == EXCEPTION_BREAKPOINT) {
                    std::cout << "OVERRIDE_READY pid=" << pid
                              << " function=0x" << std::hex << functionAddress
                              << " match-rdx=0x" << expectedRdx << std::dec << std::endl;
                } else if (code == EXCEPTION_SINGLE_STEP) {
                    Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                             THREAD_QUERY_INFORMATION, FALSE, event.dwThreadId));
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_DEBUG_REGISTERS;
                    if (thread && GetThreadContext(thread.value, &context) &&
                        (context.Dr6 & 1) && context.Rip == functionAddress) {
                        const auto incomingR8 = static_cast<std::int32_t>(context.R8);
                        if (context.Rdx == expectedRdx && incomingR8 == expectedR8) {
                            context.R8 = static_cast<std::uint32_t>(replacementR8);
                            context.Dr0 = context.Dr1 = context.Dr2 = context.Dr3 = 0;
                            context.Dr6 = context.Dr7 = 0;
                            if (!SetThreadContext(thread.value, &context)) {
                                fail("SetThreadContext failed: " + win32Error(GetLastError()));
                            }
                            std::cout << "OVERRIDE_HIT thread=" << event.dwThreadId
                                      << " r8d=" << incomingR8 << " -> " << replacementR8
                                      << " rdx=0x" << std::hex << expectedRdx << std::dec << '\n';
                            replaced = true;
                        } else {
                            context.EFlags |= 0x10000; // resume flag: execute once past DR0
                            SetThreadContext(thread.value, &context);
                        }
                    } else {
                        continueStatus = DBG_EXCEPTION_NOT_HANDLED;
                    }
                } else {
                    continueStatus = DBG_EXCEPTION_NOT_HANDLED;
                }
                break;
            }
            case EXIT_PROCESS_DEBUG_EVENT:
                ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
                return;
            default:
                break;
        }

        if (replaced) configureAllProcessThreads(pid, {});
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continueStatus);
        if (replaced) {
            if (!DebugActiveProcessStop(pid)) {
                std::cerr << "warning: explicit debugger detach returned: "
                          << win32Error(GetLastError())
                          << "; process will be detached when FrostProbe exits.\n";
            }
            return;
        }
    }

    configureAllProcessThreads(pid, {});
    DebugActiveProcessStop(pid);
    fail("Override timed out after five minutes without a matching call.");
}

void setCoalTarget(const ScanState& state, std::int32_t target) {
    auto process = openProcessForRead(state.pid);
    validateSameProcess(process.value, state);
    const auto module = moduleByName(state.pid, L"Frostpunk.exe");
    if (!module) fail("Frostpunk.exe module was not found in the target process.");

    constexpr std::uint64_t kEconomyGlobalRva = 0x3FC93B0;
    constexpr std::uint64_t kChangeResourceRva = 0x168E340;
    constexpr std::uint64_t kResourceContainerOffset = 0x2240;
    constexpr std::uint64_t kResourceCountOffset = 0x2248;
    constexpr std::uint64_t kRecordSize = 0x70;
    constexpr std::uint64_t kCoalIndex = 7;

    const auto economy = readProcessScalar<std::uint64_t>(
        process.value, module->base + kEconomyGlobalRva);
    if (!economy) fail("gEconomy is null; load a city first.");
    const auto records = readProcessScalar<std::uint64_t>(
        process.value, economy + kResourceContainerOffset);
    const auto count = readProcessScalar<std::int32_t>(
        process.value, economy + kResourceCountOffset);
    if (!records || count <= static_cast<std::int32_t>(kCoalIndex)) {
        fail("The expected resource container layout is not present.");
    }

    const auto coalRecord = records + kCoalIndex * kRecordSize;
    const auto coalResource = readProcessScalar<std::uint64_t>(process.value, coalRecord);
    const auto current = readProcessScalar<std::int32_t>(process.value, coalRecord + 8);
    const auto delta64 = static_cast<std::int64_t>(target) - current;
    if (delta64 < std::numeric_limits<std::int32_t>::min() ||
        delta64 > std::numeric_limits<std::int32_t>::max()) {
        fail("Requested target requires an out-of-range delta.");
    }
    if (current == target) {
        std::cout << "Coal is already at the requested target.\n";
        return;
    }

    std::cout << "Coal current=" << current << " target=" << target
              << " replacement-delta=" << delta64 << '\n';
    overrideR8OnCall(state.pid, module->base + kChangeResourceRva,
                     coalResource, -1, static_cast<std::int32_t>(delta64));
}

void printWatchHit(HANDLE process, DWORD pid, DWORD threadId,
                   const std::vector<std::uint64_t>& addresses) {
    Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId));
    if (!thread) fail("Could not open the triggering thread: " + win32Error(GetLastError()));

    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(thread.value, &context)) {
        fail("GetThreadContext failed: " + win32Error(GetLastError()));
    }

    std::cout << "WATCH_HIT thread=" << threadId << " dr6=0x" << std::hex
              << context.Dr6 << std::dec << '\n';
    for (std::size_t slot = 0; slot < addresses.size(); ++slot) {
        if (context.Dr6 & (1ULL << slot)) {
            std::cout << "slot=" << slot << " watched=0x" << std::hex
                      << addresses[slot] << std::dec << '\n';
        }
    }

    std::cout << "RIP(after write)=0x" << std::hex << context.Rip << std::dec;
    if (const auto module = moduleForAddress(pid, context.Rip)) {
        std::wcout << L"  module=" << module->name << L"+0x"
                   << std::hex << (context.Rip - module->base) << std::dec;
    }
    std::cout << '\n';

    std::cout << std::hex << std::uppercase
              << "RAX=" << context.Rax << " RBX=" << context.Rbx
              << " RCX=" << context.Rcx << " RDX=" << context.Rdx << '\n'
              << "RSI=" << context.Rsi << " RDI=" << context.Rdi
              << " RBP=" << context.Rbp << " RSP=" << context.Rsp << '\n'
              << "R8 =" << context.R8  << " R9 =" << context.R9
              << " R10=" << context.R10 << " R11=" << context.R11 << '\n'
              << "R12=" << context.R12 << " R13=" << context.R13
              << " R14=" << context.R14 << " R15=" << context.R15
              << std::dec << std::nouppercase << '\n';

    const std::uint64_t codeStart = context.Rip >= 16 ? context.Rip - 16 : context.Rip;
    std::array<std::uint8_t, 48> code{};
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(codeStart), code.data(),
                          code.size(), &bytesRead)) {
        std::cout << "code[0x" << std::hex << std::uppercase << codeStart << "]=";
        for (SIZE_T i = 0; i < bytesRead; ++i) {
            std::cout << (i ? " " : "") << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(code[i]);
        }
        std::cout << std::dec << std::nouppercase << '\n';
    }

    std::array<std::uint64_t, 12> stack{};
    bytesRead = 0;
    if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(context.Rsp), stack.data(),
                          sizeof(stack), &bytesRead)) {
        std::cout << "stack=";
        for (SIZE_T i = 0; i < bytesRead / sizeof(stack[0]); ++i) {
            std::cout << (i ? " " : "") << "0x" << std::hex << std::uppercase << stack[i];
        }
        std::cout << std::dec << std::nouppercase << '\n';
    }
}

void watchForWrite(const ScanState& state) {
    if (state.addresses.empty() || state.addresses.size() > 4) {
        fail("Hardware watch mode requires between one and four candidate addresses.");
    }

    auto process = openProcessForRead(state.pid);
    validateSameProcess(process.value, state);
    if (!DebugActiveProcess(state.pid)) {
        fail("DebugActiveProcess failed: " + win32Error(GetLastError()));
    }
    DebugSetProcessKillOnExit(FALSE);

    const auto clearWatchpoints = [&]() {
        configureAllProcessThreads(state.pid, {});
    };

    const ULONGLONG deadline = GetTickCount64() + 5ULL * 60ULL * 1000ULL;
    while (GetTickCount64() < deadline) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, 1000)) {
            if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
            clearWatchpoints();
            DebugActiveProcessStop(state.pid);
            fail("WaitForDebugEvent failed: " + win32Error(GetLastError()));
        }

        DWORD continueStatus = DBG_CONTINUE;
        bool hit = false;
        switch (event.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT:
                configureWatchpoints(event.u.CreateProcessInfo.hThread, state.addresses);
                if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
                if (event.u.CreateProcessInfo.hThread) CloseHandle(event.u.CreateProcessInfo.hThread);
                if (event.u.CreateProcessInfo.hProcess) CloseHandle(event.u.CreateProcessInfo.hProcess);
                break;
            case CREATE_THREAD_DEBUG_EVENT:
                configureWatchpoints(event.u.CreateThread.hThread, state.addresses);
                if (event.u.CreateThread.hThread) CloseHandle(event.u.CreateThread.hThread);
                break;
            case LOAD_DLL_DEBUG_EVENT:
                if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
                break;
            case EXCEPTION_DEBUG_EVENT: {
                const DWORD code = event.u.Exception.ExceptionRecord.ExceptionCode;
                if (code == EXCEPTION_BREAKPOINT) {
                    std::cout << "WATCH_READY pid=" << state.pid
                              << " addresses=" << state.addresses.size() << std::endl;
                } else if (code == EXCEPTION_SINGLE_STEP) {
                    Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                             FALSE, event.dwThreadId));
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (thread && GetThreadContext(thread.value, &context) && (context.Dr6 & 0xF)) {
                        printWatchHit(process.value, state.pid, event.dwThreadId, state.addresses);
                        hit = true;
                    } else {
                        continueStatus = DBG_EXCEPTION_NOT_HANDLED;
                    }
                } else {
                    continueStatus = DBG_EXCEPTION_NOT_HANDLED;
                }
                break;
            }
            case EXIT_PROCESS_DEBUG_EVENT:
                ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
                return;
            default:
                break;
        }

        if (hit) clearWatchpoints();
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continueStatus);
        if (hit) {
            if (!DebugActiveProcessStop(state.pid)) {
                std::cerr << "warning: explicit debugger detach returned: "
                          << win32Error(GetLastError())
                          << "; process will be detached when FrostProbe exits.\n";
            }
            return;
        }
    }

    clearWatchpoints();
    DebugActiveProcessStop(state.pid);
    fail("Watch timed out after five minutes without a write.");
}

void printUsage() {
    std::cout <<
        "FrostProbe - read-only exact int32 process-memory scanner\n\n"
        "  FrostProbe scan --process Frostpunk.exe --value 123 --state coal.scan [--unaligned]\n"
        "  FrostProbe scan --pid 1234 --value 123 --state coal.scan [--unaligned]\n"
        "  FrostProbe next --state coal.scan --value 127\n"
        "  FrostProbe decreased-i32 --state coal.scan\n"
        "  FrostProbe decreased-f32 --state coal-f32.scan\n"
        "  FrostProbe watch --state coal.scan\n"
        "  FrostProbe override-r8 --pid 1234 --address 0x... --match-rdx 0x... --from -1 --to 100\n"
        "  FrostProbe set-coal --state coal.scan --target 200\n"
        "  FrostProbe peek --process Frostpunk.exe --address 0x... --bytes 256\n"
        "  FrostProbe list --state coal.scan [--limit 50]\n";
}

DWORD pidFromArguments(const std::vector<std::wstring>& args) {
    if (const auto pid = optionalArgumentValue(args, L"--pid")) {
        const auto parsed = parseSize(*pid);
        if (parsed == 0 || parsed > std::numeric_limits<DWORD>::max()) fail("PID is invalid.");
        return static_cast<DWORD>(parsed);
    }
    return findProcessId(argumentValue(args, L"--process"));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2) {
            printUsage();
            return 2;
        }
        const std::wstring command = argv[1];
        const std::vector<std::wstring> args(argv + 2, argv + argc);

        if (command == L"scan") {
            const DWORD pid = pidFromArguments(args);
            const auto target = parseInt32(argumentValue(args, L"--value"));
            const auto statePath = std::filesystem::path(argumentValue(args, L"--state"));
            auto process = openProcessForRead(pid);
            std::cout << "Scanning PID " << pid << " for int32 " << target << "...\n";
            auto matches = scanExactInt32(process.value, target, hasFlag(args, L"--unaligned"));
            ScanState state{pid, getProcessStartTime(process.value), target, std::move(matches)};
            saveState(statePath, state);
            std::cout << "Candidates: " << state.addresses.size() << "\n";
            printCandidates(process.value, state, 20);
            return 0;
        }

        if (command == L"next") {
            const auto statePath = std::filesystem::path(argumentValue(args, L"--state"));
            const auto target = parseInt32(argumentValue(args, L"--value"));
            auto state = loadState(statePath);
            auto process = openProcessForRead(state.pid);
            validateSameProcess(process.value, state);
            const auto before = state.addresses.size();
            state.addresses = filterExactInt32(process.value, state.addresses, target);
            state.value = target;
            saveState(statePath, state);
            std::cout << "Candidates: " << before << " -> " << state.addresses.size() << "\n";
            printCandidates(process.value, state, 20);
            return 0;
        }

        if (command == L"decreased-i32" || command == L"decreased-f32") {
            const auto statePath = std::filesystem::path(argumentValue(args, L"--state"));
            auto state = loadState(statePath);
            auto process = openProcessForRead(state.pid);
            validateSameProcess(process.value, state);
            const auto before = state.addresses.size();
            const auto interpretation = command == L"decreased-i32"
                ? ValueInterpretation::Int32
                : ValueInterpretation::Float32;
            state.addresses = filterDecreased(process.value, state, interpretation);
            saveState(statePath, state);
            std::cout << "Candidates: " << before << " -> " << state.addresses.size() << "\n";
            printCurrentValues(process.value, state, 50);
            return 0;
        }

        if (command == L"watch") {
            const auto statePath = std::filesystem::path(argumentValue(args, L"--state"));
            const auto state = loadState(statePath);
            watchForWrite(state);
            return 0;
        }

        if (command == L"override-r8") {
            const DWORD pid = static_cast<DWORD>(parseSize(argumentValue(args, L"--pid")));
            const auto address = parseUint64(argumentValue(args, L"--address"));
            const auto matchRdx = parseUint64(argumentValue(args, L"--match-rdx"));
            const auto from = parseInt32(argumentValue(args, L"--from"));
            const auto to = parseInt32(argumentValue(args, L"--to"));
            overrideR8OnCall(pid, address, matchRdx, from, to);
            return 0;
        }

        if (command == L"set-coal") {
            const auto statePath = std::filesystem::path(argumentValue(args, L"--state"));
            const auto target = parseInt32(argumentValue(args, L"--target"));
            const auto state = loadState(statePath);
            setCoalTarget(state, target);
            return 0;
        }

        if (command == L"peek") {
            const DWORD pid = pidFromArguments(args);
            const auto address = parseUint64(argumentValue(args, L"--address"));
            const auto size = parseSize(argumentValue(args, L"--bytes"));
            auto process = openProcessForRead(pid);
            printMemory(process.value, address, size);
            return 0;
        }

        if (command == L"list") {
            const auto statePath = std::filesystem::path(argumentValue(args, L"--state"));
            const auto limitText = optionalArgumentValue(args, L"--limit");
            const auto limit = limitText ? parseSize(*limitText) : 50;
            const auto state = loadState(statePath);
            auto process = openProcessForRead(state.pid);
            validateSameProcess(process.value, state);
            std::cout << "PID: " << state.pid << "  value: " << state.value
                      << "  candidates: " << state.addresses.size() << "\n";
            printCandidates(process.value, state, limit);
            return 0;
        }

        printUsage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
