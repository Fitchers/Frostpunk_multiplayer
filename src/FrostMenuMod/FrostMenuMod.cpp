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
#include "../LaunchControl.h"

namespace {

constexpr std::uintptr_t kMainMenuUpdateRva = 0x1A770F0;
constexpr std::uintptr_t kMainMenuBindButtonsRva = 0x1A76B80;
constexpr std::uintptr_t kMenuPanelCallbackRva = 0x1A748E0;
constexpr std::uintptr_t kScenariosPanelUpdateRva = 0x1A7B180;
constexpr std::uintptr_t kScenariosStartCallbackRva = 0x1A7BEA0;
constexpr std::uintptr_t kScenarioRowCallbackRva = 0x1A7BAF0;
constexpr std::uintptr_t kSetButtonEnabledRva = 0xFCD100;
constexpr std::uintptr_t kBindButtonCallbackRva = 0xFCCF20;
constexpr std::uintptr_t kSetUiStateRva = 0xF75DA0;
constexpr std::uintptr_t kMainMenuVtableRva = 0x204C488;
constexpr std::uintptr_t kScenariosPanelVtableRva = 0x204C610;
constexpr std::uintptr_t kUIButtonVtableRva = 0x1D75E08;

constexpr std::uintptr_t kStringCtorRva = 0xE48BA0;
constexpr std::uintptr_t kStringDtorRva = 0xE47C30;
constexpr std::uintptr_t kFindChildRva = 0xF77CC0;
constexpr std::uintptr_t kCastUITextRva = 0x147B790;
constexpr std::uintptr_t kSetUITextRva = 0x106B5F0;
constexpr std::uintptr_t kSetVisibilityRva = 0xFCD9F0;
constexpr std::uintptr_t kSetUiTransformRva = 0x10504B0;

constexpr std::size_t kMultiplayerButtonOffset = 0xF8;
constexpr std::size_t kPanelRootOffset = 0xA8;
constexpr std::size_t kScenarioSelectionLayoutOffset = 0xD8;
constexpr std::size_t kScenarioStartButtonOffset = 0x110;
constexpr std::size_t kUiElementTransformOffset = 0xC0;
constexpr std::size_t kUiElementFirstChildOffset = 0x220;
constexpr std::size_t kUiElementNextSiblingOffset = 0x240;
constexpr float kInitialMultiplayerTitleOffset = 48.0f;
constexpr std::uint32_t kMultiplayerSourcePanelId = 0x61;
constexpr std::uint32_t kScenariosPanelId = 0x5B;

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
constexpr std::uint8_t kExpectedScenariosUpdatePrologue[] = {
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7,
    0x44, 0x24, 0x30, 0xFE, 0xFF, 0xFF, 0xFF,
};
constexpr std::uint8_t kExpectedScenariosStartPrologue[] = {
    0x48, 0x8B, 0xC4, 0x56, 0x57, 0x41, 0x56,
    0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00,
};
constexpr std::uint8_t kExpectedScenarioRowPrologue[] = {
    0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x54, 0x41,
    0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1,
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
using ScenariosPanelUpdate = void(__fastcall*)(void* panel);
using ScenariosStartCallback = void(__fastcall*)(void* owner, void* event);
using StringCtor = EngineString*(__fastcall*)(EngineString* value, const char* text);
using StringDtor = void(__fastcall*)(EngineString* value);
using FindChild = void*(__fastcall*)(void* element, const EngineString* name);
using CastUIText = void*(__fastcall*)(void* element);
using SetUIText = void(__fastcall*)(void* text, const EngineString* value,
                                    const TextUpdateOptions* options);
using SetVisibility = void(__fastcall*)(void* element, bool visible,
                                        bool includeChildren, bool animated);
using SetUiTransform = void(__fastcall*)(void* element, const UiTransform* transform);
using SetButtonEnabled = void(__fastcall*)(void* button, bool enabled, bool animate);
using BindButtonCallback = void(__fastcall*)(void* button, void* owner,
    MenuPanelCallback callback, int eventType, std::uintptr_t argument, int flags);
using SetUiState = void(__fastcall*)(void* element, const EngineString* state,
    bool enabled, float duration, int flags, bool children, bool immediate,
    int context, void* completion);

HMODULE g_module = nullptr;
std::uintptr_t g_gameBase = 0;
MainMenuUpdate g_originalMainMenuUpdate = nullptr;
MainMenuBindButtons g_originalMainMenuBindButtons = nullptr;
MenuPanelCallback g_originalMenuPanelCallback = nullptr;
ScenariosPanelUpdate g_originalScenariosPanelUpdate = nullptr;
ScenariosStartCallback g_originalScenariosStartCallback = nullptr;
MenuPanelCallback g_originalScenarioRowCallback = nullptr;
std::atomic<int> g_selectedConnection = -1; // 0 Steam, 1 LAN; not a scenario id.
std::atomic<void*> g_lastConfiguredPanel = nullptr;
std::atomic<void*> g_connectionPanel = nullptr;
std::atomic_flag g_applyLock = ATOMIC_FLAG_INIT;
std::atomic<bool> g_dialogOpen = false;
std::atomic<bool> g_multiplayerMode = false;
frostlaunch::Control* g_launchControl = nullptr;
HHOOK g_messageHook = nullptr;
UINT g_launchMessage = 0;
DWORD g_uiThread = 0;
int g_launchStage = 0; // accessed only on the game's window thread
int g_nextMapIndex = 0;
ULONGLONG g_launchDeadline = 0;
void* g_endlessSelection = nullptr;
void* g_endlessConfig = nullptr;
MainMenuUpdate g_originalEndlessBuild = nullptr, g_originalEndlessShow = nullptr;

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
InlineHook g_scenariosUpdateHook;
InlineHook g_scenariosStartHook;
InlineHook g_scenarioRowHook;
InlineHook g_endlessBuildHook, g_endlessShowHook;

void* panelElement(void* panel, std::size_t elementOffset) {
    void* holder = nullptr;
    if (!panel ||
        !safeRead(static_cast<std::uint8_t*>(panel) + elementOffset, holder) ||
        !holder) {
        return nullptr;
    }
    void* element = nullptr;
    return safeRead(holder, element) ? element : nullptr;
}

void* menuButton(void* panel, std::size_t buttonOffset) {
    void* button = panelElement(panel, buttonOffset);
    if (!button) return nullptr;
    std::uintptr_t vtable = 0;
    if (!safeRead(button, vtable) || vtable != g_gameBase + kUIButtonVtableRva) return nullptr;
    return button;
}

void* multiplayerButton(void* panel) {
    return menuButton(panel, kMultiplayerButtonOffset);
}

void* findElement(void* parent, const char* elementName) {
    const auto stringCtor = reinterpret_cast<StringCtor>(g_gameBase + kStringCtorRva);
    const auto stringDtor = reinterpret_cast<StringDtor>(g_gameBase + kStringDtorRva);
    const auto findChild = reinterpret_cast<FindChild>(g_gameBase + kFindChildRva);

    EngineString name{};
    stringCtor(&name, elementName);
    void* element = findChild(parent, &name);
    stringDtor(&name);
    return element;
}

void* findText(void* parent, const char* elementName) {
    const auto castText = reinterpret_cast<CastUIText>(g_gameBase + kCastUITextRva);
    void* element = findElement(parent, elementName);
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
    // Dynamic labels have a different width from the localized stock strings.
    // Ask Liquid Engine to recalculate their centering and clipping bounds.
    options.resetLayout = 1;
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
            void* summary = findText(button, "NEW_MAP_BOUGHT");
            if (!summary) summary = findText(button, "JUST_UNLOCKED");
            if (!summary) summary = findText(button, "STORY_UNLOCKED");

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
                    // Endless' localized caption is substantially wider than
                    // "МУЛЬТИПЛЕЕР". On the first frame Liquid keeps the old
                    // left edge and centers it only after a later panel reopen.
                    // Correct the text anchor once; the stock button container
                    // and its hit box are never moved.
                    UiTransform titleTransform{};
                    if (safeRead(static_cast<std::uint8_t*>(title) +
                                     kUiElementTransformOffset,
                                 titleTransform)) {
                        titleTransform.x += kInitialMultiplayerTitleOffset;
                        const auto setTransform = reinterpret_cast<SetUiTransform>(
                            g_gameBase + kSetUiTransformRva);
                        setTransform(title, &titleTransform);
                    }
                    g_lastConfiguredPanel.store(panel, std::memory_order_relaxed);
                    logLine(summarySet
                                ? L"Native multiplayer menu label and summary configured."
                                : L"Native multiplayer menu label configured; summary was not found.");
                } else {
                    logLine(L"The multiplayer button was found, but its title element was not.");
                }
            }

            setVisibility(button, true, true, false);
            // Keep a gold co-op badge when this version of the Endless template
            // exposes one of its optional notification text elements.
            if (summary) setVisibility(summary, true, true, false);

            configured = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine(L"Game rejected a UI call while configuring the multiplayer item.");
    }

    g_applyLock.clear(std::memory_order_release);
    return configured;
}

void* firstChild(void* element) {
    void* child = nullptr;
    return element && safeRead(static_cast<std::uint8_t*>(element) +
                                   kUiElementFirstChildOffset,
                               child)
        ? child
        : nullptr;
}

void* nextSibling(void* element) {
    void* sibling = nullptr;
    return element && safeRead(static_cast<std::uint8_t*>(element) +
                                   kUiElementNextSiblingOffset,
                               sibling)
        ? sibling
        : nullptr;
}

void setUiState(void* element, const char* name, bool enabled = true) {
    EngineString state{};
    reinterpret_cast<StringCtor>(g_gameBase + kStringCtorRva)(&state, name);
    reinterpret_cast<SetUiState>(g_gameBase + kSetUiStateRva)(
        element, &state, enabled, 0.0f, 0, true, true, 0, nullptr);
    reinterpret_cast<StringDtor>(g_gameBase + kStringDtorRva)(&state);
}

struct ConnectionButtonState {
    void* button = nullptr;
    bool originallyEnabled = false;
    int styledSelection = -2;
};
ConnectionButtonState g_connectionButtons[2];

void restoreConnectionButtons() {
    __try {
        for (auto& saved : g_connectionButtons) {
            std::uintptr_t vtable = 0;
            if (saved.button && safeRead(saved.button, vtable) &&
                vtable == g_gameBase + kUIButtonVtableRva) {
                reinterpret_cast<SetButtonEnabled>(g_gameBase + kSetButtonEnabledRva)(
                    saved.button, saved.originallyEnabled, false);
                setUiState(saved.button, "UNSELECTED");
                setUiState(saved.button, "LOCKED", !saved.originallyEnabled);
            }
            saved = {};
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine(L"Connection panel was destroyed before restoring input state.");
    }
}

bool configureConnectionButton(void* panel, void* wrapper, int index) {
    void* button = findElement(wrapper, "SCENARIO_BUTTON");
    std::uintptr_t vtable = 0;
    if (!button || !safeRead(button, vtable) ||
        vtable != g_gameBase + kUIButtonVtableRva) return false;
    auto& saved = g_connectionButtons[index];
    if (saved.button != button) {
        std::uint32_t flags = 0;
        if (!safeRead(static_cast<std::uint8_t*>(button) + 0x270, flags)) return false;
        saved = {button, (flags & 8) != 0, -2};
    }

    // Locked scenario rows have BOTH no callback and disabled input. Merely
    // hiding DISABLED_TEXT does not change either. Use the same binding and
    // enable APIs as CreateScenarioRow (RVA 1A7AD20), with the intercepted
    // stock callback. BindButtonCallback deduplicates identical bindings.
    reinterpret_cast<BindButtonCallback>(g_gameBase + kBindButtonCallbackRva)(
        button, panel,
        reinterpret_cast<MenuPanelCallback>(g_gameBase + kScenarioRowCallbackRva),
        0, static_cast<std::uintptr_t>(index), 1);
    reinterpret_cast<SetButtonEnabled>(g_gameBase + kSetButtonEnabledRva)(
        button, true, false);

    // Keep selection state entirely in the mod. Never select/unlock the real
    // scenario behind the reused row or mutate profile/DLC entitlement data.
    const int selected = g_selectedConnection.load(std::memory_order_acquire);
    if (saved.styledSelection != selected) {
        setUiState(button, "LOCKED", false);
        setUiState(button, selected == index ? "SELECTED" : "UNSELECTED");
        saved.styledSelection = selected;
    }
    return true;
}

bool configureConnectionPanel(void* panel) {
    if (!panel || !g_multiplayerMode.load(std::memory_order_acquire) ||
        g_applyLock.test_and_set(std::memory_order_acquire)) {
        return false;
    }

    bool configured = false;
    __try {
        void* root = panelElement(panel, kPanelRootOffset);
        void* selectionLayout =
            panelElement(panel, kScenarioSelectionLayoutOffset);
        void* firstWrapper = firstChild(selectionLayout);
        if (root && selectionLayout && firstWrapper) {
            const auto setVisibility =
                reinterpret_cast<SetVisibility>(g_gameBase + kSetVisibilityRva);

            // The Russian build converts narrow strings as Windows-1251.
            constexpr char kConnectionHeader[] =
                "\xC2\xDB\xC1\xD0\xC0\xD2\xDC "
                "\xCF\xCE\xC4\xCA\xCB\xDE\xD7\xC5\xCD\xC8\xC5";
            constexpr char kSteamTitle[] = "STEAM";
            constexpr char kSteamSummary[] = "STEAM P2P";
            constexpr char kLanTitle[] =
                "\xCB\xCE\xCA\xC0\xCB\xDC\xCD\xC0\xDF "
                "\xD1\xC5\xD2\xDC";
            constexpr char kLanSummary[] =
                "\xCF\xD0\xDF\xCC\xCE\xC5 P2P \xCF\xCE IP";
            constexpr char kSelectTitle[] =
                "\xC2\xDB\xC1\xD0\xC0\xD2\xDC";

            if (void* headerLayout = findElement(root, "HEADER_LAYOUT")) {
                setText(findText(headerLayout, "TEXT"), kConnectionHeader);
            }

            setVisibility(firstWrapper, true, true, false);
            const bool steamBound = configureConnectionButton(panel, firstWrapper, 0);
            setText(findText(firstWrapper, "TITLE_TEXT"), kSteamTitle);
            if (void* summary = findText(firstWrapper, "MAIN_STORY_TEXT")) {
                setText(summary, kSteamSummary);
                setVisibility(summary, true, true, false);
            }
            if (void* disabled = findElement(firstWrapper, "DISABLED_TEXT")) {
                setVisibility(disabled, false, true, false);
            }
            if (void* unlock = findElement(firstWrapper, "UNLOCK_SCENARIO_TEXT")) {
                setVisibility(unlock, false, true, false);
            }

            // SCENARIO_SELECTION_LAYOUT alternates wrappers and dotted separators.
            // Reuse the first two scenario rows for Steam and direct LAN P2P.
            void* firstSeparator = nextSibling(firstWrapper);
            void* lanWrapper = nextSibling(firstSeparator);
            if (firstSeparator) setVisibility(firstSeparator, true, true, false);
            if (lanWrapper) {
                setVisibility(lanWrapper, true, true, false);
                configureConnectionButton(panel, lanWrapper, 1);
                setText(findText(lanWrapper, "TITLE_TEXT"), kLanTitle);
                if (void* summary = findText(lanWrapper, "MAIN_STORY_TEXT")) {
                    setText(summary, kLanSummary);
                    setVisibility(summary, true, true, false);
                }
                if (void* disabled = findElement(lanWrapper, "DISABLED_TEXT")) {
                    setVisibility(disabled, false, true, false);
                }
                if (void* unlock = findElement(lanWrapper, "UNLOCK_SCENARIO_TEXT")) {
                    setVisibility(unlock, false, true, false);
                }
            }

            void* item = lanWrapper ? nextSibling(lanWrapper) : firstSeparator;
            for (int index = 0; item && index < 32; ++index) {
                void* following = nextSibling(item);
                setVisibility(item, false, true, false);
                item = following;
            }

            if (void* startButton =
                    panelElement(panel, kScenarioStartButtonOffset)) {
                setText(findText(startButton, "TEXT"), kSelectTitle);
                reinterpret_cast<SetButtonEnabled>(g_gameBase + kSetButtonEnabledRva)(
                    startButton, g_selectedConnection.load(std::memory_order_acquire) >= 0,
                    false);
            }

            if (g_connectionPanel.exchange(panel, std::memory_order_acq_rel) != panel) {
                logLine(L"Connection selection panel configured with Steam and LAN P2P.");
                if (!steamBound) logLine(L"SCENARIO_BUTTON type check failed; connection input not bound.");
            }
            configured = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine(L"Game rejected a UI call while configuring the connection panel.");
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

void __fastcall scenariosPanelUpdateHook(void* panel) {
    g_originalScenariosPanelUpdate(panel);
    if (g_multiplayerMode.load(std::memory_order_acquire)) {
        configureConnectionPanel(panel);
    }
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

DWORD WINAPI showConnectionDialog(void* selected) {
    const bool lan = reinterpret_cast<std::uintptr_t>(selected) == 1;
    wchar_t path[32768]{};
    GetModuleFileNameW(g_module, path, 32768);
    wchar_t* separator = wcsrchr(path, L'\\');
    if (separator) {
        *separator = L'\0';
        const std::wstring directory(path);
        const std::wstring executable = directory + L"\\FrostBridgeNet.exe";
        std::wstring command = L"\"" + executable + L"\" --ui " +
            (lan ? L"--lan" : L"--steam") + L" --pid " +
            std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                           FALSE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process)) {
            AllowSetForegroundWindow(process.dwProcessId);
            CloseHandle(process.hThread);
            logLine(lan ? L"LAN connection form opened." : L"Steam connection form opened.");
            // Non-modal. A later click launches a short-lived instance which
            // restores the existing form through its per-city mutex.
            CloseHandle(process.hProcess);
        } else {
            const DWORD code = GetLastError();
            const std::wstring error = L"Не удалось открыть встроенное окно FrostBridgeNet.exe. Код Windows: " +
                std::to_wstring(code) + L".\nФайл: " + executable +
                (code == ERROR_VIRUS_INFECTED || code == ERROR_VIRUS_DELETED
                    ? L"\nWindows заблокировала файл. Подробности — в журнале защиты."
                    : L"\nПроверьте комплект сборки: нужен FrostBridgeNet.exe рядом с DLL.");
            MessageBoxW(findGameWindow(), error.c_str(), L"FrostBridge", MB_OK | MB_ICONERROR);
        }
    }
    g_dialogOpen.store(false, std::memory_order_release);
    return 0;
}

void openConnectionForm(int index) {
    if (index < 0 || index > 1) return;
    // A form may be behind the game or minimized. Restore it directly from
    // the input-owning game instead of requiring another confirmation click.
    for (const auto* transport : {L"LAN", L"Steam"}) {
        const auto title = L"FrostBridge — " + std::wstring(transport) +
            L" — PID " + std::to_wstring(GetCurrentProcessId());
        if (HWND window = FindWindowW(L"FrostBridgeConnectionUI", title.c_str())) {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
            return;
        }
    }
    if (!g_dialogOpen.exchange(true, std::memory_order_acq_rel)) {
        HANDLE thread = CreateThread(nullptr, 0, showConnectionDialog,
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(index)), 0, nullptr);
        if (thread) CloseHandle(thread);
        else {
            g_dialogOpen.store(false, std::memory_order_release);
            logLine(L"Could not start connection-form launch thread.");
        }
    }
}

// Test seam: routing tests replace process/window launching with a recorder.
void (*g_openConnectionForm)(int) = openConnectionForm;

void __fastcall menuPanelCallbackHook(void* owner, void* event) {
    std::uint32_t panelId = 0;
    if (event && safeRead(static_cast<std::uint8_t*>(event) + 0x18, panelId) &&
        panelId == kMultiplayerSourcePanelId) {
        g_multiplayerMode.store(true, std::memory_order_release);
        g_selectedConnection.store(-1, std::memory_order_release);
        g_connectionPanel.store(nullptr, std::memory_order_release);

        // Reuse the game's native Scenarios screen. Its controller callback only
        // consumes the panel id at +0x18, so redirect the stock Endless item to
        // the scenarios id without changing game files.
        alignas(8) std::uint8_t redirectedEvent[0x20]{};
        std::memcpy(redirectedEvent, event, sizeof(redirectedEvent));
        *reinterpret_cast<std::uint32_t*>(redirectedEvent + 0x18) =
            kScenariosPanelId;
        g_originalMenuPanelCallback(owner, redirectedEvent);
        return;
    }
    if (panelId != kMultiplayerSourcePanelId) {
        g_multiplayerMode.store(false, std::memory_order_release);
        restoreConnectionButtons();
        g_connectionPanel.store(nullptr, std::memory_order_release);
    }
    g_originalMenuPanelCallback(owner, event);
}

void __fastcall scenarioRowCallbackHook(void* owner, void* event) {
    if (!g_multiplayerMode.load(std::memory_order_acquire)) {
        g_originalScenarioRowCallback(owner, event);
        return;
    }
    std::uint32_t index = 0;
    if (!event || !safeRead(static_cast<std::uint8_t*>(event) + 0x18, index) ||
        index > 1) return;
    g_selectedConnection.store(static_cast<int>(index), std::memory_order_release);
    logLine(index == 0 ? L"Steam connection selected." : L"LAN connection selected.");
    configureConnectionPanel(owner);
    g_openConnectionForm(static_cast<int>(index));
    // Do not call the scenario callback: it checks DLC/progression and opens
    // scenario details. These two rows represent transports, not scenarios.
}

void __fastcall scenariosStartCallbackHook(void* owner, void* event) {
    if (g_multiplayerMode.load(std::memory_order_acquire)) {
        const int index = g_selectedConnection.load(std::memory_order_acquire);
        if (index >= 0 && index <= 1) g_openConnectionForm(index);
        return;
    }
    g_originalScenariosStartCallback(owner, event);
}

void* locateLivePanel(std::uintptr_t vtableRva, std::size_t requiredElementOffset) {
    const std::uintptr_t expectedVtable = g_gameBase + vtableRva;
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
                        panelElement(reinterpret_cast<void*>(address),
                                     requiredElementOffset)) {
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

void __fastcall endlessBuildHook(void* panel) {
    g_originalEndlessBuild(panel);
    if (g_launchStage == 1) g_endlessSelection = panel;
}

void __fastcall endlessShowHook(void* panel) {
    g_originalEndlessShow(panel);
    if (g_launchStage == 2) g_endlessConfig = panel;
}

void advanceEndlessLaunch() {
    if (!g_launchControl) return;
    __try {
        const LONG state = InterlockedCompareExchange(&g_launchControl->state, 0, 0);
        if (state == frostlaunch::unavailable || state == frostlaunch::ready) {
            const LONG next = g_multiplayerMode.load() && g_connectionPanel.load()
                ? frostlaunch::ready : frostlaunch::unavailable;
            InterlockedCompareExchange(&g_launchControl->state, next, state);
            return;
        }
        if (state != frostlaunch::requested) return;
        if (!g_launchStage) {
            if (!g_multiplayerMode.load() || !g_connectionPanel.load()) {
                InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
                return;
            }
            g_launchStage = 1;
            g_launchDeadline = GetTickCount64() + 20000;
            g_multiplayerMode.store(false);
            restoreConnectionButtons();
            g_connectionPanel.store(nullptr);
            alignas(8) unsigned char event[0x20]{};
            *reinterpret_cast<DWORD*>(event + 0x18) = 0x61;
            g_originalMenuPanelCallback(nullptr, event);
            logLine(L"Endless launch: native mode selection requested.");
        } else if (GetTickCount64() > g_launchDeadline) {
            InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
            logLine(L"Endless launch timed out; no further native calls will be made.");
        } else if (g_launchStage == 1 && g_endlessSelection) {
            void* root = panelElement(g_endlessSelection, kPanelRootOffset);
            void* layout = root ? findElement(root, "MODES_LAYOUT") : nullptr;
            void* button = layout ? firstChild(layout) : nullptr;
            std::uintptr_t vtable = 0;
            if (!button || !safeRead(button, vtable) || vtable != g_gameBase + kUIButtonVtableRva) return;
            // Use the real first mode button and native entitlement checks.
            alignas(8) unsigned char event[0x20]{};
            *reinterpret_cast<void***>(event + 8) = &button;
            g_launchStage = 2;
            reinterpret_cast<MenuPanelCallback>(g_gameBase + 0x1A879C0)(g_endlessSelection, event);
            logLine(L"Endless launch: first native mode selected.");
        } else if (g_launchStage == 2 && g_endlessConfig) {
            auto* panel = static_cast<unsigned char*>(g_endlessConfig);
            int count = 0, index = -1;
            void* definition = nullptr;
            if (!safeRead(panel + 0xF0, definition) || !definition ||
                !safeRead(panel + 0x100, count) || !safeRead(panel + 0x110, index) ||
                index < 0 || index >= count) return;
            if (count > 64 || g_nextMapIndex >= count) {
                InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
                logLine(L"Endless launch: no available map with an enabled Start button.");
                return;
            }
            // The first map may be unowned DLC (e.g. The Rifts). Select through
            // the native API and inspect its resulting Start enabled state.
            using SelectMap = void(__fastcall*)(void*, int);
            reinterpret_cast<SelectMap>(g_gameBase + 0x1A8B880)(g_endlessConfig, g_nextMapIndex++);
            void* root = panelElement(g_endlessConfig, kPanelRootOffset);
            void* start = root ? findElement(root, "START_BUTTON") : nullptr;
            std::uint32_t flags = 0;
            if (!start || !safeRead(static_cast<unsigned char*>(start) + 0x270, flags) || !(flags & 8)) return;
            g_launchStage = 3; // exactly once, including reentrant callbacks
            reinterpret_cast<MenuPanelCallback>(g_gameBase + 0x1A8ADE0)(g_endlessConfig, nullptr);
            InterlockedExchange(&g_launchControl->state, frostlaunch::dispatched);
            logLine(L"Endless launch: native start callback dispatched.");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
        logLine(L"Endless launch rejected an invalid native state.");
    }
}

LRESULT CALLBACK launchMessageHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && wp == PM_REMOVE) {
        auto* message = reinterpret_cast<MSG*>(lp);
        if (message->message == g_launchMessage) {
            message->message = WM_NULL;
            advanceEndlessLaunch();
        }
    }
    return CallNextHookEx(g_messageHook, code, wp, lp);
}

bool installHooks() {
    // Whole instructions, no RIP-relative operands or relative branches.
    constexpr unsigned char prologue[] = {
        0x48,0x8B,0xC4,0x55,0x41,0x54,0x41,0x55,
        0x41,0x56,0x41,0x57,0x48,0x8D,0x68,0xA1};
    if (!g_endlessBuildHook.install(reinterpret_cast<void*>(g_gameBase + 0x1A871E0),
            reinterpret_cast<void*>(endlessBuildHook), prologue, sizeof(prologue))) return false;
    g_originalEndlessBuild = g_endlessBuildHook.original<MainMenuUpdate>();
    if (!g_endlessShowHook.install(reinterpret_cast<void*>(g_gameBase + 0x1A89900),
            reinterpret_cast<void*>(endlessShowHook), prologue, sizeof(prologue))) return false;
    g_originalEndlessShow = g_endlessShowHook.original<MainMenuUpdate>();
    auto* updateTarget = reinterpret_cast<void*>(g_gameBase + kMainMenuUpdateRva);
    auto* bindButtonsTarget = reinterpret_cast<void*>(g_gameBase + kMainMenuBindButtonsRva);
    auto* callbackTarget = reinterpret_cast<void*>(g_gameBase + kMenuPanelCallbackRva);
    auto* scenariosUpdateTarget =
        reinterpret_cast<void*>(g_gameBase + kScenariosPanelUpdateRva);
    auto* scenariosStartTarget =
        reinterpret_cast<void*>(g_gameBase + kScenariosStartCallbackRva);

    if (!g_scenarioRowHook.install(
            reinterpret_cast<void*>(g_gameBase + kScenarioRowCallbackRva),
            reinterpret_cast<void*>(scenarioRowCallbackHook),
            kExpectedScenarioRowPrologue, sizeof(kExpectedScenarioRowPrologue))) {
        logLine(L"Scenario-row callback signature mismatch; unsupported build.");
        return false;
    }
    g_originalScenarioRowCallback = g_scenarioRowHook.original<MenuPanelCallback>();

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

    if (!g_scenariosUpdateHook.install(
            scenariosUpdateTarget, reinterpret_cast<void*>(scenariosPanelUpdateHook),
            kExpectedScenariosUpdatePrologue,
            sizeof(kExpectedScenariosUpdatePrologue))) {
        logLine(L"Scenarios-panel update signature mismatch; unsupported build.");
        return false;
    }
    g_originalScenariosPanelUpdate =
        g_scenariosUpdateHook.original<ScenariosPanelUpdate>();

    if (!g_scenariosStartHook.install(
            scenariosStartTarget, reinterpret_cast<void*>(scenariosStartCallbackHook),
            kExpectedScenariosStartPrologue,
            sizeof(kExpectedScenariosStartPrologue))) {
        logLine(L"Scenarios start-button signature mismatch; unsupported build.");
        return false;
    }
    g_originalScenariosStartCallback =
        g_scenariosStartHook.original<ScenariosStartCallback>();
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
    const auto mappingName = frostlaunch::name(GetCurrentProcessId());
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(frostlaunch::Control), mappingName.c_str());
    if (mapping) g_launchControl = static_cast<frostlaunch::Control*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(frostlaunch::Control)));
    if (g_launchControl) {
        g_launchControl->signature = frostlaunch::magic;
        InterlockedExchange(&g_launchControl->state, frostlaunch::unavailable);
    }
    g_launchMessage = RegisterWindowMessageW(L"FrostBridge.NativeLaunch.Tick.V1");

    // The panels can already be open when the DLL is injected. Hooks handle all
    // later updates; this worker supplies a persistent attach-time fallback.
    logLine(L"Waiting for Frostpunk main-menu panels.");
    for (;;) {
        if (!g_messageHook) {
            if (HWND window = findGameWindow()) {
                g_uiThread = GetWindowThreadProcessId(window, nullptr);
                g_messageHook = SetWindowsHookExW(WH_GETMESSAGE, launchMessageHook, g_module, g_uiThread);
            }
        }
        if (g_messageHook && g_launchMessage) PostThreadMessageW(g_uiThread, g_launchMessage, 0, 0);
        if (!g_lastConfiguredPanel.load(std::memory_order_acquire)) {
            if (void* panel = locateLivePanel(kMainMenuVtableRva,
                                              kMultiplayerButtonOffset)) {
                configureMenuPanel(panel);
            }
        }
        if (g_multiplayerMode.load(std::memory_order_acquire)) {
            void* panel = g_connectionPanel.load(std::memory_order_acquire);
            std::uintptr_t vtable = 0;
            if (!panel || !safeRead(panel, vtable) ||
                vtable != g_gameBase + kScenariosPanelVtableRva) {
                panel = locateLivePanel(kScenariosPanelVtableRva, kPanelRootOffset);
            }
            // Some late menu transitions repopulate the stock scenario entries
            // after the panel's update callback. Reapply the lightweight view
            // projection so only the Steam row remains visible.
            if (panel) configureConnectionPanel(panel);
        }
        Sleep(250);
    }
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
