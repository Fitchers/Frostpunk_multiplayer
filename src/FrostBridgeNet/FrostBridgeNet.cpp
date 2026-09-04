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
#include "../LaunchControl.h"

namespace {

constexpr std::uint32_t kProtocolMagic = 0x31504246;  // "FBP1"
constexpr std::uint16_t kProtocolVersion = 4;
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
};

struct HeartbeatPayload {
    std::uint64_t uptimeMs = 0;
};
struct StartPayload { std::uint32_t request; std::int32_t status; };

struct CitySnapshotPayload {
    std::int32_t coal = 0;
    std::int32_t wood = 0;
    std::int32_t steel = 0;
    std::int32_t steamCores = 0;
    std::int32_t rawFood = 0;
    std::int32_t foodRations = 0;
    std::int32_t population = 0;
    std::int32_t temperature = 0;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 32);
static_assert(sizeof(CitySnapshotPayload) == 32);

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
          started_(std::chrono::steady_clock::now()), output_(std::move(output)) {}

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
    LONG localLaunch(bool request = false) {
        if (!frostpunkProcessId_) return frostlaunch::unavailable;
        WinHandle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
            frostlaunch::name(frostpunkProcessId_).c_str()));
        if (!mapping) return frostlaunch::unavailable;
        auto* control = static_cast<frostlaunch::Control*>(MapViewOfFile(
            mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(frostlaunch::Control)));
        if (!control) return frostlaunch::unavailable;
        LONG state = frostlaunch::unavailable;
        if (control->signature == frostlaunch::magic) {
            state = request ? InterlockedCompareExchange(&control->state,
                frostlaunch::requested, frostlaunch::ready)
                : InterlockedCompareExchange(&control->state, 0, 0);
            if (request && state == frostlaunch::ready) state = frostlaunch::requested;
        }
        UnmapViewOfFile(control);
        return state;
    }

    void resourceLine(const std::string& name, const CitySnapshotPayload& city) {
        std::ostringstream text;
        text << name << ": ресурсы: уголь " << city.coal << "; древесина " << city.wood
             << "; сталь " << city.steel << "; паровые ядра " << city.steamCores
             << "; сырая еда " << city.rawFood << "; пищевые пайки " << city.foodRations;
        print(text.str());
    }

    void sendStart(MessageType type, std::uint32_t request, LONG state) {
        sendPacket(makePacket(type, ++sequence_, local_, StartPayload{request, state}), true);
    }

    void commitLocal() {
        const LONG state = localLaunch(true);
        startCommitted_ = true;
        waitingStart_ = false;
        launchDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        print(state == frostlaunch::requested
            ? "[game] launching: запускается бесконечный режим…"
            : "[game] failed: игра не готова к запуску. Вернитесь в меню подключения.");
        if (state != frostlaunch::requested) {
            launchReported_ = true;
            sendStart(MessageType::startResult, startRequest_, frostlaunch::failed);
        }
    }

    void receiveStart(MessageType type, const StartPayload& start) {
        if (!connected_ || !start.request || std::strcmp(transport_.name(), "lan") != 0) return;
        if (type == MessageType::startPrepare && !host_) {
            if (startCommitted_ || start.request < startRequest_) return;
            startRequest_ = start.request;
            const LONG state = localLaunch();
            prepared_ = state == frostlaunch::ready;
            launchDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            sendStart(MessageType::startReady, start.request, state);
        } else if (type == MessageType::startReady && host_ && waitingStart_ && start.request == startRequest_) {
            waitingStart_ = false;
            if (start.status != frostlaunch::ready || localLaunch() != frostlaunch::ready) {
                print("[game] rejected: оба игрока должны находиться в меню подключения с обновлённым модом.");
                return;
            }
            if (!sendPacket(makePacket(MessageType::startCommit, ++sequence_, local_,
                                       StartPayload{startRequest_, 0}), true)) {
                print("[game] rejected: не удалось передать команду клиенту.");
                return;
            }
            commitLocal();
        } else if (type == MessageType::startCommit && !host_ && prepared_ &&
                   !startCommitted_ && start.request == startRequest_ &&
                   std::chrono::steady_clock::now() < launchDeadline_) {
            prepared_ = false;
            commitLocal();
        } else if (type == MessageType::startResult && start.request == startRequest_ && startCommitted_) {
            print(start.status == frostlaunch::dispatched
                ? "[game] peer-loading: второй игрок загружает город."
                : "[game] peer-failed: второму игроку не удалось запустить город.");
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
        sendPacket(makePacket(type, ++sequence_, local_, payload), true);
    }

    void processCommand(const std::string& line) {
        if (line == "start") {
            if (!host_ || !connected_ || startCommitted_ || waitingStart_) {
                print("[game] rejected: запуск доступен только подключённому LAN-хосту, один раз за сессию.");
                return;
            }
            if (localLaunch() != frostlaunch::ready) {
                print("[game] rejected: откройте меню подключения в игре; необходим обновлённый мод.");
                return;
            }
            ++startRequest_;
            waitingStart_ = true;
            launchDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            sendStart(MessageType::startPrepare, startRequest_, 0);
            print("[game] preparing: проверяю готовность обоих игроков…");
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
                connected_ = true;
                expectedPeer_ = sender;
                peerName_ = hello.playerName;
                for (auto& ch : peerName_) if (static_cast<unsigned char>(ch) < 32) ch = ' ';
                print(std::string("[peer] player: ") + hello.playerName +
                      ", city: " + hello.cityName +
                      (header.type == MessageType::hello ? " (hello)" : " (connected)"));
                if (header.type == MessageType::hello) sendHello(MessageType::helloAck);
            } else if (connected_ && header.type >= MessageType::startPrepare &&
                       header.type <= MessageType::startResult && header.payloadBytes == sizeof(StartPayload)) {
                StartPayload start{};
                std::memcpy(&start, payload, sizeof(start));
                receiveStart(header.type, start);
            } else if (connected_ && header.type == MessageType::citySnapshot &&
                       header.payloadBytes == sizeof(CitySnapshotPayload)) {
                CitySnapshotPayload city{};
                std::memcpy(&city, payload, sizeof(city));
                resourceLine(peerName_, city);
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

        while (running_.load(std::memory_order_acquire)) {
            transport_.pump();
            receivePackets();
            if (transport_.isClosed()) {
                print("[connection] disconnected");
                break;
            }
            drainCommands();

            const auto now = std::chrono::steady_clock::now();
            if (waitingStart_ && now > launchDeadline_) {
                waitingStart_ = false;
                print("[game] rejected: второй игрок не подтвердил готовность за 10 секунд.");
            }
            if (startCommitted_ && !launchReported_) {
                const LONG state = localLaunch();
                if (state == frostlaunch::dispatched || state == frostlaunch::failed || now > launchDeadline_) {
                    launchReported_ = true;
                    const bool ok = state == frostlaunch::dispatched;
                    print(ok ? "[game] loading: команда запуска передана игре; ожидаем ресурсы города."
                             : "[game] failed: автозапуск не завершился. Проверьте окно игры и FrostMenuMod.log.");
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
            if (connected_ && watchFrostpunk_ && now >= nextCityRead) {
                if (const auto city = readLocalCity(frostpunkProcessId_)) {
                    {
                        sendPacket(makePacket(MessageType::citySnapshot, ++sequence_, local_,
                                              *city), false);
                        resourceLine(playerName_, *city);
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
    }

    PacketTransport& transport_;
    std::uint64_t local_ = 0;
    std::uint64_t expectedPeer_ = 0;
    std::string playerName_;
    std::string cityName_;
    bool watchFrostpunk_ = false;
    DWORD frostpunkProcessId_ = 0;
    bool host_ = false, waitingStart_ = false, prepared_ = false;
    bool startCommitted_ = false, launchReported_ = false;
    std::uint32_t startRequest_ = 0;
    std::chrono::steady_clock::time_point launchDeadline_{};
    std::string peerName_;
    bool frostpunkMissingReported_ = false;
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
