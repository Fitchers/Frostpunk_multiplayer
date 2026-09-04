// No game process, injection or Steam is used by these routing tests.
#include "../src/FrostMenuMod/FrostMenuMod.cpp"
#include <cassert>
#include <iostream>

namespace {
int originalCalls = 0;
int openedForms = 0;
int openedTransport = -1;
void recordOpen(int index) { ++openedForms; openedTransport = index; }
void __fastcall originalCallback(void*, void*) { ++originalCalls; }
}

int main() {
    g_originalScenarioRowCallback = originalCallback;
    g_originalScenariosStartCallback = originalCallback;
    g_openConnectionForm = recordOpen;
    alignas(8) std::uint8_t event[0x20]{};
    auto& index = *reinterpret_cast<std::uint32_t*>(event + 0x18);

    g_multiplayerMode = false;
    scenarioRowCallbackHook(nullptr, event);
    assert(originalCalls == 1); // Ordinary scenarios retain their callback.

    g_multiplayerMode = true;
    g_selectedConnection = -1;
    scenariosStartCallbackHook(nullptr, event);
    assert(originalCalls == 1); // No selection cannot start a real scenario.
    assert(openedForms == 0);
    index = 1;
    scenarioRowCallbackHook(nullptr, event);
    assert(g_selectedConnection == 1);
    assert(openedForms == 1 && openedTransport == 1); // Single LAN click opens form.
    assert(originalCalls == 1); // LAN cannot enter scenario/DLC logic.
    index = 0;
    scenarioRowCallbackHook(nullptr, event);
    assert(g_selectedConnection == 0);
    assert(openedForms == 2 && openedTransport == 0); // Steam also opens directly.
    index = 2;
    scenarioRowCallbackHook(nullptr, event);
    assert(g_selectedConnection == 0); // Hidden scenarios are ignored.
    scenarioRowCallbackHook(nullptr, nullptr);
    assert(g_selectedConnection == 0);
    assert(openedForms == 2); // Hidden/invalid rows never launch anything.

    g_dialogOpen = true; // Avoid a real MessageBox in this headless test.
    scenariosStartCallbackHook(nullptr, event);
    assert(openedForms == 3 && openedTransport == 0); // Legacy Select still works.
    assert(originalCalls == 1);
    g_multiplayerMode = false;
    scenariosStartCallbackHook(nullptr, event);
    assert(originalCalls == 2);
    assert(openedForms == 3);
    std::cout << "PASS: Steam/LAN routing, invalid selection, native scenario isolation.\n";
}
