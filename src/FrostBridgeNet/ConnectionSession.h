#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace frostbridge {
struct ConnectionOptions {
    bool steam = false;
    bool host = false;
    std::string playerName;
    std::string address;
    std::uint16_t port = 27020;
    std::uint32_t gamePid = 0;
};
// Owns a networking thread, not a child process. Destruction cancels and joins.
class ConnectionSession {
public:
    virtual ~ConnectionSession() = default;
    virtual void send(std::string command) = 0;
    virtual void stop() = 0;
    virtual std::vector<std::string> takeOutput() = 0;
    virtual bool finished() const = 0;
    virtual bool failed() const = 0;
};
std::unique_ptr<ConnectionSession> connect(ConnectionOptions options);
int runConnectionUI(const std::vector<std::wstring>& arguments);
}
