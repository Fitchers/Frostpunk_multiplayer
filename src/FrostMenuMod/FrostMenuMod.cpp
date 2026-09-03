#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::uintptr_t kMainMenuUpdateRva = 0x1A770F0;
constexpr std::uintptr_t kMainMenuBindButtonsRva = 0x1A76B80;
constexpr std::uintptr_t kMenuPanelCallbackRva = 0x1A748E0;
constexpr std::uintptr_t kMainMenuVtableRva = 0x204C488;
constexpr std::uintptr_t kUIButtonVtableRva = 0x1D75E08;

constexpr std::uintptr_t kStringCtorRva = 0xE48BA0;
constexpr std::uintptr_t kStringDtorRva = 0xE47C30;
constexpr std::uintptr_t kFindChildRva = 0xF77CC0;
constexpr std::uintptr_t kCastUITextRva = 0x147B790;
constexpr std::uintptr_t kSetUITextRva = 0x106B5F0;
constexpr std::uintptr_t kSetVisibilityRva = 0xFCD9F0;
constexpr std::uintptr_t kSetUiTransformRva = 0x10504B0;

constexpr std::size_t kCustomScenarioButtonOffset = 0xF0;
constexpr std::size_t kContinueButtonOffset = 0xD0;
constexpr std::size_t kUiElementTransformOffset = 0xC0;
// The stock menu rows are spaced by 44 UI units (Continue -> Scenario -> ...).
constexpr float kMultiplayerVerticalGap = 44.0f;
// The hidden custom row's authored label is offset to the right by this amount.
// Compensate horizontally so its title and summary share the stock column center.
constexpr float kMultiplayerHorizontalOffset = 60.0f;
constexpr std::uint32_t kCustomScenarioPanelId = 0x60;

// Verified against Frostpunk.exe SHA-256
// 719c6e016bcdb1021a6401b1624aebdcfb03c6782a763d2fbd8c2805f9dae9d4.
constexpr std::uint8_t kExpectedUpdatePrologue[] = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x8D, 0xA8, 0x98, 0xF7, 0xFF, 0xFF,
};
constexpr std::uint8_t kExpectedBindButtonsPrologue[] = {
    0x40, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8B, 0xEC, 0x48, 0x83,
    0xEC, 0x40, 0x48, 0xC7, 0x45, 0xF0, 0xFE, 0xFF, 0xFF, 0xFF,
};
constexpr std::uint8_t kExpectedCallbackPrologue[] = {
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7,
    0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF,
};

struct EngineString {
    char* data = nullptr;
};

struct TextUpdateOptions {
    std::uint8_t resetLayout = 0;
    std::uint8_t refreshImmediately = 1;
    std::uint16_t reserved = 0;
    float animationTime = 0.0f;
    std::int32_t context = 0;
};

struct alignas(16) UiTransform {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

using MainMenuUpdate = void(__fastcall*)(void* panel);
using MainMenuBindButtons = void(__fastcall*)(void* panel);
using MenuPanelCallback = void(__fastcall*)(void* owner, void* event);
using StringCtor = EngineString*(__fastcall*)(EngineString* value, const char* text);
using StringDtor = void(__fastcall*)(EngineString* value);
using FindChild = void*(__fastcall*)(void* element, const EngineString* name);
using CastUIText = void*(__fastcall*)(void* element);
using SetUIText = void(__fastcall*)(void* text, const EngineString* value,
                                    const TextUpdateOptions* options);
using SetVisibility = void(__fastcall*)(void* element, bool visible,
                                        bool includeChildren, bool animated);
using SetUiTransform = void(__fastcall*)(void* element, const UiTransform* transform);

HMODULE g_module = nullptr;
std::uintptr_t g_gameBase = 0;
MainMenuUpdate g_originalMainMenuUpdate = nullptr;
MainMenuBindButtons g_originalMainMenuBindButtons = nullptr;
MenuPanelCallback g_originalMenuPanelCallback = nullptr;
std::atomic<void*> g_lastConfiguredPanel = nullptr;
std::atomic_flag g_applyLock = ATOMIC_FLAG_INIT;
std::atomic<bool> g_dialogOpen = false;

void logLine(const wchar_t* message) {
    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(g_module, modulePath, MAX_PATH)) return;
    wchar_t* separator = wcsrchr(modulePath, L'\\');
    if (!separator) return;
    wcscpy_s(separator + 1, MAX_PATH - static_cast<std::size_t>(separator + 1 - modulePath),
             L"FrostMenuMod.log");

    HANDLE file = CreateFileW(modulePath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t line[1024]{};
    _snwprintf_s(line, _TRUNCATE, L"[%02u:%02u:%02u.%03u] %s\r\n",
                 now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, message);
    const int required = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (required > 1) {
        std::string utf8(static_cast<std::size_t>(required), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), required, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size() - 1), &written, nullptr);
    }
    CloseHandle(file);
}

template <typename T>
bool safeRead(const void* address, T& value) {
    __try {
        value = *static_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void writeAbsoluteJump(std::uint8_t* destination, const void* target) {
    // jmp qword ptr [rip+0]; followed by the destination address.
    destination[0] = 0xFF;
    destination[1] = 0x25;
    std::memset(destination + 2, 0, 4);
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    std::memcpy(destination + 6, &address, sizeof(address));
}

class InlineHook {
public:
    bool install(void* target, void* replacement, const std::uint8_t* expected,
                 std::size_t stolenLength) {
        if (stolenLength < kJumpSize || std::memcmp(target, expected, stolenLength) != 0) {
            return false;
        }

        target_ = static_cast<std::uint8_t*>(target);
        stolenLength_ = stolenLength;
        original_.assign(target_, target_ + stolenLength_);
        trampoline_ = static_cast<std::uint8_t*>(VirtualAlloc(
            nullptr, stolenLength_ + kJumpSize, MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
        if (!trampoline_) return false;

        std::memcpy(trampoline_, original_.data(), stolenLength_);
        writeAbsoluteJump(trampoline_ + stolenLength_, target_ + stolenLength_);

        DWORD oldProtection = 0;
        if (!VirtualProtect(target_, stolenLength_, PAGE_EXECUTE_READWRITE, &oldProtection)) {
            VirtualFree(trampoline_, 0, MEM_RELEASE);
            trampoline_ = nullptr;
            return false;
        }
        writeAbsoluteJump(target_, replacement);
        if (stolenLength_ > kJumpSize) {
            std::memset(target_ + kJumpSize, 0x90, stolenLength_ - kJumpSize);
        }
        FlushInstructionCache(GetCurrentProcess(), target_, stolenLength_);
        DWORD ignored = 0;
        VirtualProtect(target_, stolenLength_, oldProtection, &ignored);
        return true;
    }

    template <typename T>
    T original() const {
        return reinterpret_cast<T>(trampoline_);
    }

private:
    static constexpr std::size_t kJumpSize = 14;
    std::uint8_t* target_ = nullptr;
    std::uint8_t* trampoline_ = nullptr;
    std::size_t stolenLength_ = 0;
    std::vector<std::uint8_t> original_;
};

InlineHook g_updateHook;
InlineHook g_bindButtonsHook;
InlineHook g_callbackHook;

void* menuButton(void* panel, std::size_t buttonOffset) {
    void* holder = nullptr;
    if (!safeRead(static_cast<std::uint8_t*>(panel) + buttonOffset, holder) ||
        !holder) {
        return nullptr;
    }
    void* button = nullptr;
    if (!safeRead(holder, button) || !button) return nullptr;
    std::uintptr_t vtable = 0;
    if (!safeRead(button, vtable) || vtable != g_gameBase + kUIButtonVtableRva) return nullptr;
    return button;
}

void* multiplayerButton(void* panel) {
    return menuButton(panel, kCustomScenarioButtonOffset);
}

void* findText(void* button, const char* elementName) {
    const auto stringCtor = reinterpret_cast<StringCtor>(g_gameBase + kStringCtorRva);
    const auto stringDtor = reinterpret_cast<StringDtor>(g_gameBase + kStringDtorRva);
    const auto findChild = reinterpret_cast<FindChild>(g_gameBase + kFindChildRva);
    const auto castText = reinterpret_cast<CastUIText>(g_gameBase + kCastUITextRva);

    EngineString name{};
    stringCtor(&name, elementName);
    void* element = findChild(button, &name);
    stringDtor(&name);
    return element ? castText(element) : nullptr;
}

bool setText(void* textElement, const char* utf8Text) {
    if (!textElement) return false;
    const auto stringCtor = reinterpret_cast<StringCtor>(g_gameBase + kStringCtorRva);
    const auto stringDtor = reinterpret_cast<StringDtor>(g_gameBase + kStringDtorRva);
    const auto setUiText = reinterpret_cast<SetUIText>(g_gameBase + kSetUITextRva);

    EngineString text{};
    stringCtor(&text, utf8Text);
    TextUpdateOptions options{};
    setUiText(textElement, &text, &options);
    stringDtor(&text);
    return true;
}

bool configureMenuPanel(void* panel) {
    if (!panel || g_applyLock.test_and_set(std::memory_order_acquire)) return false;

    bool configured = false;
    __try {
        if (void* button = multiplayerButton(panel)) {
            const auto setVisibility =
                reinterpret_cast<SetVisibility>(g_gameBase + kSetVisibilityRva);
            void* summary = findText(button, "STORY_UNLOCKED");

            if (g_lastConfiguredPanel.load(std::memory_order_relaxed) != panel) {
                // Liquid Engine's narrow-string converter uses Windows-1251 in
                // this Russian build (not UTF-8).
                constexpr char kTitle[] =
                    "\xCC\xD3\xCB\xDC\xD2\xC8\xCF\xCB\xC5\xC5\xD0";
                constexpr char kSummary[] =
                    "* \xCD\xCE\xC2\xDB\xC9 CO-OP "
                    "\xD0\xC5\xC6\xC8\xCC *";

                // These are the real names from the 1.6.1 UI tree. The names used
                // by the menu's localization keys are not the element names.
                void* title = findText(button, "MENU_OPTION_TEXT");
                const bool titleSet = setText(title, kTitle);
                const bool summarySet = setText(summary, kSummary);
                if (titleSet) {
                    g_lastConfiguredPanel.store(panel, std::memory_order_relaxed);
                    logLine(summarySet
                                ? L"Native multiplayer menu label and summary configured."
                                : L"Native multiplayer menu label configured; summary was not found.");
                } else {
                    logLine(L"The multiplayer button was found, but its title element was not.");
                }
            }

            setVisibility(button, true, true, false);
            // The button's on-show routine restores STORY_UNLOCKED to hidden, so
            // the gold co-op badge has to be enabled afterwards.
            if (summary) setVisibility(summary, true, true, false);

            // CUSTOM_SCENARIO_BUTTON is authored as a hidden item almost on top
            // of CONTINUE_BUTTON. Use the engine setter after visibility because
            // a raw write leaves its cached render transform inconsistent.
            if (void* continueButton = menuButton(panel, kContinueButtonOffset)) {
                UiTransform transform{};
                UiTransform continueTransform{};
                if (safeRead(static_cast<std::uint8_t*>(button) +
                                 kUiElementTransformOffset,
                             transform) &&
                    safeRead(static_cast<std::uint8_t*>(continueButton) +
                                 kUiElementTransformOffset,
                             continueTransform)) {
                    transform.x = continueTransform.x - kMultiplayerHorizontalOffset;
                    transform.y = continueTransform.y - kMultiplayerVerticalGap;
                    const auto setTransform = reinterpret_cast<SetUiTransform>(
                        g_gameBase + kSetUiTransformRva);
                    setTransform(button, &transform);
                }
            }
            configured = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine(L"Game rejected a UI call while configuring the multiplayer item.");
    }

    g_applyLock.clear(std::memory_order_release);
    return configured;
}

void __fastcall mainMenuUpdateHook(void* panel) {
    g_originalMainMenuUpdate(panel);
    configureMenuPanel(panel);
}

void __fastcall mainMenuBindButtonsHook(void* panel) {
    g_originalMainMenuBindButtons(panel);
    configureMenuPanel(panel);
}

HWND findGameWindow() {
    struct Search {
        DWORD processId;
        HWND window;
    } search{GetCurrentProcessId(), nullptr};

    EnumWindows(
        [](HWND window, LPARAM value) -> BOOL {
            auto* search = reinterpret_cast<Search*>(value);
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId == search->processId && IsWindowVisible(window) &&
                GetWindow(window, GW_OWNER) == nullptr) {
                search->window = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.window;
}

DWORD WINAPI showPrototypeDialog(void*) {
    MessageBoxW(findGameWindow(),
                L"Пункт меню работает.\n\n"
                L"Следующий этап — экран создания и подключения к сессии, "
                L"затем синхронизация команд между двумя компьютерами.",
                L"FrostBridge — Мультиплеер", MB_OK | MB_ICONINFORMATION);
    g_dialogOpen.store(false, std::memory_order_release);
    return 0;
}

void __fastcall menuPanelCallbackHook(void* owner, void* event) {
    std::uint32_t panelId = 0;
    if (event && safeRead(static_cast<std::uint8_t*>(event) + 0x18, panelId) &&
        panelId == kCustomScenarioPanelId) {
        if (!g_dialogOpen.exchange(true, std::memory_order_acq_rel)) {
            HANDLE thread = CreateThread(nullptr, 0, showPrototypeDialog, nullptr, 0, nullptr);
            if (thread) {
                CloseHandle(thread);
            } else {
                g_dialogOpen.store(false, std::memory_order_release);
            }
        }
        return;
    }
    g_originalMenuPanelCallback(owner, event);
}

void* locateLiveMainMenuPanel() {
    const std::uintptr_t expectedVtable = g_gameBase + kMainMenuVtableRva;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto cursor = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);

    while (cursor < maximum) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &memory, sizeof(memory))) break;
        const auto regionBase = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto regionEnd = regionBase + memory.RegionSize;
        const DWORD protection = memory.Protect & 0xff;
        const bool readablePrivate = memory.State == MEM_COMMIT && memory.Type == MEM_PRIVATE &&
            !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
             protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY);

        if (readablePrivate) {
            const auto first = (regionBase + alignof(std::uintptr_t) - 1) &
                               ~(alignof(std::uintptr_t) - 1);
            __try {
                const auto firstByte = static_cast<unsigned char>(expectedVtable & 0xff);
                auto* match = reinterpret_cast<const unsigned char*>(first);
                const auto* end = reinterpret_cast<const unsigned char*>(regionEnd);
                while (match + sizeof(std::uintptr_t) <= end) {
                    match = static_cast<const unsigned char*>(
                        std::memchr(match, firstByte, static_cast<std::size_t>(end - match)));
                    if (!match) break;
                    const auto address = reinterpret_cast<std::uintptr_t>(match);
                    if ((address % alignof(std::uintptr_t)) == 0 &&
                        address + sizeof(std::uintptr_t) <= regionEnd &&
                        *reinterpret_cast<const std::uintptr_t*>(address) == expectedVtable &&
                        multiplayerButton(reinterpret_cast<void*>(address))) {
                        return reinterpret_cast<void*>(address);
                    }
                    ++match;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // A custom heap can release a region between VirtualQuery and the scan.
            }
        }
        if (regionEnd <= cursor) break;
        cursor = regionEnd;
    }
    return nullptr;
}

bool installHooks() {
    auto* updateTarget = reinterpret_cast<void*>(g_gameBase + kMainMenuUpdateRva);
    auto* bindButtonsTarget = reinterpret_cast<void*>(g_gameBase + kMainMenuBindButtonsRva);
    auto* callbackTarget = reinterpret_cast<void*>(g_gameBase + kMenuPanelCallbackRva);

    if (!g_callbackHook.install(callbackTarget, reinterpret_cast<void*>(menuPanelCallbackHook),
                                kExpectedCallbackPrologue,
                                sizeof(kExpectedCallbackPrologue))) {
        logLine(L"Callback hook signature mismatch; unsupported Frostpunk build.");
        return false;
    }
    g_originalMenuPanelCallback = g_callbackHook.original<MenuPanelCallback>();

    if (!g_updateHook.install(updateTarget, reinterpret_cast<void*>(mainMenuUpdateHook),
                              kExpectedUpdatePrologue, sizeof(kExpectedUpdatePrologue))) {
        logLine(L"Main-menu update hook signature mismatch; unsupported Frostpunk build.");
        return false;
    }
    g_originalMainMenuUpdate = g_updateHook.original<MainMenuUpdate>();

    if (!g_bindButtonsHook.install(bindButtonsTarget,
                                   reinterpret_cast<void*>(mainMenuBindButtonsHook),
                                   kExpectedBindButtonsPrologue,
                                   sizeof(kExpectedBindButtonsPrologue))) {
        logLine(L"Main-menu button-binding hook signature mismatch; unsupported build.");
        return false;
    }
    g_originalMainMenuBindButtons =
        g_bindButtonsHook.original<MainMenuBindButtons>();
    logLine(L"FrostMenuMod hooks installed.");
    return true;
}

DWORD WINAPI workerThread(void*) {
    g_gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Frostpunk.exe"));
    if (!g_gameBase) {
        logLine(L"Frostpunk.exe module was not found.");
        return 1;
    }
    if (!installHooks()) return 2;

    // The menu can already be open or its resources can finish loading well after
    // injection. The binding hook handles normal creation; this bounded fallback
    // makes attaching from an already-open menu reliable too.
    logLine(L"Waiting for a live main-menu panel.");
    for (int attempt = 0;
         attempt < 120 && !g_lastConfiguredPanel.load(std::memory_order_acquire);
         ++attempt) {
        Sleep(500);
        if (void* panel = locateLiveMainMenuPanel()) {
            logLine(L"Live main-menu panel found.");
            if (configureMenuPanel(panel)) break;
            if ((attempt % 10) == 9) {
                logLine(L"The panel exists but is currently being configured by the game thread.");
            }
        }
    }
    if (!g_lastConfiguredPanel.load(std::memory_order_acquire)) {
        logLine(L"No configurable main-menu panel appeared within 60 seconds.");
    }
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, workerThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
