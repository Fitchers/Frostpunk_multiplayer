#include "../src/FrostMenuMod/FrostMenuMod.cpp"
#include <cassert>
#include <iostream>

int main() {
    frostlaunch::Control control{};
    g_launchControl = &control;
    alignas(8) unsigned char host[0x220]{}, client[0x220]{};
    alignas(8) unsigned char hostSliders[4][0x320]{}, clientSliders[4][0x320]{};
    void* hostEntries[8]{}, *clientEntries[8]{};
    for (int i = 0; i < 4; ++i) {
        hostEntries[i * 2 + 1] = hostSliders[i];
        clientEntries[i * 2 + 1] = clientSliders[i];
        *reinterpret_cast<int*>(hostSliders[i] + 0x314) = 3;
    }
    for (bool story : {true, false}) {
        const auto offset = story ? 0x1C8 : 0x1B0;
        *reinterpret_cast<void**>(host + offset) = hostEntries;
        *reinterpret_cast<void**>(client + offset) = clientEntries;
        *reinterpret_cast<int*>(host + offset + 8) = 4;
        *reinterpret_cast<int*>(client + offset + 8) = 4;
        assert(readLaunchDifficulty(host, story, control.difficulty));
        assert(applyLaunchDifficulty(client, story));
        frostlaunch::Difficulty observed{};
        assert(readLaunchDifficulty(client, story, observed));
        assert(observed == control.difficulty);
        for (int i = 0; i < 4; ++i) {
            control.difficulty.levels[i] = i; // Custom per-category difficulty.
        }
        assert(applyLaunchDifficulty(client, story));
        assert(readLaunchDifficulty(client, story, observed));
        assert(observed == control.difficulty);
        control.difficulty.levels[0] = 99;
        assert(!applyLaunchDifficulty(client, story));
        control.difficulty.levels[0] = 0;
        control.difficulty.count = 5;
        assert(!applyLaunchDifficulty(client, story));
    }
    assert(!readLaunchDifficulty(nullptr, true, control.difficulty));
    std::cout << "Difficulty: extreme, custom categories, incompatible count and invalid input passed.\n";
}
