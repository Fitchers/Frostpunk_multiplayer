#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <functional>
#include "ConnectionSession.h"
#include "ResourceReader.h"
#include "CityVitals.h"
#include "SaveSync.h"
#include "../LaunchControl.h"
#include "../OverlayControl.h"
#include "../SessionControl.h"
#include "../ClockSync.h"

namespace {

constexpr std::uint32_t kProtocolMagic = 0x31504246;  // "FBP1"
constexpr std::uint16_t kProtocolVersion = 16;
constexpr int kChannel = 17;
constexpr int kSendUnreliable = 0;
constexpr int kSendReliable = 2;
constexpr std::uint32_t kMaxPacketSize = 64 * 1024;
using LogSink = std::function<void(const std::string&)>;
void consoleLog(const std::string& text) { std::cout << text << '\n'; }

enum class MessageType : std::uint16_t {
    hello = 1,
    helloAck = 2,
    heartbeat = 3,
    citySnapshot = 4,
    chat = 5,
    startPrepare = 6,
    startReady = 7,
    startCommit = 8,
    startResult = 9,
    transferRequest = 10,
    transferResult = 11,
    pauseState = 12,
    sessionState = 13,
    startGo = 14,
    speedRequest = 15,
    speedState = 16,
    checkpoint = 17,
};

#pragma pack(push, 1)
struct PacketHeader {
    std::uint32_t magic = kProtocolMagic;
    std::uint16_t version = kProtocolVersion;
    MessageType type = MessageType::hello;
    std::uint32_t sequence = 0;
    std::uint32_t payloadBytes = 0;
    std::uint64_t senderSteamId = 0;
    std::uint64_t timestampMs = 0;
};

struct HelloPayload {
    char playerName[64]{};
    char cityName[64]{};
    std::uint64_t sessionStartedAtMs = 0;
};

struct HeartbeatPayload {
    std::uint64_t uptimeMs = 0;
};
struct StartPayload {
    std::uint32_t request = 0;
    std::int32_t status = 0;
    std::int32_t mapIndex = -1;
    frostlaunch::Difficulty difficulty{};
};

struct TransferRequestPayload {
    std::uint32_t id = 0;
    std::int32_t resource = 0;
    std::int32_t amount = 0;
};

struct TransferResultPayload {
    std::uint32_t id = 0;
    std::int32_t status = 0;
    std::int32_t applied = 0;
};

struct PausePayload {
    std::uint32_t request = 0;
    std::int32_t paused = 1;
    std::uint32_t delayMs = 0;
};
struct SpeedPayload {
    std::uint32_t revision = 0;
    std::int32_t mode = -1;
};

struct SessionStatePayload {
    std::uint32_t request = 0;
    std::int32_t loaded = 0;
    std::int32_t paused = 1;
    std::int64_t gameTimeMs = 0;
};

struct CitySnapshotPayload {
    std::int32_t coal = 0;
    std::int32_t wood = 0;
    std::int32_t steel = 0;
    std::int32_t steamCores = 0;
    std::int32_t rawFood = 0;
    std::int32_t foodRations = 0;
    std::int32_t population = 0;
    std::int32_t temperature = 0;
    std::int32_t hope = -1;
    std::int32_t discontent = -1;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 32);
static_assert(sizeof(CitySnapshotPayload) == 40);

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::filesystem::path executableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) fail("Could not resolve executable path.");
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::uint64_t parseSteamId(const std::wstring& text) {
    std::size_t used = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(text, &used, 10);
    } catch (...) {
        fail("SteamID64 must be a 17-digit unsigned number.");
    }
    if (used != text.size() || value < 76561197960265728ULL) {
        fail("SteamID64 is invalid; AppID 480 is not a peer SteamID.");
    }
    return static_cast<std::uint64_t>(value);
}

std::uint64_t timestampMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

struct WinHandle {
    HANDLE value = nullptr;
    WinHandle() = default;
    explicit WinHandle(HANDLE handle) : value(handle) {}
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    ~WinHandle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

template <typename T>
std::optional<T> readProcess(HANDLE process, std::uintptr_t address) {
    T value{};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(address), &value,
                           sizeof(value), &bytesRead) || bytesRead != sizeof(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<DWORD> frostpunkProcessId() {
    WinHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return std::nullopt;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.value, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"Frostpunk.exe") == 0) {
                return entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot.value, &entry));
    }
    return std::nullopt;
}

std::optional<std::uintptr_t> frostpunkModuleBase(DWORD processId) {
    WinHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                                processId));
    if (!snapshot) return std::nullopt;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot.value, &module)) {
        do {
            if (_wcsicmp(module.szModule, L"Frostpunk.exe") == 0) {
                return reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
            }
        } while (Module32NextW(snapshot.value, &module));
    }
    return std::nullopt;
}

std::optional<CitySnapshotPayload> readLocalCity(DWORD selectedProcessId) {
    const auto processId = selectedProcessId
        ? std::optional<DWORD>(selectedProcessId)
        : frostpunkProcessId();
    if (!processId) return std::nullopt;
    const auto moduleBase = frostpunkModuleBase(*processId);
    if (!moduleBase) return std::nullopt;
    WinHandle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                                  *processId));
    if (!process) return std::nullopt;

    const auto resources = frostbridge::resources::readSnapshot(
        [&](std::uintptr_t address, void* destination, std::size_t size) {
            SIZE_T bytesRead = 0;
            return ReadProcessMemory(process.value, reinterpret_cast<const void*>(address),
                                     destination, size, &bytesRead) && bytesRead == size;
        }, *moduleBase);
    if (!resources) return std::nullopt;

    CitySnapshotPayload city{};
    const auto vitals = frostbridge::vitals::readSnapshot(
        [&](std::uintptr_t address, void* destination, std::size_t size) {
            SIZE_T bytesRead = 0;
            return ReadProcessMemory(process.value, reinterpret_cast<const void*>(address),
                destination, size, &bytesRead) && bytesRead == size;
        }, *moduleBase);
    city.hope = vitals.hope;
    city.discontent = vitals.discontent;
    city.coal = resources->coal;
    city.wood = resources->wood;
    city.steel = resources->steel;
    city.steamCores = resources->steamCores;
    city.rawFood = resources->rawFood;
    city.foodRations = resources->foodRations;
    city.population = -1;  // Not recovered yet.
    city.temperature = -1; // Not recovered yet.
    return city;
}

class OverlayBridge {
public:
    OverlayBridge() = default;
    OverlayBridge(const OverlayBridge&) = delete;
    OverlayBridge& operator=(const OverlayBridge&) = delete;
    ~OverlayBridge() { close(); }

    void open(DWORD processId, const std::string& localName) {
        close();
        if (!processId) return;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(frostoverlay::Control), frostoverlay::name(processId).c_str());
        if (!mapping_) return;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        control_ = static_cast<frostoverlay::Control*>(MapViewOfFile(
            mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(frostoverlay::Control)));
        if (!control_) { CloseHandle(mapping_); mapping_ = nullptr; return; }
        if (created) {
            ZeroMemory(control_, sizeof(*control_));
            control_->localHope = control_->localDiscontent = -1;
            control_->peerHope = control_->peerDiscontent = -1;
            control_->layoutVersion = frostoverlay::version;
            MemoryBarrier();
            control_->signature = frostoverlay::magic;
        }
        if (control_->signature != frostoverlay::magic ||
            control_->layoutVersion != frostoverlay::version) {
            close();
            return;
        }
        writeNames(localName, "");
        InterlockedExchange(&control_->connection,
            static_cast<LONG>(frostoverlay::Connection::offline));
        InterlockedExchange(&control_->transferBusy, 0);
        lastOutgoing_ = InterlockedCompareExchange(&control_->outgoingSequence, 0, 0);
    }

    void close() {
        if (control_) {
            InterlockedExchange(&control_->connection,
                static_cast<LONG>(frostoverlay::Connection::offline));
            InterlockedExchange(&control_->transferBusy, 0);
            UnmapViewOfFile(control_);
        }
        if (mapping_) CloseHandle(mapping_);
        control_ = nullptr;
        mapping_ = nullptr;
    }

    bool available() const { return control_ != nullptr; }

    void connected(const std::string& localName, const std::string& peerName) {
        if (!control_) return;
        writeNames(localName, peerName);
        InterlockedExchange(&control_->connection,
            static_cast<LONG>(frostoverlay::Connection::connected));
    }

    void local(const CitySnapshotPayload& city) {
        if (!control_) return;
        write(&control_->local, city);
        InterlockedExchange(&control_->localHope, city.hope);
        InterlockedExchange(&control_->localDiscontent, city.discontent);
    }
    void peer(const CitySnapshotPayload& city) {
        if (!control_) return;
        write(&control_->peer, city);
        InterlockedExchange(&control_->peerHope, city.hope);
        InterlockedExchange(&control_->peerDiscontent, city.discontent);
    }

    struct Request { LONG resource; LONG amount; };
    std::optional<Request> outgoing() {
        if (!control_) return std::nullopt;
        const LONG sequence = InterlockedCompareExchange(&control_->outgoingSequence, 0, 0);
        if (sequence == lastOutgoing_) return std::nullopt;
        MemoryBarrier();
        lastOutgoing_ = sequence;
        return Request{control_->outgoingResource, control_->outgoingAmount};
    }

    LONG apply(LONG resource, LONG delta) {
        if (!control_ || !InterlockedCompareExchange(&control_->modReady, 0, 0) ||
            !frostoverlay::validResource(resource) || !delta) return 0;
        control_->applyResource = resource;
        control_->applyDelta = delta;
        MemoryBarrier();
        return InterlockedIncrement(&control_->applyRequestSequence);
    }

    std::optional<LONG> applied(LONG sequence) const {
        if (!control_ || !sequence ||
            InterlockedCompareExchange(&control_->applyResultSequence, 0, 0) != sequence)
            return std::nullopt;
        MemoryBarrier();
        return control_->applyResult;
    }

    void session(LONG local, LONG peer, LONG skew) {
        if (!control_) return;
        InterlockedExchange(&control_->localState, local);
        InterlockedExchange(&control_->peerState, peer);
        InterlockedExchange(&control_->skewSeconds, skew);
    }

    void notify(const std::string& utf8, bool transfer = false) {
        if (!control_) return;
        wchar_t text[160]{};
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, text,
                            static_cast<int>(std::size(text)));
        wcsncpy_s(control_->notification, text, _TRUNCATE);
        MemoryBarrier();
        InterlockedIncrement(&control_->notificationSequence);
        if (transfer) {
            InterlockedIncrement(&control_->historySequence);
            MemoryBarrier();
            for (int i = 2; i > 0; --i)
                wcsncpy_s(control_->history[i], control_->history[i - 1], _TRUNCATE);
            wcsncpy_s(control_->history[0], text, _TRUNCATE);
            MemoryBarrier();
            InterlockedIncrement(&control_->historySequence);
        }
    }

    void busy(bool value) {
        if (control_) InterlockedExchange(&control_->transferBusy, value ? 1 : 0);
    }

private:
    static void copy(char (&target)[64], const std::string& text) {
        ZeroMemory(target, sizeof(target));
        std::memcpy(target, text.data(), (std::min)(text.size(), sizeof(target) - 1));
    }
    void writeNames(const std::string& localName, const std::string& peerName) {
        InterlockedIncrement(&control_->namesSequence);
        MemoryBarrier();
        copy(control_->localName, localName);
        copy(control_->peerName, peerName);
        MemoryBarrier();
        InterlockedIncrement(&control_->namesSequence);
    }
    static void write(frostoverlay::Snapshot* target, const CitySnapshotPayload& city) {
        if (!target) return;
        frostoverlay::Values values{{city.coal, city.wood, city.steel,
            city.steamCores, city.rawFood, city.foodRations}};
        frostoverlay::writeSnapshot(*target, values);
    }
    HANDLE mapping_ = nullptr;
    frostoverlay::Control* control_ = nullptr;
    LONG lastOutgoing_ = 0;
};

class GameSessionBridge {
public:
    struct State {
        bool available = false;
        bool loaded = false;
        bool paused = true;
        LONG64 gameTimeMs = 0;
        LONG generation = 0;
    };

    ~GameSessionBridge() { close(); }
    void open(DWORD processId) {
        close();
        if (!processId) return;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(frostsession::Control), frostsession::name(processId).c_str());
        if (!mapping_) return;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        control_ = static_cast<frostsession::Control*>(MapViewOfFile(
            mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(frostsession::Control)));
        if (!control_) { CloseHandle(mapping_); mapping_ = nullptr; return; }
        if (created) {
            ZeroMemory(control_, sizeof(*control_));
            control_->layoutVersion = frostsession::version;
            control_->paused = 1;
            control_->localPauseValue = 1;
            control_->pauseCommandValue = 1;
            control_->localSpeedValue = -1;
            control_->currentSpeed = -1;
            control_->speedCommandValue = -1;
            MemoryBarrier();
            control_->signature = frostsession::magic;
        }
        if (control_->signature != frostsession::magic ||
            control_->layoutVersion != frostsession::version) {
            close();
            return;
        }
        lastPauseEvent_ = InterlockedCompareExchange(
            &control_->localPauseSequence, 0, 0);
        lastSpeedEvent_ = InterlockedCompareExchange(&control_->localSpeedSequence, 0, 0);
    }
    void close() {
        if (control_) UnmapViewOfFile(control_);
        if (mapping_) CloseHandle(mapping_);
        control_ = nullptr;
        mapping_ = nullptr;
    }
    State state() const {
        if (!control_ || !InterlockedCompareExchange(&control_->modReady, 0, 0)) return {};
        State result{};
        result.available = true;
        result.loaded = InterlockedCompareExchange(&control_->gameLoaded, 0, 0) != 0;
        result.paused = InterlockedCompareExchange(&control_->paused, 0, 0) != 0;
        result.gameTimeMs = InterlockedCompareExchange64(&control_->gameTimeMs, 0, 0);
        result.generation = InterlockedCompareExchange(&control_->loadGeneration, 0, 0);
        return result;
    }
    std::optional<bool> localPauseEvent() {
        if (!control_) return std::nullopt;
        const LONG sequence = InterlockedCompareExchange(
            &control_->localPauseSequence, 0, 0);
        if (sequence == lastPauseEvent_) return std::nullopt;
        lastPauseEvent_ = sequence;
        MemoryBarrier();
        return control_->localPauseValue != 0;
    }
    bool localPause() const {
        return control_ && InterlockedCompareExchange(&control_->localPauseValue, 0, 0) != 0;
    }
    LONG commandPause(bool paused) {
        if (!control_ || !InterlockedCompareExchange(&control_->modReady, 0, 0)) return 0;
        const LONG current = InterlockedCompareExchange(&control_->pauseCommandSequence, 0, 0);
        const LONG generation = InterlockedCompareExchange(&control_->loadGeneration, 0, 0);
        if (current && generation == lastCommandGeneration_ && (control_->pauseCommandValue != 0) == paused) return current;
        lastCommandGeneration_ = generation;
        InterlockedExchange(&control_->pauseCommandValue, paused ? 1 : 0);
        MemoryBarrier();
        return InterlockedIncrement(&control_->pauseCommandSequence);
    }
    int speed() const { return control_ ? control_->currentSpeed : -1; }
    std::optional<int> localSpeedEvent() {
        if (!control_) return std::nullopt;
        const LONG sequence = InterlockedCompareExchange(&control_->localSpeedSequence, 0, 0);
        if (sequence == lastSpeedEvent_) return std::nullopt;
        lastSpeedEvent_ = sequence;
        MemoryBarrier();
        const int mode = control_->localSpeedValue;
        return mode >= 0 && mode <= 2 ? std::optional<int>(mode) : std::nullopt;
    }
    void commandSpeed(int mode) {
        if (!control_ || mode < 0 || mode > 2) return;
        InterlockedExchange(&control_->speedCommandValue, mode);
        MemoryBarrier();
        InterlockedIncrement(&control_->speedCommandSequence);
    }

private:
    HANDLE mapping_ = nullptr;
    frostsession::Control* control_ = nullptr;
    LONG lastPauseEvent_ = 0;
    LONG lastCommandGeneration_ = -1;
    LONG lastSpeedEvent_ = 0;
};

class SteamApi {
public:
    explicit SteamApi(const std::filesystem::path& directory) {
        const auto dllPath = directory / L"steam_api64.dll";
        module_ = LoadLibraryW(dllPath.c_str());
        if (!module_) {
            fail("steam_api64.dll is missing next to FrostBridgeNet.exe.");
        }

        init_ = load<Init>("SteamAPI_Init");
        shutdown_ = load<Shutdown>("SteamAPI_Shutdown");
        runCallbacks_ = load<RunCallbacks>("SteamAPI_RunCallbacks");
        steamUser_ = load<Accessor>("SteamAPI_SteamUser_v023");
        steamFriends_ = load<Accessor>("SteamAPI_SteamFriends_v017");
        steamNetworking_ = load<Accessor>("SteamAPI_SteamNetworking_v006");
        loggedOn_ = load<LoggedOn>("SteamAPI_ISteamUser_BLoggedOn");
        getSteamId_ = load<GetSteamId>("SteamAPI_ISteamUser_GetSteamID");
        getPersonaName_ = load<GetPersonaName>("SteamAPI_ISteamFriends_GetPersonaName");
        send_ = load<Send>("SteamAPI_ISteamNetworking_SendP2PPacket");
        available_ = load<Available>("SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
        read_ = load<Read>("SteamAPI_ISteamNetworking_ReadP2PPacket");
        accept_ = load<Accept>("SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser");
        close_ = load<Close>("SteamAPI_ISteamNetworking_CloseP2PSessionWithUser");
        allowRelay_ = load<AllowRelay>("SteamAPI_ISteamNetworking_AllowP2PPacketRelay");

        if (!init_()) {
            fail("SteamAPI_Init failed. Start Steam and verify steam_appid.txt contains 480.");
        }
        initialized_ = true;
        user_ = steamUser_();
        friends_ = steamFriends_();
        networking_ = steamNetworking_();
        if (!user_ || !friends_ || !networking_) fail("Steam interfaces are unavailable.");
        if (!loggedOn_(user_)) fail("Steam is initialized, but the user is not logged on.");
        allowRelay_(networking_, true);
    }

    SteamApi(const SteamApi&) = delete;
    SteamApi& operator=(const SteamApi&) = delete;

    ~SteamApi() {
        if (initialized_) shutdown_();
        if (module_) FreeLibrary(module_);
    }

    std::uint64_t localSteamId() const { return getSteamId_(user_); }
    const char* personaName() const { return getPersonaName_(friends_); }
    void runCallbacks() const { runCallbacks_(); }
    bool accept(std::uint64_t peer) const { return accept_(networking_, peer); }
    bool close(std::uint64_t peer) const { return close_(networking_, peer); }

    bool send(std::uint64_t peer, const void* data, std::uint32_t size,
              bool reliable) const {
        return send_(networking_, peer, data, size,
                     reliable ? kSendReliable : kSendUnreliable, kChannel);
    }

    bool available(std::uint32_t& size) const {
        return available_(networking_, &size, kChannel);
    }

    bool read(void* data, std::uint32_t capacity, std::uint32_t& size,
              std::uint64_t& sender) const {
        return read_(networking_, data, capacity, &size, &sender, kChannel);
    }

private:
    using Init = bool(__cdecl*)();
    using Shutdown = void(__cdecl*)();
    using RunCallbacks = void(__cdecl*)();
    using Accessor = void*(__cdecl*)();
    using LoggedOn = bool(__cdecl*)(void*);
    using GetSteamId = std::uint64_t(__cdecl*)(void*);
    using GetPersonaName = const char*(__cdecl*)(void*);
    using Send = bool(__cdecl*)(void*, std::uint64_t, const void*, std::uint32_t,
                                int, int);
    using Available = bool(__cdecl*)(void*, std::uint32_t*, int);
    using Read = bool(__cdecl*)(void*, void*, std::uint32_t, std::uint32_t*,
                                std::uint64_t*, int);
    using Accept = bool(__cdecl*)(void*, std::uint64_t);
    using Close = bool(__cdecl*)(void*, std::uint64_t);
    using AllowRelay = bool(__cdecl*)(void*, bool);

    template <typename T>
    T load(const char* name) {
        const auto function = reinterpret_cast<T>(GetProcAddress(module_, name));
        if (!function) fail(std::string("Missing Steamworks export: ") + name);
        return function;
    }

    HMODULE module_ = nullptr;
    bool initialized_ = false;
    void* user_ = nullptr;
    void* friends_ = nullptr;
    void* networking_ = nullptr;
    Init init_ = nullptr;
    Shutdown shutdown_ = nullptr;
    RunCallbacks runCallbacks_ = nullptr;
    Accessor steamUser_ = nullptr;
    Accessor steamFriends_ = nullptr;
    Accessor steamNetworking_ = nullptr;
    LoggedOn loggedOn_ = nullptr;
    GetSteamId getSteamId_ = nullptr;
    GetPersonaName getPersonaName_ = nullptr;
    Send send_ = nullptr;
    Available available_ = nullptr;
    Read read_ = nullptr;
    Accept accept_ = nullptr;
    Close close_ = nullptr;
    AllowRelay allowRelay_ = nullptr;
};

class PacketTransport {
public:
    virtual ~PacketTransport() = default;
    virtual const char* name() const = 0;
    virtual std::uint64_t localId() const = 0;
    virtual std::uint64_t expectedPeerId() const = 0;
    virtual void pump() = 0;
    virtual bool send(const void* data, std::uint32_t size, bool reliable) = 0;
    virtual bool receive(std::vector<std::uint8_t>& packet,
                         std::uint64_t& sender) = 0;
    virtual void close() = 0;
    virtual bool isClosed() const { return false; }
};

class SteamTransport final : public PacketTransport {
public:
    SteamTransport(SteamApi& steam, std::uint64_t peer)
        : steam_(steam), peer_(peer) {
        steam_.accept(peer_);
    }

    const char* name() const override { return "steam"; }
    std::uint64_t localId() const override { return steam_.localSteamId(); }
    std::uint64_t expectedPeerId() const override { return peer_; }

    void pump() override {
        steam_.runCallbacks();
        steam_.accept(peer_);
    }

    bool send(const void* data, std::uint32_t size, bool reliable) override {
        return steam_.send(peer_, data, size, reliable);
    }

    bool receive(std::vector<std::uint8_t>& packet,
                 std::uint64_t& sender) override {
        std::uint32_t size = 0;
        if (!steam_.available(size)) return false;
        if (size < sizeof(PacketHeader) || size > kMaxPacketSize) {
            std::vector<std::uint8_t> rejected(size);
            std::uint32_t received = 0;
            steam_.read(rejected.data(), size, received, sender);
            return false;
        }
        packet.resize(size);
        std::uint32_t received = 0;
        if (!steam_.read(packet.data(), size, received, sender)) return false;
        packet.resize(received);
        return true;
    }

    void close() override { steam_.close(peer_); }

private:
    SteamApi& steam_;
    std::uint64_t peer_ = 0;
};

class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            fail("WSAStartup failed.");
        }
    }
    ~SocketRuntime() { WSACleanup(); }
};

class SocketHandle {
public:
    SocketHandle() = default;
    explicit SocketHandle(SOCKET value) : value_(value) {}
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    SocketHandle(SocketHandle&& other) noexcept : value_(other.release()) {}
    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    ~SocketHandle() { reset(); }
    SOCKET get() const { return value_; }
    explicit operator bool() const { return value_ != INVALID_SOCKET; }
    SOCKET release() {
        const SOCKET result = value_;
        value_ = INVALID_SOCKET;
        return result;
    }
    void reset(SOCKET value = INVALID_SOCKET) {
        if (value_ != INVALID_SOCKET) closesocket(value_);
        value_ = value;
    }

private:
    SOCKET value_ = INVALID_SOCKET;
};

std::uint16_t parsePort(const std::wstring& text) {
    std::size_t used = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(text, &used, 10);
    } catch (...) {
        fail("Port must be a number from 1 to 65535.");
    }
    if (used != text.size() || value == 0 || value > 65535) {
        fail("Port must be a number from 1 to 65535.");
    }
    return static_cast<std::uint16_t>(value);
}

DWORD parseProcessId(const std::wstring& text) {
    std::size_t used = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(text, &used, 10);
    } catch (...) {
        fail("PID must be a positive process id.");
    }
    if (used != text.size() || value == 0) fail("PID must be a positive process id.");
    return static_cast<DWORD>(value);
}

struct LanEndpoint {
    std::string host;
    std::uint16_t port = 27020;
};

LanEndpoint parseEndpoint(const std::wstring& text) {
    const std::string value = narrow(text);
    if (value.empty()) fail("LAN address is empty.");
    LanEndpoint endpoint{};
    const auto colon = value.rfind(':');
    if (colon == std::string::npos) {
        endpoint.host = value;
    } else {
        endpoint.host = value.substr(0, colon);
        endpoint.port = parsePort(std::wstring(text.begin() + static_cast<std::ptrdiff_t>(colon + 1),
                                               text.end()));
    }
    if (endpoint.host.empty()) fail("LAN host is empty.");
    return endpoint;
}

class LanTransport final : public PacketTransport {
public:
    static std::unique_ptr<LanTransport> host(std::uint16_t port,
            const std::atomic<bool>* cancel = nullptr, LogSink log = consoleLog) {
        auto transport = std::unique_ptr<LanTransport>(new LanTransport());
        SocketHandle listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (!listener) fail("Could not create the LAN listen socket.");

        BOOL exclusive = TRUE;
        setsockopt(listener.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (bind(listener.get(), reinterpret_cast<const sockaddr*>(&address),
                 sizeof(address)) == SOCKET_ERROR) {
            fail("Could not bind LAN port " + std::to_string(port) +
                 " (Winsock " + std::to_string(WSAGetLastError()) + ").");
        }
        if (listen(listener.get(), 1) == SOCKET_ERROR) fail("LAN listen failed.");
        log("[lan] Hosting on port " + std::to_string(port) + "; waiting for one player...");
        u_long nonBlocking = 1;
        if (ioctlsocket(listener.get(), FIONBIO, &nonBlocking) != 0) fail("LAN listen configuration failed.");
        sockaddr_storage remote{};
        int remoteSize = sizeof(remote);
        SocketHandle accepted;
        for (;;) {
            if (cancel && cancel->load()) fail("Connection cancelled.");
            accepted.reset(accept(listener.get(), reinterpret_cast<sockaddr*>(&remote), &remoteSize));
            if (accepted) break;
            if (WSAGetLastError() != WSAEWOULDBLOCK) fail("LAN accept failed.");
            Sleep(25);
        }
        transport->socket_ = std::move(accepted);
        transport->configureConnectedSocket();
        log("[lan] Player connected.");
        return transport;
    }

    static std::unique_ptr<LanTransport> join(const LanEndpoint& endpoint,
            const std::atomic<bool>* cancel = nullptr, LogSink log = consoleLog) {
        auto transport = std::unique_ptr<LanTransport>(new LanTransport());
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        // Embedded UI accepts numeric addresses: no unbounded DNS call on close.
        if (cancel) hints.ai_flags = AI_NUMERICHOST;
        addrinfo* rawAddresses = nullptr;
        const std::string port = std::to_string(endpoint.port);
        const int lookup = getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints,
                                       &rawAddresses);
        if (lookup != 0) fail("Could not resolve LAN address " + endpoint.host + ".");
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(rawAddresses,
                                                                    freeaddrinfo);
        for (const addrinfo* address = addresses.get(); address; address = address->ai_next) {
            if (cancel && cancel->load()) fail("Connection cancelled.");
            SocketHandle candidate(socket(address->ai_family, address->ai_socktype,
                                          address->ai_protocol));
            if (!candidate) continue;
            u_long nonBlocking = 1;
            if (ioctlsocket(candidate.get(), FIONBIO, &nonBlocking) != 0) continue;
            bool connected = ::connect(candidate.get(), address->ai_addr,
                        static_cast<int>(address->ai_addrlen)) == 0;
            if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
                const auto deadline = GetTickCount64() + 5000;
                while (GetTickCount64() < deadline) {
                    if (cancel && cancel->load()) fail("Connection cancelled.");
                    fd_set writable, errors;
                    FD_ZERO(&writable); FD_ZERO(&errors);
                    FD_SET(candidate.get(), &writable); FD_SET(candidate.get(), &errors);
                    timeval wait{0, 25000};
                    const int result = select(0, nullptr, &writable, &errors, &wait);
                    if (result == SOCKET_ERROR) break;
                    if (result > 0) {
                        int error = 0, length = sizeof(error);
                        connected = getsockopt(candidate.get(), SOL_SOCKET, SO_ERROR,
                            reinterpret_cast<char*>(&error), &length) == 0 && error == 0;
                        break;
                    }
                }
            }
            if (connected) {
                transport->socket_ = std::move(candidate);
                break;
            }
        }
        if (!transport->socket_) {
            fail("Could not connect to " + endpoint.host + ":" +
                 std::to_string(endpoint.port) + ".");
        }
        transport->configureConnectedSocket();
        log("[lan] Connected to " + endpoint.host + ':' + std::to_string(endpoint.port) + ".");
        return transport;
    }

    const char* name() const override { return "lan"; }
    std::uint64_t localId() const override { return localId_; }
    std::uint64_t expectedPeerId() const override { return 0; }

    void pump() override {
        if (!socket_ || closed_) return;
        std::uint8_t block[8192];
        for (;;) {
            const int received = recv(socket_.get(), reinterpret_cast<char*>(block),
                                      sizeof(block), 0);
            if (received > 0) {
                receiveBuffer_.insert(receiveBuffer_.end(), block, block + received);
                if (receiveBuffer_.size() > 4 * kMaxPacketSize) { closed_ = true; return; }
                continue;
            }
            if (received == 0) {
                closed_ = true;
                return;
            }
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) return;
            closed_ = true;
            return;
        }
    }

    bool send(const void* data, std::uint32_t size, bool) override {
        if (!socket_ || closed_ || size > kMaxPacketSize) return false;
        const std::uint32_t networkSize = htonl(size);
        std::vector<std::uint8_t> frame(sizeof(networkSize) + size);
        std::memcpy(frame.data(), &networkSize, sizeof(networkSize));
        std::memcpy(frame.data() + sizeof(networkSize), data, size);
        std::size_t sentTotal = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (sentTotal < frame.size()) {
            if (std::chrono::steady_clock::now() >= deadline) { closed_ = true; return false; }
            const int sent = ::send(socket_.get(),
                                    reinterpret_cast<const char*>(frame.data() + sentTotal),
                                    static_cast<int>(frame.size() - sentTotal), 0);
            if (sent > 0) {
                sentTotal += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                Sleep(1);
                continue;
            }
            closed_ = true;
            return false;
        }
        return true;
    }

    bool receive(std::vector<std::uint8_t>& packet,
                 std::uint64_t& sender) override {
        if (receiveBuffer_.size() < sizeof(std::uint32_t)) return false;
        std::uint32_t networkSize = 0;
        std::memcpy(&networkSize, receiveBuffer_.data(), sizeof(networkSize));
        const std::uint32_t size = ntohl(networkSize);
        if (size < sizeof(PacketHeader) || size > kMaxPacketSize) {
            closed_ = true;
            return false;
        }
        const std::size_t frameSize = sizeof(networkSize) + size;
        if (receiveBuffer_.size() < frameSize) return false;
        packet.assign(receiveBuffer_.begin() + sizeof(networkSize),
                      receiveBuffer_.begin() + frameSize);
        receiveBuffer_.erase(receiveBuffer_.begin(),
                             receiveBuffer_.begin() + frameSize);
        PacketHeader header{};
        std::memcpy(&header, packet.data(), sizeof(header));
        sender = header.senderSteamId;
        return true;
    }

    void close() override {
        if (socket_) shutdown(socket_.get(), SD_BOTH);
        socket_.reset();
        closed_ = true;
    }
    bool isClosed() const override { return closed_; }

private:
    LanTransport()
        : localId_((static_cast<std::uint64_t>(timestampMs()) << 16) ^
                   static_cast<std::uint64_t>(GetCurrentProcessId())) {}

    void configureConnectedSocket() {
        u_long nonBlocking = 1;
        if (ioctlsocket(socket_.get(), FIONBIO, &nonBlocking) == SOCKET_ERROR) {
            fail("Could not configure the LAN socket.");
        }
        BOOL noDelay = TRUE;
        setsockopt(socket_.get(), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    }

    SocketRuntime runtime_;
    SocketHandle socket_;
    std::vector<std::uint8_t> receiveBuffer_;
    std::uint64_t localId_ = 0;
    bool closed_ = false;
};

template <typename Payload>
std::vector<std::uint8_t> makePacket(MessageType type, std::uint32_t sequence,
                                     std::uint64_t sender, const Payload& payload) {
    PacketHeader header{};
    header.type = type;
    header.sequence = sequence;
    header.payloadBytes = sizeof(Payload);
    header.senderSteamId = sender;
    header.timestampMs = timestampMs();
    std::vector<std::uint8_t> packet(sizeof(header) + sizeof(payload));
    std::memcpy(packet.data(), &header, sizeof(header));
    std::memcpy(packet.data() + sizeof(header), &payload, sizeof(payload));
    return packet;
}

std::vector<std::uint8_t> makeTextPacket(MessageType type, std::uint32_t sequence,
                                         std::uint64_t sender,
                                         const std::string& text) {
    PacketHeader header{};
    header.type = type;
    header.sequence = sequence;
    header.payloadBytes = static_cast<std::uint32_t>(text.size());
    header.senderSteamId = sender;
    header.timestampMs = timestampMs();
    std::vector<std::uint8_t> packet(sizeof(header) + text.size());
    std::memcpy(packet.data(), &header, sizeof(header));
    if (!text.empty()) std::memcpy(packet.data() + sizeof(header), text.data(), text.size());
    return packet;
}

class BridgeSession {
public:
    BridgeSession(PacketTransport& transport, std::string playerName,
                  std::string cityName, bool watchFrostpunk,
                  DWORD frostpunkProcessId, bool host = false, LogSink output = consoleLog)
        : transport_(transport), local_(transport.localId()),
          expectedPeer_(transport.expectedPeerId()),
          playerName_(std::move(playerName)), cityName_(std::move(cityName)),
          watchFrostpunk_(watchFrostpunk),
          frostpunkProcessId_(frostpunkProcessId), host_(host),
          sessionStartedAtMs_(timestampMs()),
          started_(std::chrono::steady_clock::now()), output_(std::move(output)) {
        overlay_.open(frostpunkProcessId_, playerName_);
        gameSession_.open(frostpunkProcessId_);
        roleDetermined_=dynamic_cast<SteamTransport*>(&transport_)==nullptr;
        wchar_t appData[32768]{};
        const DWORD appDataLength=GetEnvironmentVariableW(L"APPDATA",appData,static_cast<DWORD>(std::size(appData)));
        if(appDataLength && appDataLength<std::size(appData)) {
            saves_=std::make_unique<frostsave::Sync>(frostpunkProcessId_,playerName_,host_,
                std::filesystem::path(appData)/L"11bitstudios"/L"Frostpunk"/L"Default"/L"saves");
            saves_->send=[this](const frostsave::Packet& packet) {
                sendPacket(makePacket(MessageType::checkpoint,++sequence_,local_,packet),true);
            };
            saves_->log=[this](const std::string& message) { print(message); overlay_.notify(message); };
            saves_->kick=[this] { transport_.close(); };
            saves_->state=[this] { auto s=gameSession_.state(); return frostsave::State{s.loaded,s.paused,s.generation}; };
            saves_->trading=[this] { return outgoingTransfer_.stage!=TransferStage::none || incomingTransfer_.active; };
        }
        nextTransferId_ = static_cast<std::uint32_t>(timestampMs()) ^
                          static_cast<std::uint32_t>(local_);
    }

    void enqueue(std::string line) {
        std::lock_guard lock(queueMutex_);
        if (commands_.size() < 256) commands_.push(std::move(line));
        queueChanged_.notify_one();
    }
    void requestStop() {
        running_.store(false);
        queueChanged_.notify_one();
    }
    void runEmbedded() { networkLoop(); }

    void run() {
        print("Commands: status COAL WOOD STEEL CORES RAW_FOOD FOOD_RATIONS POPULATION TEMPERATURE | chat TEXT | hello | quit");
        worker_ = std::thread([this] {
            try { networkLoop(); }
            catch (const std::exception& error) {
                print(std::string("error: ") + error.what());
                transport_.close();
            }
        });

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "quit" || line == "exit") break;
            {
                std::lock_guard lock(queueMutex_);
                commands_.push(std::move(line));
            }
            queueChanged_.notify_one();
        }

        running_.store(false, std::memory_order_release);
        queueChanged_.notify_one();
        worker_.join();
    }

private:
    enum class TransferStage { none, debit, awaitingPeer, refund };
    struct OutgoingTransfer {
        TransferStage stage = TransferStage::none;
        std::uint32_t id = 0;
        LONG resource = 0;
        LONG amount = 0;
        LONG applySequence = 0;
        LONG refundAmount = 0;
        int retries = 0;
        std::chrono::steady_clock::time_point deadline{};
    } outgoingTransfer_;
    struct IncomingTransfer {
        bool active = false;
        TransferRequestPayload request{};
        LONG applySequence = 0;
    } incomingTransfer_;

    struct LaunchState { LONG state = frostlaunch::unavailable; LONG mapIndex = -1; frostlaunch::Difficulty difficulty{}; };
    LaunchState localLaunch() {
        if (!frostpunkProcessId_) return {};
        WinHandle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
            frostlaunch::name(frostpunkProcessId_).c_str()));
        if (!mapping) return {};
        auto* control = static_cast<frostlaunch::Control*>(MapViewOfFile(
            mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(frostlaunch::Control)));
        if (!control) return {};
        LaunchState result{};
        if (control->signature == frostlaunch::magic &&
            control->layoutVersion == frostlaunch::version) {
            result.state = InterlockedCompareExchange(&control->state, 0, 0);
            result.mapIndex = InterlockedCompareExchange(&control->mapIndex, 0, 0);
            if (result.state == frostlaunch::prepared) result.difficulty = control->difficulty;
        }
        UnmapViewOfFile(control);
        return result;
    }

    LONG requestLocalPrepare(LONG mapIndex) {
        if (!frostpunkProcessId_) return frostlaunch::unavailable;
        WinHandle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
            frostlaunch::name(frostpunkProcessId_).c_str()));
        if (!mapping) return frostlaunch::unavailable;
        auto* control = static_cast<frostlaunch::Control*>(MapViewOfFile(
            mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(frostlaunch::Control)));
        if (!control) return frostlaunch::unavailable;
        LONG state = frostlaunch::unavailable;
        if (control->signature == frostlaunch::magic &&
            control->layoutVersion == frostlaunch::version) {
            control->mapIndex = mapIndex;
            control->difficulty = selectedDifficulty_;
            MemoryBarrier();
            state = InterlockedCompareExchange(&control->state,
                frostlaunch::prepareRequested, frostlaunch::ready);
            if (state == frostlaunch::ready) state = frostlaunch::prepareRequested;
        }
        UnmapViewOfFile(control);
        return state;
    }

    LONG requestLocalCommit() {
        if (!frostpunkProcessId_) return frostlaunch::unavailable;
        WinHandle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
            frostlaunch::name(frostpunkProcessId_).c_str()));
        if (!mapping) return frostlaunch::unavailable;
        auto* control = static_cast<frostlaunch::Control*>(MapViewOfFile(
            mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(frostlaunch::Control)));
        if (!control) return frostlaunch::unavailable;
        LONG state = frostlaunch::unavailable;
        if (control->signature == frostlaunch::magic &&
            control->layoutVersion == frostlaunch::version) {
            state = InterlockedCompareExchange(&control->state,
                frostlaunch::commitRequested, frostlaunch::prepared);
            if (state == frostlaunch::prepared) state = frostlaunch::commitRequested;
        }
        UnmapViewOfFile(control);
        return state;
    }

    void resetLocalLaunch() {
        if (!frostpunkProcessId_ || startCommitted_) return;
        WinHandle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
            frostlaunch::name(frostpunkProcessId_).c_str()));
        if (!mapping) return;
        auto* control = static_cast<frostlaunch::Control*>(MapViewOfFile(
            mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(frostlaunch::Control)));
        if (control && control->signature == frostlaunch::magic &&
            control->layoutVersion == frostlaunch::version) {
            InterlockedExchange(&control->mapIndex, -1);
            InterlockedExchange(&control->state, frostlaunch::ready);
        }
        if(control) UnmapViewOfFile(control);
    }

    void resourceLine(const std::string& name, const CitySnapshotPayload& city) {
        std::ostringstream text;
        text << name << ": resources: coal " << city.coal << "; wood " << city.wood
             << "; steel " << city.steel << "; steam cores " << city.steamCores
             << "; raw food " << city.rawFood << "; food rations " << city.foodRations;
        print(text.str());
    }

    static const char* resourceName(LONG resource) {
        constexpr const char* names[] = {
            "coal", "wood", "steel", "steam cores", "raw food", "food rations"};
        return frostoverlay::validResource(resource) ? names[resource] : "unknown resource";
    }

    static LONG cityAmount(const CitySnapshotPayload& city, LONG resource) {
        const LONG values[] = {city.coal, city.wood, city.steel,
            city.steamCores, city.rawFood, city.foodRations};
        return frostoverlay::validResource(resource) ? values[resource] : -1;
    }

    void sendTransferResult(const TransferResultPayload& result) {
        sendPacket(makePacket(MessageType::transferResult, ++sequence_, local_, result), true);
    }

    void beginOverlayTransfer(LONG resource, LONG amount) {
        if(saves_ && saves_->active()) { overlay_.notify("Wait for the save or load operation to finish."); return; }
        if (!connected_ || outgoingTransfer_.stage != TransferStage::none ||
            incomingTransfer_.active) {
            overlay_.notify("A transfer is already in progress or the connection is unavailable.");
            return;
        }
        if (!frostoverlay::validResource(resource) || !frostoverlay::validAmount(amount)) {
            overlay_.notify("Invalid transfer request rejected.");
            return;
        }
        if (!lastLocalCity_ || cityAmount(*lastLocalCity_, resource) < amount) {
            overlay_.notify(std::string("Not enough ") + resourceName(resource) + " to transfer.");
            return;
        }
        const LONG applySequence = overlay_.apply(resource, -amount);
        if (!applySequence) {
            overlay_.notify("The game mod is not ready to change this resource.");
            return;
        }
        outgoingTransfer_ = {TransferStage::debit, ++nextTransferId_, resource,
            amount, applySequence, 0, 0, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
        overlay_.notify(("Transferring " + std::to_string(amount) + " ") + resourceName(resource) + "…");
        overlay_.busy(true);
    }

    void receiveTransferRequest(const TransferRequestPayload& request) {
        if (!request.id || !frostoverlay::validResource(request.resource) ||
            !frostoverlay::validAmount(request.amount)) {
            sendTransferResult({request.id, -1, 0});
            return;
        }
        if (lastIncomingResult_.id == request.id) {
            sendTransferResult(lastIncomingResult_); // idempotent retry; never apply twice
            return;
        }
        if (incomingTransfer_.active || outgoingTransfer_.stage != TransferStage::none) {
            sendTransferResult({request.id, -2, 0});
            return;
        }
        const LONG sequence = overlay_.apply(request.resource, request.amount);
        if (!sequence) {
            sendTransferResult({request.id, -1, 0});
            return;
        }
        incomingTransfer_ = {true, request, sequence};
    }

    void receiveTransferResult(const TransferResultPayload& result) {
        if (outgoingTransfer_.stage != TransferStage::awaitingPeer ||
            result.id != outgoingTransfer_.id) return;
        if (result.status == 1 && result.applied == outgoingTransfer_.amount) {
            const std::string message = "You sent " + peerName_ + " " + std::to_string(outgoingTransfer_.amount) + " " +
                std::string(resourceName(outgoingTransfer_.resource)) + ".";
            print("[trade] " + message);
            overlay_.notify(message, true);
            overlay_.busy(false);
            outgoingTransfer_ = {};
            return;
        }
        // A receiver can hit storage capacity and accept only part of the credit.
        // Never refund the already credited part: that would create resources.
        if (result.applied < 0 || result.applied > outgoingTransfer_.amount) return;
        outgoingTransfer_.refundAmount = outgoingTransfer_.amount - result.applied;
        if (!outgoingTransfer_.refundAmount) {
            overlay_.busy(false);
            outgoingTransfer_ = {};
            return;
        }
        outgoingTransfer_.applySequence = overlay_.apply(
            outgoingTransfer_.resource, outgoingTransfer_.refundAmount);
        outgoingTransfer_.stage = TransferStage::refund;
        outgoingTransfer_.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }

    void pollOverlay(const std::chrono::steady_clock::time_point now) {
        if (const auto request = overlay_.outgoing()) {
            beginOverlayTransfer(request->resource, request->amount);
        }

        if (outgoingTransfer_.stage == TransferStage::debit) {
            if (const auto applied = overlay_.applied(outgoingTransfer_.applySequence)) {
                if (*applied == -outgoingTransfer_.amount) {
                    const TransferRequestPayload packet{outgoingTransfer_.id,
                        outgoingTransfer_.resource, outgoingTransfer_.amount};
                    if (sendPacket(makePacket(MessageType::transferRequest,
                                              ++sequence_, local_, packet), true)) {
                        outgoingTransfer_.stage = TransferStage::awaitingPeer;
                        outgoingTransfer_.deadline = now + std::chrono::seconds(2);
                    } else {
                        outgoingTransfer_.refundAmount = outgoingTransfer_.amount;
                        outgoingTransfer_.applySequence = overlay_.apply(
                            outgoingTransfer_.resource, outgoingTransfer_.refundAmount);
                        outgoingTransfer_.stage = TransferStage::refund;
                    }
                } else {
                    // If the engine clamped a debit, restore precisely what it removed.
                    outgoingTransfer_.refundAmount = *applied < 0 ? -*applied : 0;
                    if (outgoingTransfer_.refundAmount) {
                        outgoingTransfer_.applySequence = overlay_.apply(
                            outgoingTransfer_.resource, outgoingTransfer_.refundAmount);
                        outgoingTransfer_.stage = TransferStage::refund;
                    } else {
                        overlay_.notify(std::string("Not enough ") +
                            resourceName(outgoingTransfer_.resource) + " to transfer.");
                        overlay_.busy(false);
                        outgoingTransfer_ = {};
                    }
                }
            } else if (now > outgoingTransfer_.deadline) {
                overlay_.notify("The game did not confirm the resource deduction.");
                overlay_.busy(false);
                outgoingTransfer_ = {};
            }
        } else if (outgoingTransfer_.stage == TransferStage::awaitingPeer &&
                   now > outgoingTransfer_.deadline) {
            if (outgoingTransfer_.retries++ < 3) {
                const TransferRequestPayload packet{outgoingTransfer_.id,
                    outgoingTransfer_.resource, outgoingTransfer_.amount};
                sendPacket(makePacket(MessageType::transferRequest, ++sequence_, local_, packet), true);
                outgoingTransfer_.deadline = now + std::chrono::seconds(2);
            } else {
                // Do not refund an ambiguous acknowledged-over-the-network credit;
                // that could duplicate resources. The repeated ID is safe to retry later.
                overlay_.notify("Transfer was not confirmed; automatic rollback is unsafe.");
                print("[trade] confirmation timeout; local debit kept to prevent duplication.");
                overlay_.busy(false);
                outgoingTransfer_ = {};
            }
        } else if (outgoingTransfer_.stage == TransferStage::refund) {
            if (const auto applied = overlay_.applied(outgoingTransfer_.applySequence)) {
                overlay_.notify(*applied == outgoingTransfer_.refundAmount
                    ? "The unaccepted resource was returned to the sender."
                    : "Resource rollback failed; check your city.");
                overlay_.busy(false);
                outgoingTransfer_ = {};
            } else if (now > outgoingTransfer_.deadline) {
                overlay_.notify("The game did not confirm the resource rollback.");
                overlay_.busy(false);
                outgoingTransfer_ = {};
            }
        }

        if (incomingTransfer_.active) {
            if (const auto applied = overlay_.applied(incomingTransfer_.applySequence)) {
                const bool success = *applied == incomingTransfer_.request.amount;
                lastIncomingResult_ = {incomingTransfer_.request.id, success ? 1 : -1, *applied};
                sendTransferResult(lastIncomingResult_);
                if (success) {
                    const std::string message = "Received " + std::to_string(incomingTransfer_.request.amount) + " " +
                        std::string(resourceName(incomingTransfer_.request.resource)) +
                        " from " + peerName_ + ".";
                    print("[trade] " + message);
                    overlay_.notify(message, true);
                }
                incomingTransfer_ = {};
            }
        }
    }

    void sendPause(bool paused, std::uint32_t request = 0, std::uint32_t delayMs = 0) {
        const PausePayload payload{request, paused ? 1 : 0, delayMs};
        sendPacket(makePacket(MessageType::pauseState, ++sequence_, local_, payload), true);
    }
    void sendSpeed(MessageType type, int mode, std::uint32_t revision = 0) {
        const SpeedPayload payload{revision, mode};
        sendPacket(makePacket(type, ++sequence_, local_, payload), true);
    }
    void commitSpeed(int mode) {
        if (mode < 0 || mode > 2) return;
        // The host orders both players' inputs so simultaneous clicks converge.
        sharedSpeed_ = mode;
        ++speedRevision_;
        gameSession_.commandSpeed(mode);
        clockCorrection_.reset();
        sendSpeed(MessageType::speedState, mode, speedRevision_);
    }
    void receiveSpeed(MessageType type, const SpeedPayload& speed) {
        if (speed.mode < 0 || speed.mode > 2) return;
        if (host_ && type == MessageType::speedRequest && !speed.revision) {
            commitSpeed(speed.mode);
        } else if (!host_ && type == MessageType::speedState &&
                   speed.revision && speed.revision > speedRevision_) {
            speedRevision_ = speed.revision;
            sharedSpeed_ = speed.mode;
            gameSession_.commandSpeed(speed.mode);
            clockCorrection_.reset();
        }
    }

    void receivePause(const PausePayload& pause) {
        if (pause.paused != 0 && pause.paused != 1) return;
        if (pause.request && pause.request != startRequest_) return;
        const bool paused = pause.paused != 0;
        if (!pause.request) peerPauseRequested_ = paused;
        if (pause.delayMs) {
            if (pause.request && !paused) sessionBarrierReleased_ = true;
            pendingPause_.active = true;
            pendingPause_.paused = paused;
            pendingPause_.deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds((std::min)(pause.delayMs, 5000u));
        } else if (!(startCommitted_ && !sessionBarrierReleased_ && !paused)) {
            gameSession_.commandPause(paused || peerPauseRequested_ || clockHold_ || (saves_ && saves_->active()));
        }
        print(paused ? "[sync] The other player enabled the shared pause."
                     : "[sync] The other player disabled the shared pause.");
    }

    void receiveSessionState(const SessionStatePayload& state) {
        if (state.loaded != 0 && state.loaded != 1) return;
        if (state.paused != 0 && state.paused != 1) return;
        if (state.gameTimeMs < 0) return;
        peerSession_ = state;
        peerSessionReceived_ = std::chrono::steady_clock::now();
    }

    void sendSessionState() {
        const auto state = gameSession_.state();
        if (!state.available) return;
        const SessionStatePayload payload{startCommitted_ ? startRequest_ : 0,
            state.loaded ? 1 : 0, state.paused ? 1 : 0, state.gameTimeMs};
        sendPacket(makePacket(MessageType::sessionState, ++sequence_, local_, payload), false);
    }

    void pollGameSession(const std::chrono::steady_clock::time_point now) {
        const auto localState = gameSession_.state();
        const bool peerFresh = connected_ && peerSession_ && now - peerSessionReceived_ < std::chrono::seconds(2);
        overlay_.session(!localState.available ? 0 : !localState.loaded ? 1 : localState.paused ? 3 : 2,
            !peerFresh ? 0 : !peerSession_->loaded ? 1 : peerSession_->paused ? 3 : 2,
            peerFresh && localState.loaded && peerSession_->loaded
                ? static_cast<LONG>((localState.gameTimeMs - peerSession_->gameTimeMs) / 1000) : 0);
        const auto speed = gameSession_.localSpeedEvent();
        if (connected_) {
            if (speed) {
                if (host_) commitSpeed(*speed);
                else sendSpeed(MessageType::speedRequest, *speed);
            } else if (host_ && !speedRevision_ && localState.loaded && gameSession_.speed() >= 0) {
                commitSpeed(gameSession_.speed());
            }
        }
        if (pendingPause_.active && now >= pendingPause_.deadline) {
            gameSession_.commandPause(pendingPause_.paused || peerPauseRequested_ || clockHold_ || (saves_ && saves_->active()));
            pendingPause_ = {};
        }
        if (const auto pause = gameSession_.localPauseEvent()) {
            if (startCommitted_ && !sessionBarrierReleased_ && !*pause) {
                gameSession_.commandPause(true);
                print("[sync] Startup pause remains active until the other city loads.");
            }
            if (connected_) {
                sendPause(*pause);
                print(*pause ? "[sync] Shared pause enabled."
                             : "[sync] Shared pause disabled.");
            }
        }

        // Keep the leading city still and let the lagging city simulate normally.
        // Peer/UI holds remain authoritative, and correction holds are never echoed.
        if (connected_ && localState.available && localState.loaded) {
            const bool fresh = peerSession_ && now - peerSessionReceived_ < std::chrono::seconds(2);
            const bool waiting = !fresh || !peerSession_->loaded;
            if (waiting) clockCorrection_.reset();
            const auto wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            const bool hold = waiting || clockCorrection_.update(localState.gameTimeMs,
                peerSession_->gameTimeMs, wallMs, gameSession_.localPause() || peerPauseRequested_);
            gameSession_.commandPause(hold || peerPauseRequested_ || (saves_ && saves_->active()));
            if (hold != clockHold_) {
                clockHold_ = hold;
                gameSession_.commandPause(hold || peerPauseRequested_ || (saves_ && saves_->active()));
                // Frame-sized corrections are normal; do not flood the chat.
                if (hold && (waiting || localState.gameTimeMs - peerSession_->gameTimeMs > 60000) &&
                    !clockWaitReported_) {
                    print("[sync] City is waiting for game-time alignment.");
                    clockWaitReported_ = true;
                } else if (!hold && clockWaitReported_) {
                    print("[sync] Game time aligned; the city is resuming.");
                    clockWaitReported_ = false;
                }
            }
        }
        // Once both cities exist, correction replaces the initial all-city hold.
        if (startCommitted_ && !sessionBarrierReleased_ && peerSession_ &&
            localState.loaded && peerSession_->loaded &&
            peerSession_->request == startRequest_) {
            sessionBarrierReleased_ = true;
            gameSession_.commandPause(clockHold_ || peerPauseRequested_ || (saves_ && saves_->active()));
        }
    }

    void sendStart(MessageType type, std::uint32_t request, LONG state, LONG mapIndex = -1) {
        sendPacket(makePacket(type, ++sequence_, local_,
                              StartPayload{request, state, mapIndex, selectedDifficulty_}), true);
    }

    void commitLocalPreparedMap() {
        const LONG state = requestLocalCommit();
        startCommitted_ = state == frostlaunch::commitRequested;
        sessionBarrierReleased_ = false;
        skewReported_ = false;
        peerSession_.reset();
        pendingPause_ = {};
        waitingStart_ = false;
        launchDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        print(state == frostlaunch::commitRequested
            ? "[game] launching: starting the selected map for both players…"
            : "[game] failed: the game is not ready to start. Return to the connection menu.");
        if (state != frostlaunch::commitRequested) {
            launchReported_ = true;
            sendStart(MessageType::startResult, startRequest_, frostlaunch::failed);
        }
    }

    void abortMapSelection(const char* reason, bool notifyPeer) {
        if(notifyPeer && connected_ && startRequest_)
            sendStart(MessageType::startResult,startRequest_,frostlaunch::failed,selectedMapIndex_);
        waitingStart_=prepared_=hostMapPreparing_=waitingClientMap_=false;
        clientMapPreparing_=clientMapPrepared_=false;
        selectedMapIndex_=-1;
        selectedDifficulty_ = {};
        resetLocalLaunch();
        print(std::string("[game] rejected: ")+reason);
    }

    void receiveStart(MessageType type, const StartPayload& start) {
        if (!connected_ || !start.request) return;
        if (type == MessageType::startPrepare && !host_) {
            if (startCommitted_ || start.request < startRequest_) return;
            startRequest_ = start.request;
            const LONG state = localLaunch().state;
            prepared_ = state == frostlaunch::ready;
            launchDeadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(10);
            sendStart(MessageType::startReady, start.request, state);
        } else if (type == MessageType::startReady && host_ && waitingStart_ && start.request == startRequest_) {
            waitingStart_ = false;
            if (start.status != frostlaunch::ready || localLaunch().state != frostlaunch::ready) {
                print("[game] rejected: both players must be in the connection menu with the current mod.");
                return;
            }
            const LONG state = requestLocalPrepare(selectedMapIndex_);
            if (state != frostlaunch::prepareRequested) {
                print("[game] rejected: the host could not open map selection.");
                return;
            }
            hostMapPreparing_ = true;
            launchDeadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(10);
            print("[game] host-map: the host is preparing the map and its exact index.");
        } else if (type == MessageType::startCommit && !host_ && prepared_ &&
                   !startCommitted_ && start.request == startRequest_ &&
                   std::chrono::steady_clock::now() < launchDeadline_) {
            prepared_ = false;
            if (!start.difficulty.valid()) {
                abortMapSelection("invalid host difficulty settings.", true);
                return;
            }
            selectedDifficulty_ = start.difficulty;
            selectedMapIndex_ = start.mapIndex;
            const LONG state = requestLocalPrepare(selectedMapIndex_);
            clientMapPreparing_ = state == frostlaunch::prepareRequested;
            launchDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            if (!clientMapPreparing_)
                sendStart(MessageType::startResult, startRequest_, frostlaunch::failed,
                          selectedMapIndex_);
        } else if (type == MessageType::startGo && !host_ && clientMapPrepared_ &&
                   start.request == startRequest_ && start.mapIndex == selectedMapIndex_ &&
                   start.difficulty == selectedDifficulty_) {
            commitLocalPreparedMap();
        } else if (type == MessageType::startResult && host_ && waitingClientMap_ &&
                   start.request == startRequest_ && start.status == frostlaunch::prepared) {
            waitingClientMap_ = false;
            if (start.mapIndex != selectedMapIndex_ || !(start.difficulty == selectedDifficulty_)) {
                abortMapSelection("the client prepared different map or difficulty settings.", true);
                return;
            }
            sendStart(MessageType::startGo, startRequest_, frostlaunch::commitRequested,
                      selectedMapIndex_);
            commitLocalPreparedMap();
        } else if (type == MessageType::startResult && start.request == startRequest_ &&
                   start.status == frostlaunch::dispatched) {
            print("[game] peer-loading: the other player started the same map.");
        } else if (type == MessageType::startResult && start.request == startRequest_ &&
                   start.status == frostlaunch::failed) {
            abortMapSelection("the other player cancelled map selection or could not prepare it.",false);
        }
    }

    void pollMapLaunch(const std::chrono::steady_clock::time_point now) {
        const auto launch = localLaunch();
        if (hostMapPreparing_) {
            if (launch.state == frostlaunch::prepared) {
                hostMapPreparing_ = false;
                waitingClientMap_ = true;
                selectedMapIndex_ = launch.mapIndex;
                selectedDifficulty_ = launch.difficulty;
                if (!selectedDifficulty_.valid()) {
                    abortMapSelection("could not read the host difficulty settings.", true);
                    return;
                }
                sendStart(MessageType::startCommit, startRequest_, frostlaunch::prepared,
                          selectedMapIndex_);
                launchDeadline_ = now + std::chrono::seconds(30);
                print("[game] host-map: map index sent to the client; waiting for confirmation.");
            } else if (launch.state == frostlaunch::failed || now > launchDeadline_) {
                abortMapSelection("map selection cancelled; you can choose another mode.",true);
            }
        }
        if (clientMapPreparing_) {
            if (launch.state == frostlaunch::prepared) {
                clientMapPreparing_ = false;
                if (!(launch.difficulty == selectedDifficulty_)) {
                    abortMapSelection("client difficulty verification failed.", true);
                    return;
                }
                clientMapPrepared_ = true;
                sendStart(MessageType::startResult, startRequest_, frostlaunch::prepared,
                          launch.mapIndex);
                print("[game] client-map: the host map is ready; waiting for the shared start.");
            } else if (launch.state == frostlaunch::failed || now > launchDeadline_) {
                abortMapSelection("the host map was cancelled or is unavailable.",true);
            }
        }
        if (waitingClientMap_ && now > launchDeadline_) {
            abortMapSelection("the client did not confirm the map within 30 seconds.",true);
        }
    }

    void print(const std::string& text) {
        std::lock_guard lock(outputMutex_);
        output_(text);
    }

    bool sendPacket(const std::vector<std::uint8_t>& packet, bool reliable) {
        if (!transport_.send(packet.data(), static_cast<std::uint32_t>(packet.size()),
                             reliable)) {
            print(std::string("[") + transport_.name() + "] send failed.");
            return false;
        }
        return true;
    }

    void sendHello(MessageType type) {
        HelloPayload payload{};
        std::memcpy(payload.playerName, playerName_.data(),
                    (std::min)(playerName_.size(), sizeof(payload.playerName) - 1));
        std::memcpy(payload.cityName, cityName_.data(),
                    (std::min)(cityName_.size(), sizeof(payload.cityName) - 1));
        payload.sessionStartedAtMs=sessionStartedAtMs_;
        sendPacket(makePacket(type, ++sequence_, local_, payload), true);
    }

    void processCommand(const std::string& line) {
        if(line.rfind("save ",0)==0 || line.rfind("load ",0)==0) {
            if(!connected_ || !saves_) { print("[save] Connect both players first."); return; }
            const auto text=line.substr(5);
            int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
            std::wstring slot(size,L'\0');
            if(size) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),slot.data(),size);
            saves_->request(line[0]=='s'?frostsave::save:frostsave::load,slot); return;
        }
        if (line == "start" || line == "start story") {
            if(saves_ && saves_->active()) { print("[save] Wait for the checkpoint operation."); return; }
            if (!host_ || !connected_ || startCommitted_ || waitingStart_) {
                print("[game] rejected: only the connected host can start, once per session.");
                return;
            }
            if (localLaunch().state != frostlaunch::ready) {
                print("[game] rejected: open the in-game connection menu; the current mod is required.");
                return;
            }
            ++startRequest_;
            selectedMapIndex_ = line == "start story" ? frostlaunch::chooseStory : -1;
            hostMapPreparing_ = clientMapPreparing_ = clientMapPrepared_ = false;
            waitingClientMap_ = false;
            waitingStart_ = true;
            launchDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            sendStart(MessageType::startPrepare, startRequest_, 0);
            print("[game] preparing: checking both players…");
            return;
        }
        if (line == "hello") {
            sendHello(MessageType::hello);
            return;
        }
        if (line.rfind("chat ", 0) == 0) {
            const std::string text = line.substr(5);
            sendPacket(makeTextPacket(MessageType::chat, ++sequence_, local_, text), true);
            print("[you] " + text);
            return;
        }
        if (line.rfind("status ", 0) == 0) {
            CitySnapshotPayload status{};
            std::istringstream input(line.substr(7));
            if (!(input >> status.coal >> status.wood >> status.steel >> status.steamCores >>
                  status.rawFood >> status.foodRations >> status.population >> status.temperature)) {
                print("Usage: status COAL WOOD STEEL CORES RAW_FOOD FOOD_RATIONS POPULATION TEMPERATURE");
                return;
            }
            sendPacket(makePacket(MessageType::citySnapshot, ++sequence_, local_, status),
                       true);
            lastLocalCity_ = status;
            overlay_.local(status);
            print("[city] snapshot sent.");
            return;
        }
        if (!line.empty()) print("Unknown command. Type hello, status ..., chat ..., or quit.");
    }

    void drainCommands() {
        std::queue<std::string> pending;
        {
            std::lock_guard lock(queueMutex_);
            std::swap(pending, commands_);
        }
        while (!pending.empty()) {
            processCommand(pending.front());
            pending.pop();
        }
    }

    void receivePackets() {
        for (;;) {
            std::vector<std::uint8_t> bytes;
            std::uint64_t sender = 0;
            if (!transport_.receive(bytes, sender)) break;
            const auto received = static_cast<std::uint32_t>(bytes.size());
            if ((expectedPeer_ && sender != expectedPeer_) ||
                received < sizeof(PacketHeader)) continue;

            PacketHeader header{};
            std::memcpy(&header, bytes.data(), sizeof(header));
            if (header.magic != kProtocolMagic || header.version != kProtocolVersion ||
                header.senderSteamId != sender ||
                header.payloadBytes != received - sizeof(PacketHeader)) {
                print("[protocol] rejected malformed packet.");
                continue;
            }

            lastReceived_ = std::chrono::steady_clock::now();
            const auto* payload = bytes.data() + sizeof(PacketHeader);
            if ((header.type == MessageType::hello ||
                 header.type == MessageType::helloAck) &&
                header.payloadBytes == sizeof(HelloPayload)) {
                HelloPayload hello{};
                std::memcpy(&hello, payload, sizeof(hello));
                hello.playerName[sizeof(hello.playerName) - 1] = '\0';
                hello.cityName[sizeof(hello.cityName) - 1] = '\0';
                if (!hello.playerName[0]) continue;
                if(!roleDetermined_) {
                    // The earlier Connect click owns the Steam session. Epoch
                    // milliseconds make the decision identical on both peers;
                    // SteamID is only a sub-millisecond tie breaker.
                    host_=sessionStartedAtMs_<hello.sessionStartedAtMs ||
                        (sessionStartedAtMs_==hello.sessionStartedAtMs && local_<sender);
                    roleDetermined_=true;
                    if(saves_) saves_->setHost(host_);
                }
                const bool firstConnection=!connected_;
                connected_ = true;
                expectedPeer_ = sender;
                peerName_ = hello.playerName;
                for (auto& ch : peerName_) if (static_cast<unsigned char>(ch) < 32) ch = ' ';
                if (header.type == MessageType::hello) sendHello(MessageType::helloAck);
                if(firstConnection) {
                    print(std::string("[peer] player: ") + hello.playerName +
                          ", city: " + hello.cityName + " (connected)");
                    overlay_.connected(playerName_, peerName_);
                    if(saves_) saves_->connected(true);
                    print(host_?"[role] host":"[role] client");
                    sendPause(gameSession_.localPause());
                    if (host_ && speedRevision_) sendSpeed(MessageType::speedState, sharedSpeed_, speedRevision_);
                }
            } else if (connected_ &&
                       ((header.type >= MessageType::startPrepare &&
                         header.type <= MessageType::startResult) ||
                        header.type == MessageType::startGo) &&
                       header.payloadBytes == sizeof(StartPayload)) {
                StartPayload start{};
                std::memcpy(&start, payload, sizeof(start));
                receiveStart(header.type, start);
            } else if (connected_ && header.type == MessageType::citySnapshot &&
                       header.payloadBytes == sizeof(CitySnapshotPayload)) {
                CitySnapshotPayload city{};
                std::memcpy(&city, payload, sizeof(city));
                if (city.hope < -1 || city.hope > 10000 ||
                    city.discontent < -1 || city.discontent > 10000) continue;
                overlay_.peer(city);
            } else if (connected_ && header.type == MessageType::transferRequest &&
                       header.payloadBytes == sizeof(TransferRequestPayload)) {
                TransferRequestPayload request{};
                std::memcpy(&request, payload, sizeof(request));
                receiveTransferRequest(request);
            } else if (connected_ && header.type == MessageType::transferResult &&
                       header.payloadBytes == sizeof(TransferResultPayload)) {
                TransferResultPayload result{};
                std::memcpy(&result, payload, sizeof(result));
                receiveTransferResult(result);
            } else if (connected_ && (header.type == MessageType::speedRequest ||
                       header.type == MessageType::speedState) && header.payloadBytes == sizeof(SpeedPayload)) {
                SpeedPayload speed{};
                std::memcpy(&speed, payload, sizeof(speed));
                receiveSpeed(header.type, speed);
            } else if (connected_ && header.type == MessageType::pauseState &&
                       header.payloadBytes == sizeof(PausePayload)) {
                PausePayload pause{};
                std::memcpy(&pause, payload, sizeof(pause));
                receivePause(pause);
            } else if (connected_ && header.type == MessageType::sessionState &&
                       header.payloadBytes == sizeof(SessionStatePayload)) {
                SessionStatePayload state{};
                std::memcpy(&state, payload, sizeof(state));
                receiveSessionState(state);
            } else if (connected_ && header.type == MessageType::checkpoint && header.payloadBytes==sizeof(frostsave::Packet)) {
                frostsave::Packet checkpoint{};
                std::memcpy(&checkpoint,payload,sizeof(checkpoint));
                if(saves_) saves_->receive(checkpoint);
            } else if (connected_ && header.type == MessageType::chat) {
                print("[peer] " + std::string(reinterpret_cast<const char*>(payload),
                                               header.payloadBytes));
            }
        }
    }

    void networkLoop() {
        sendHello(MessageType::hello);
        auto nextHello = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        auto nextHeartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto nextCityRead = std::chrono::steady_clock::now();
        auto nextSessionState = std::chrono::steady_clock::now();

        while (running_.load(std::memory_order_acquire)) {
            transport_.pump();
            receivePackets();
            if (transport_.isClosed()) {
                print("[connection] disconnected");
                break;
            }
            drainCommands();

            const auto now = std::chrono::steady_clock::now();
            pollOverlay(now);
            pollGameSession(now);
            pollMapLaunch(now);
            if(saves_) saves_->poll();
            if (waitingStart_ && now > launchDeadline_) {
                waitingStart_ = false;
                print("[game] rejected: the other player did not confirm readiness within 10 seconds.");
            }
            if (startCommitted_ && !launchReported_) {
                const LONG state = localLaunch().state;
                if (state == frostlaunch::dispatched || state == frostlaunch::failed || now > launchDeadline_) {
                    launchReported_ = true;
                    const bool ok = state == frostlaunch::dispatched;
                    print(ok ? "[game] loading: start command delivered; waiting for city resources."
                             : "[game] failed: automatic start did not complete. Check the game and FrostMenuMod.log.");
                    sendStart(MessageType::startResult, startRequest_, ok ? frostlaunch::dispatched : frostlaunch::failed);
                }
            }
            if (!connected_ && now >= nextHello) {
                sendHello(MessageType::hello);
                nextHello = now + std::chrono::seconds(2);
            }
            if (now >= nextHeartbeat) {
                HeartbeatPayload heartbeat{};
                heartbeat.uptimeMs = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count());
                sendPacket(makePacket(MessageType::heartbeat, ++sequence_, local_, heartbeat),
                           false);
                nextHeartbeat = now + std::chrono::seconds(5);
            }
            if (connected_ && now >= nextSessionState) {
                sendSessionState();
                nextSessionState = now + std::chrono::milliseconds(25);
            }
            if (connected_ && watchFrostpunk_ && now >= nextCityRead) {
                if (const auto city = readLocalCity(frostpunkProcessId_)) {
                    {
                        sendPacket(makePacket(MessageType::citySnapshot, ++sequence_, local_,
                                              *city), false);
                        overlay_.local(*city);
                        lastLocalCity_ = *city;
                    }
                    frostpunkMissingReported_ = false;
                } else if (!frostpunkMissingReported_) {
                    print("[local city] waiting for a supported Frostpunk city...");
                    frostpunkMissingReported_ = true;
                }
                nextCityRead = now + std::chrono::seconds(1);
            }

            std::unique_lock lock(queueMutex_);
            queueChanged_.wait_for(lock, std::chrono::milliseconds(25));
        }
        transport_.close();
        if(saves_) saves_->connected(false);
        overlay_.close();
        gameSession_.close();
    }

    PacketTransport& transport_;
    std::uint64_t local_ = 0;
    std::uint64_t expectedPeer_ = 0;
    std::string playerName_;
    std::string cityName_;
    bool watchFrostpunk_ = false;
    DWORD frostpunkProcessId_ = 0;
    std::unique_ptr<frostsave::Sync> saves_;
    bool host_ = false, roleDetermined_ = false, waitingStart_ = false, prepared_ = false;
    bool hostMapPreparing_ = false, waitingClientMap_ = false;
    bool clientMapPreparing_ = false, clientMapPrepared_ = false;
    bool startCommitted_ = false, launchReported_ = false;
    std::uint32_t startRequest_ = 0;
    LONG selectedMapIndex_ = -1;
    frostlaunch::Difficulty selectedDifficulty_{};
    std::chrono::steady_clock::time_point launchDeadline_{};
    std::string peerName_;
    std::optional<CitySnapshotPayload> lastLocalCity_;
    TransferResultPayload lastIncomingResult_{};
    std::uint32_t nextTransferId_ = 0;
    OverlayBridge overlay_;
    GameSessionBridge gameSession_;
    struct PendingPause {
        bool active = false;
        bool paused = true;
        std::chrono::steady_clock::time_point deadline{};
    } pendingPause_;
    bool peerPauseRequested_ = false;
    std::optional<SessionStatePayload> peerSession_;
    std::chrono::steady_clock::time_point peerSessionReceived_{};
    bool clockHold_ = false;
    std::uint32_t speedRevision_ = 0;
    int sharedSpeed_ = -1;
    frostsync::ClockCorrection clockCorrection_;
    bool clockWaitReported_ = false;
    bool sessionBarrierReleased_ = false;
    bool skewReported_ = false;
    bool frostpunkMissingReported_ = false;
    std::uint64_t sessionStartedAtMs_ = 0;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point lastReceived_{};
    std::atomic<bool> running_{true};
    bool connected_ = false;
    std::uint32_t sequence_ = 0;
    std::thread worker_;
    std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::queue<std::string> commands_;
    std::mutex outputMutex_;
    LogSink output_;
};

void printUsage() {
    std::cout <<
        "FrostBridgeNet - direct multiplayer transport prototype\n\n"
        "  FrostBridgeNet.exe --identity\n"
        "  FrostBridgeNet.exe --peer STEAMID64 --name PLAYER [--city CITY] [--watch-frostpunk] [--pid PID]\n"
        "  FrostBridgeNet.exe --lan-host PORT --name PLAYER [--city CITY] [--watch-frostpunk] [--pid PID]\n"
        "  FrostBridgeNet.exe --lan-join HOST[:PORT] --name PLAYER [--city CITY] [--watch-frostpunk] [--pid PID]\n\n"
        "For two games on one PC, join 127.0.0.1 and pass each Frostpunk PID.\n";
}

}  // namespace

namespace frostbridge {
namespace {
class EmbeddedSession final : public ConnectionSession {
public:
    explicit EmbeddedSession(ConnectionOptions options)
        : thread_([this, options = std::move(options)] { run(options); }) {}
    ~EmbeddedSession() override { stop(); }
    void send(std::string command) override {
        std::lock_guard lock(mutex_);
        if (session_) session_->enqueue(std::move(command));
    }
    void stop() override {
        cancelled_.store(true);
        {
            std::lock_guard lock(mutex_);
            if (session_) session_->requestStop();
        }
        if (thread_.joinable()) thread_.join();
    }
    std::vector<std::string> takeOutput() override {
        std::lock_guard lock(mutex_);
        std::vector<std::string> result;
        result.swap(output_);
        return result;
    }
    bool finished() const override { return finished_.load(); }
    bool failed() const override { return failed_.load(); }
private:
    void log(const std::string& text) {
        std::lock_guard lock(mutex_);
        if (output_.size() < 1024) output_.push_back(text);
    }
    void run(const ConnectionOptions& options) {
        try {
            const LogSink logger = [this](const std::string& text) { log(text); };
            // Destruction order: transport/session are released before Steam API.
            std::unique_ptr<SteamApi> steam;
            std::unique_ptr<PacketTransport> transport;
            if (options.steam) {
                steam = std::make_unique<SteamApi>(executableDirectory());
                const auto peer = parseSteamId(std::wstring(options.address.begin(), options.address.end()));
                if (peer == steam->localSteamId()) fail("Peer SteamID64 cannot be your own SteamID64.");
                transport = std::make_unique<SteamTransport>(*steam, peer);
            } else if (options.host) {
                transport = LanTransport::host(options.port, &cancelled_, logger);
            } else {
                transport = LanTransport::join(LanEndpoint{options.address, options.port}, &cancelled_, logger);
            }
            if (!cancelled_.load()) {
                auto session = std::make_shared<BridgeSession>(*transport, options.playerName,
                    options.playerName, options.gamePid != 0, options.gamePid, options.host, logger);
                {
                    std::lock_guard lock(mutex_);
                    session_ = session;
                    if (cancelled_.load()) session->requestStop();
                }
                session->runEmbedded();
                std::lock_guard lock(mutex_);
                session_.reset();
            }
        } catch (const std::exception& error) {
            if (!cancelled_.load()) { failed_.store(true); log(std::string("error: ") + error.what()); }
            std::lock_guard lock(mutex_);
            session_.reset();
        }
        finished_.store(true);
    }
    std::mutex mutex_;
    std::shared_ptr<BridgeSession> session_;
    std::vector<std::string> output_;
    std::atomic<bool> cancelled_{false}, finished_{false}, failed_{false};
    std::thread thread_; // last: all shared fields initialized before the thread
};
}
std::unique_ptr<ConnectionSession> connect(ConnectionOptions options) {
    if (options.playerName.empty() || options.playerName.size() >= 64 ||
        options.playerName.find_first_of("\r\n") != std::string::npos)
        fail("Player name must contain 1 to 63 UTF-8 bytes and no line breaks.");
    if (!options.port) fail("Port must be a number from 1 to 65535.");
    return std::make_unique<EmbeddedSession>(std::move(options));
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--ui") {
        return frostbridge::runConnectionUI(std::vector<std::wstring>(argv + 2, argv + argc));
    }
    // Pipe readers in FrostConnectionUI need immediate output, not console buffering.
    std::cout << std::unitbuf;
    try {
        bool identityOnly = false;
        bool watchFrostpunk = false;
        std::uint64_t peer = 0;
        std::optional<std::uint16_t> lanHostPort;
        std::optional<LanEndpoint> lanJoin;
        DWORD processId = 0;
        std::string playerName;
        std::string city = "New London";
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--identity") {
                identityOnly = true;
            } else if (arg == L"--watch-frostpunk") {
                watchFrostpunk = true;
            } else if (arg == L"--peer" && i + 1 < argc) {
                peer = parseSteamId(argv[++i]);
            } else if (arg == L"--lan-host" && i + 1 < argc) {
                lanHostPort = parsePort(argv[++i]);
            } else if (arg == L"--lan-join" && i + 1 < argc) {
                lanJoin = parseEndpoint(argv[++i]);
            } else if (arg == L"--name" && i + 1 < argc) {
                playerName = narrow(argv[++i]);
            } else if (arg == L"--city" && i + 1 < argc) {
                city = narrow(argv[++i]);
            } else if (arg == L"--pid" && i + 1 < argc) {
                processId = parseProcessId(argv[++i]);
            } else {
                printUsage();
                return 2;
            }
        }

        const auto directory = executableDirectory();
        SetCurrentDirectoryW(directory.c_str());
        const int selectedModes = (peer ? 1 : 0) + (lanHostPort ? 1 : 0) +
                                  (lanJoin ? 1 : 0);
        if (identityOnly) {
            if (selectedModes) fail("--identity cannot be combined with a session mode.");
            SteamApi steam(directory);
            std::cout << "Steam user: " << steam.personaName() << '\n'
                      << "SteamID64: " << steam.localSteamId() << '\n'
                      << "Development AppID: 480\n";
            return 0;
        }
        if (selectedModes != 1 || playerName.empty()) {
            printUsage();
            return 2;
        }
        if (playerName.size() >= sizeof(HelloPayload::playerName) ||
            city.size() >= sizeof(HelloPayload::cityName)) {
            fail("Player and city names must be shorter than 64 UTF-8 bytes.");
        }

        std::unique_ptr<PacketTransport> transport;
        std::unique_ptr<SteamApi> steam;
        if (peer) {
            steam = std::make_unique<SteamApi>(directory);
            if (peer == steam->localSteamId()) {
                fail("Peer SteamID64 cannot be your own SteamID64.");
            }
            std::cout << "Steam user: " << steam->personaName() << '\n'
                      << "SteamID64: " << steam->localSteamId() << '\n'
                      << "Development AppID: 480\n"
                      << "Peer SteamID64: " << peer << '\n';
            transport = std::make_unique<SteamTransport>(*steam, peer);
        } else if (lanHostPort) {
            transport = LanTransport::host(*lanHostPort);
        } else {
            transport = LanTransport::join(*lanJoin);
        }

        std::cout << "Player: " << playerName << '\n'
                  << "Local city: " << city << '\n';
        if (processId) std::cout << "Frostpunk PID: " << processId << '\n';
        BridgeSession session(*transport, playerName, city, watchFrostpunk,
                              processId, lanHostPort.has_value());
        session.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
