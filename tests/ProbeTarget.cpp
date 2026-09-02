#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <iostream>
#include <string>

volatile std::int32_t g_probeValue = 0;
volatile std::int32_t g_probeTag = 0;

__declspec(noinline) void applyDelta(volatile std::int32_t* target,
                                     volatile std::int32_t* tag,
                                     std::int32_t delta) {
    if (tag == &g_probeTag) *target += delta;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    g_probeValue = static_cast<std::int32_t>(std::stoll(argv[1]));
    std::cout << "PID=" << GetCurrentProcessId()
              << " address=0x" << std::hex
              << reinterpret_cast<std::uintptr_t>(&g_probeValue)
              << " function=0x" << reinterpret_cast<std::uintptr_t>(&applyDelta)
              << " tag=0x" << reinterpret_cast<std::uintptr_t>(&g_probeTag)
              << std::dec << " value=" << g_probeValue << std::endl;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "q") break;
        applyDelta(&g_probeValue, &g_probeTag, static_cast<std::int32_t>(std::stoll(line)));
        std::cout << "value=" << g_probeValue << std::endl;
    }
    return 0;
}
