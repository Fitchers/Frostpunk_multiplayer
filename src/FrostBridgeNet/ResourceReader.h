#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace frostbridge::resources {

// Layout recovered for the executable documented in recovered-economy-api.md.
// Array order is NOT a resource ID and can change between cities/processes.
inline constexpr std::uintptr_t economyRva = 0x3FC93B0;
inline constexpr std::uintptr_t economyVtableRva = 0x1FA66A0;
inline constexpr std::uintptr_t recordsOffset = 0x2240;
inline constexpr std::uintptr_t countOffset = 0x2248;
inline constexpr std::size_t recordSize = 0x70;
inline constexpr std::size_t maxRecords = 64;

struct Snapshot {
    std::int32_t coal, wood, steel, steamCores, rawFood, foodRations;
};

template<class T, class Reader>
std::optional<T> value(Reader& read, std::uintptr_t address) {
    T result{};
    if (!read(address, &result, sizeof(result))) return std::nullopt;
    return result;
}

inline bool pointer(std::uintptr_t address) {
    return address >= 0x10000 && address < 0x00007FFFFFFF0000ULL;
}

template<class Reader>
std::optional<std::string> text(Reader& read, std::uintptr_t address) {
    if (!pointer(address)) return std::nullopt;
    std::string result;
    // Byte reads do not over-read the allocation/page after a short string.
    for (std::size_t i = 0; i < 64; ++i) {
        const auto ch = value<char>(read, address + i);
        if (!ch) return std::nullopt;
        if (*ch == '\0') return result;
        if (*ch < 32 || *ch > 126) return std::nullopt;
        result += *ch;
    }
    return std::nullopt;
}

// Reader has signature bool(uintptr_t address, void* destination, size_t bytes).
// No game writes, amount-based guesses, or cached pointers/indices across ticks.
template<class Reader>
std::optional<Snapshot> readSnapshot(Reader read, std::uintptr_t moduleBase) {
    const auto economy = value<std::uintptr_t>(read, moduleBase + economyRva);
    if (!economy || !pointer(*economy)) return std::nullopt;
    if (value<std::uintptr_t>(read, *economy) != moduleBase + economyVtableRva)
        return std::nullopt;
    const auto records = value<std::uintptr_t>(read, *economy + recordsOffset);
    const auto count = value<std::uint32_t>(read, *economy + countOffset);
    if (!records || !pointer(*records) || !count || *count < 6 || *count > maxRecords)
        return std::nullopt;

    std::array<std::byte, maxRecords * recordSize> table{};
    if (!read(*records, table.data(), *count * recordSize)) return std::nullopt;
    constexpr std::array<std::string_view, 6> names{
        "Coal", "Wood", "Steel", "Steam Cores", "Raw Food", "Food Rations"};
    constexpr std::array<std::string_view, 6> keys{
        "res_coal", "res_wood", "res_steel", "res_cores", "res_food", "res_food_rations"};
    std::array<std::optional<std::int32_t>, 6> amounts{};
    for (std::size_t i = 0; i < *count; ++i) {
        std::uintptr_t entry = 0;
        std::int32_t amount = 0;
        std::memcpy(&entry, table.data() + i * recordSize, sizeof(entry));
        std::memcpy(&amount, table.data() + i * recordSize + 8, sizeof(amount));
        if (!pointer(entry)) return std::nullopt;
        // ResourceEntry +0 is the internal Name, +0xC0 the telemetry key.
        // Neither is a translated display label. Both were verified in two cities.
        const auto namePointer = value<std::uintptr_t>(read, entry);
        const auto keyPointer = value<std::uintptr_t>(read, entry + 0xC0);
        if (!namePointer || !keyPointer) return std::nullopt;
        const auto name = text(read, *namePointer);
        const auto key = text(read, *keyPointer);
        if (!name || !key) return std::nullopt;
        for (std::size_t kind = 0; kind < names.size(); ++kind) {
            if (*name != names[kind] && *key != keys[kind]) continue;
            if (*name != names[kind] || *key != keys[kind] || amounts[kind] || amount < 0)
                return std::nullopt;
            amounts[kind] = amount;
        }
        // Reject a record replaced while its identity was being read.
        if (value<std::uintptr_t>(read, *records + i * recordSize) != entry)
            return std::nullopt;
    }
    // A loading/unloading city must not publish partially resolved/old data.
    if (value<std::uintptr_t>(read, moduleBase + economyRva) != economy ||
        value<std::uintptr_t>(read, *economy + recordsOffset) != records ||
        value<std::uint32_t>(read, *economy + countOffset) != count)
        return std::nullopt;
    for (const auto& amount : amounts) if (!amount) return std::nullopt;
    return Snapshot{*amounts[0], *amounts[1], *amounts[2], *amounts[3], *amounts[4], *amounts[5]};
}

} // namespace frostbridge::resources
