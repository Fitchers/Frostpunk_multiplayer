#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../src/FrostBridgeNet/ResourceReader.h"
#include "../src/FrostBridgeNet/CityVitals.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

namespace r = frostbridge::resources;
constexpr std::uintptr_t base = 0x140000000, economy = 0x200000000, records = 0x300000000;
struct Row { std::string name, key; std::int32_t amount; };
const std::vector<Row> standard{
    {"Coal", "res_coal", 50}, {"Wood", "res_wood", 30},
    {"Steel", "res_steel", 20}, {"Steam Cores", "res_cores", 3},
    {"Wooden Materials", "res_scaffolding", 0}, {"Steel Plates", "res_steelplates", 0},
    {"Steam Tools", "res_pipes", 0}, {"Raw Food", "res_food", 80},
    {"Food Rations", "res_food_rations", 0}, {"Bodies", "res_bodies", 0},
    {"Augmentation parts", "res_augparts", 0}
};
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

struct Memory {
    std::map<std::uintptr_t, unsigned char> data;
    void bytes(std::uintptr_t address, const void* source, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(source);
        for (std::size_t i = 0; i < size; ++i) data[address + i] = p[i];
    }
    template<class T> void put(std::uintptr_t address, T item) { bytes(address, &item, sizeof(item)); }
    bool read(std::uintptr_t address, void* destination, std::size_t size) const {
        auto* p = static_cast<unsigned char*>(destination);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = data.find(address + i);
            if (it == data.end()) return false;
            p[i] = it->second;
        }
        return true;
    }
    explicit Memory(const std::vector<Row>& rows) {
        put(base + r::economyRva, economy);
        put(economy, base + r::economyVtableRva);
        put(economy + r::recordsOffset, records);
        put(economy + r::countOffset, static_cast<std::uint32_t>(rows.size()));
        for (std::size_t i = 0; i < rows.size(); ++i) {
            std::array<std::byte, r::recordSize> zero{};
            bytes(records + i * r::recordSize, zero.data(), zero.size());
            const auto entry = 0x400000000 + i * 0x1000;
            put(records + i * r::recordSize, entry);
            put(records + i * r::recordSize + 8, rows[i].amount);
            put(entry, entry + 0x200);
            put(entry + 0xC0, entry + 0x300);
            bytes(entry + 0x200, rows[i].name.c_str(), rows[i].name.size() + 1);
            bytes(entry + 0x300, rows[i].key.c_str(), rows[i].key.size() + 1);
        }
    }
    std::optional<r::Snapshot> snapshot() const {
        return r::readSnapshot([&](auto a, auto d, auto n) { return read(a, d, n); }, base);
    }
};

void tests() {
    std::array<int, 6> permutation{0, 1, 2, 3, 7, 8};
    do {
        std::vector<Row> rows;
        for (const auto index : permutation) rows.push_back(standard[index]);
        const auto s = Memory(rows).snapshot();
        require(s && s->coal == 50 && s->wood == 30 && s->steel == 20 && s->steamCores == 3 &&
                s->rawFood == 80 && s->foodRations == 0, "Six resource permutation failed");
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    auto rows = standard;
    std::mt19937 rng(480);
    for (int i = 0; i < 100; ++i) {
        std::shuffle(rows.begin(), rows.end(), rng);
        const auto s = Memory(rows).snapshot();
        require(s && s->coal == 50 && s->wood == 30 && s->steel == 20 && s->steamCores == 3 &&
                s->rawFood == 80 && s->foodRations == 0,
                "Shuffled extended resource table failed");
    }
    for (const int amount : {0, 3, 20}) {
        rows = standard;
        for (auto& row : rows) row.amount = amount;
        const auto s = Memory(rows).snapshot();
        require(s && s->coal == amount && s->wood == amount && s->steel == amount && s->steamCores == amount &&
                s->rawFood == amount && s->foodRations == amount,
                "Identity must not depend on quantity, including zero");
    }
    for (const auto index : {7, 8}) {
        rows = standard; rows.erase(rows.begin() + index);
        require(!Memory(rows).snapshot(), "Missing food resource accepted");
        rows = standard; rows.push_back(rows[index]);
        require(!Memory(rows).snapshot(), "Duplicate food resource accepted");
    }
    rows = standard; std::swap(rows[7].key, rows[8].key);
    require(!Memory(rows).snapshot(), "Swapped food identities accepted");
    rows = standard; rows[7].amount = 15; rows[8].amount = 90;
    require(Memory(rows).snapshot()->rawFood == 15 && Memory(rows).snapshot()->foodRations == 90,
            "Food quantity hardcoded or swapped");
    rows = standard; rows.erase(rows.begin());
    require(!Memory(rows).snapshot(), "Missing coal accepted");
    rows = standard; rows.push_back(rows.front());
    require(!Memory(rows).snapshot(), "Duplicate coal accepted");
    rows = standard; rows[0].key = "res_wood";
    require(!Memory(rows).snapshot(), "Conflicting name/key accepted");
    rows = standard; rows[0].name = "coal";
    require(!Memory(rows).snapshot(), "Unverified name accepted");
    rows = standard; rows[0].amount = -1;
    require(!Memory(rows).snapshot(), "Negative resource accepted");
    auto m = Memory(standard);
    m.data.erase(records + 8);
    require(!m.snapshot(), "Partial table read accepted");
    m = Memory(standard); m.put(economy + r::countOffset, std::uint32_t(65));
    require(!m.snapshot(), "Unbounded count accepted");
    m = Memory(standard); m.put(economy, base);
    require(!m.snapshot(), "Wrong economy vtable accepted");
    m = Memory(standard); m.put(base + r::economyRva, std::uintptr_t(0));
    require(!m.snapshot(), "Unloaded city accepted");
    m = Memory(standard); m.data.erase(0x400000200);
    require(!m.snapshot(), "Unreadable name accepted");
    m = Memory(standard);
    for (std::size_t i = 0; i < 64; ++i) m.put(0x400000200 + i, 'C');
    require(!m.snapshot(), "Unterminated name accepted");
    m = Memory(standard);
    unsigned int rootReads = 0;
    const auto reloaded = r::readSnapshot([&](auto a, auto d, auto n) {
        if (a == base + r::economyRva && ++rootReads == 2) m.put(a, economy + 0x10000);
        return m.read(a, d, n);
    }, base);
    require(!reloaded, "Reload during snapshot accepted");
    m = Memory(standard);
    const auto reordered = r::readSnapshot([&](auto a, auto d, auto n) {
        if (a == records && n == sizeof(std::uintptr_t)) m.put(records, std::uintptr_t(0x400001000));
        return m.read(a, d, n);
    }, base);
    require(!reordered, "Record identity changed during snapshot accepted");
    // Same reader called again after a city/array change must not reuse indices.
    m = Memory(standard);
    require(m.snapshot()->coal == 50, "Initial city failed");
    rows = standard; rows[0].amount = 18; rows[7].amount = 9; rows[8].amount = 72;
    std::reverse(rows.begin(), rows.end());
    m = Memory(rows);
    require(m.snapshot()->coal == 18 && m.snapshot()->steamCores == 3 &&
            m.snapshot()->rawFood == 9 && m.snapshot()->foodRations == 72, "Stale city cache");
    std::cout << "PASS: 720 six-resource permutations, 100 shuffled cities, equal/zero amounts, identity validation, "
                 "missing/duplicate resources, read failures, reload/reorder guards, no stale cache.\n";
}

int main(int argc, char** argv) {
    try {
        tests();
        Memory m(standard);
        constexpr std::uintptr_t hope = 0x500000000, discontent = 0x600000000;
        m.put(base + 0x3FD3DE0, hope); m.put(hope, base + 0x2013278);
        m.put(base + 0x3FD1410, discontent); m.put(discontent, base + 0x1FF0140);
        m.put(hope + 0x2160, std::uint8_t(1)); m.put(hope + 0x2161, std::uint8_t(1));
        m.put(discontent + 0x2198, std::uint8_t(1)); m.put(discontent + 0x2199, std::uint8_t(1));
        m.put(hope + 0x2164, 32.0f); m.put(hope + 0x2168, std::int32_t(80));
        m.put(discontent + 0x21E8, 20.0f); m.put(discontent + 0x21EC, std::int32_t(80));
        auto vitals = [&] { return frostbridge::vitals::readSnapshot(
            [&](auto a, auto d, auto n) { return m.read(a,d,n); }, base); };
        require(vitals().hope == 4000 && vitals().discontent == 2500, "Wrong native averages");
        m.put(hope + 0x2161, std::uint8_t(0));
        require(vitals().hope == -1, "Disabled hope must be unavailable");
        m.put(hope + 0x2161, std::uint8_t(1));
        m.put(hope + 0x2164, std::numeric_limits<float>::quiet_NaN());
        require(vitals().hope == -1, "Non-finite hope accepted");
        m.put(discontent, base);
        require(vitals().discontent == -1, "Wrong vitals vtable accepted");
        std::cout << "PASS: native vitals averages, disabled/missing fields and invalid values.\n";
        // Optional read-only integration check using the production reader.
        if (argc == 3) {
            const auto pid = static_cast<DWORD>(std::stoul(argv[1]));
            const auto module = static_cast<std::uintptr_t>(std::stoull(argv[2], nullptr, 0));
            const auto process = OpenProcess(PROCESS_VM_READ, FALSE, pid);
            require(process != nullptr, "Cannot open game for read-only check");
            const auto s = r::readSnapshot([&](auto a, auto d, auto n) {
                SIZE_T got = 0;
                return ReadProcessMemory(process, reinterpret_cast<const void*>(a), d, n, &got) && got == n;
            }, module);
            const auto v = frostbridge::vitals::readSnapshot([&](auto a, auto d, auto n) {
                SIZE_T got = 0;
                return ReadProcessMemory(process, reinterpret_cast<const void*>(a), d, n, &got) && got == n;
            }, module);
            CloseHandle(process);
            require(s.has_value(), "Live city resources unavailable");
            std::cout << "PID " << pid << ": coal=" << s->coal << " wood=" << s->wood
                      << " steel=" << s->steel << " steamCores=" << s->steamCores
                      << " rawFood=" << s->rawFood << " foodRations=" << s->foodRations
                      << " hope=" << v.hope / 100.0 << "% discontent=" << v.discontent / 100.0 << "%\n";
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
