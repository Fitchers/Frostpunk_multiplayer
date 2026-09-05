#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <tlhelp32.h>

#include <atomic>
#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "../LaunchControl.h"
#include "../OverlayControl.h"
#include "../SessionControl.h"
#include "../SaveControl.h"
#include "../FrostBridgeNet/ResourceReader.h"

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
constexpr UINT kCloseForTransportSwitch=WM_APP+42;
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
std::atomic<bool> g_sessionTickPending{false};
DWORD g_uiThread = 0;
int g_launchStage = 0; // accessed only on the game's window thread
int g_nextMapIndex = 0;
int g_selectedMapIndex = -1;
bool g_mapSelectionIssued = false;
ULONGLONG g_launchDeadline = 0;
void* g_endlessSelection = nullptr;
void* g_endlessConfig = nullptr;
MainMenuUpdate g_originalEndlessBuild = nullptr, g_originalEndlessShow = nullptr;
MenuPanelCallback g_originalEndlessStart = nullptr;
HANDLE g_overlayMapping = nullptr;
frostoverlay::Control* g_overlayControl = nullptr;
HWND g_overlayWindow = nullptr;
HWND g_overlayButtonWindow = nullptr;
HWND g_sessionButtonWindow = nullptr;
std::atomic<bool> g_overlayExpanded = false;
LONG g_lastApplySequence = 0;
RECT g_plusButtons[frostoverlay::resourceCount]{};
HWND g_amountEdits[frostoverlay::resourceCount]{};
HWND g_amountSliders[frostoverlay::resourceCount]{};
bool g_updatingAmounts = false;
HANDLE g_sessionMapping = nullptr;
frostsession::Control* g_sessionControl = nullptr;
LONG g_lastPauseCommand = 0;
LONG g_lastSpeedCommand = 0;
int g_lastNativeSpeed = -1;
LONG g_pendingSpeedCommand = 0;
int g_pendingSpeedMode = -1;
ULONGLONG g_pendingSpeedRetry = 0;
bool g_sessionLoaded = false;
bool g_waitReloadPause = false;
LONG g_reloadPauseSequence = 0;
bool g_sessionPaused = true;
bool g_pendingInitialPause = false;
std::uint64_t g_overlayPaintRevision = 0;
ULONGLONG g_sessionNextProbe = 0;

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
InlineHook g_endlessBuildHook, g_endlessShowHook, g_endlessStartHook;

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
    if (g_launchControl && frostlaunch::isStory(g_launchControl->mapIndex) && g_launchStage == 1)
        g_endlessConfig = panel;
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
            wchar_t className[64]{};
            GetClassNameW(window, className, static_cast<int>(std::size(className)));
            if (processId == search->processId && IsWindowVisible(window) &&
                GetWindow(window, GW_OWNER) == nullptr &&
                wcscmp(className, L"SDL_app") == 0) {
                search->window = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.window;
}

bool safeCopyMemory(std::uintptr_t address, void* destination, std::size_t size) {
    __try {
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int invokeResourceChange(void* economy, void* entry, int delta) {
    // RVA 168E680 is the native four-argument wrapper. It supplies a null event
    // context and the stock false option to the core ChangeResource routine.
    using ChangeResource = int(__fastcall*)(void*, void*, int, int);
    __try {
        return reinterpret_cast<ChangeResource>(g_gameBase + 0x168E680)(
            economy, entry, delta, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void advanceOverlayApply() {
    if (!g_overlayControl || g_overlayControl->signature != frostoverlay::magic ||
        g_overlayControl->layoutVersion != frostoverlay::version) return;
    const LONG sequence = InterlockedCompareExchange(
        &g_overlayControl->applyRequestSequence, 0, 0);
    if (!sequence || sequence == g_lastApplySequence) return;
    g_lastApplySequence = sequence; // exactly once, even if validation fails
    MemoryBarrier();
    const LONG resource = g_overlayControl->applyResource;
    const LONG delta = g_overlayControl->applyDelta;
    LONG result = 0;
    if (frostoverlay::validResource(resource) && delta &&
        delta >= -frostoverlay::maxTransferAmount && delta <= frostoverlay::maxTransferAmount) {
        auto reader = [](std::uintptr_t address, void* destination, std::size_t size) {
            return safeCopyMemory(address, destination, size);
        };
        const auto resolved = frostbridge::resources::readResolved(reader, g_gameBase);
        std::uintptr_t economy = 0;
        if (resolved && safeCopyMemory(g_gameBase + frostbridge::resources::economyRva,
                                       &economy, sizeof(economy)) && economy) {
            result = invokeResourceChange(reinterpret_cast<void*>(economy),
                reinterpret_cast<void*>(resolved->entries[static_cast<std::size_t>(resource)]), delta);
        }
    }
    g_overlayControl->applyResult = result;
    MemoryBarrier();
    InterlockedExchange(&g_overlayControl->applyResultSequence, sequence);
}

void publishLocalPause(bool paused) {
    g_sessionPaused = paused;
    if (!g_sessionControl) return;
    InterlockedExchange(&g_sessionControl->paused, paused ? 1 : 0);
    InterlockedExchange(&g_sessionControl->localPauseValue, paused ? 1 : 0);
    MemoryBarrier();
    InterlockedIncrement(&g_sessionControl->localPauseSequence);
}

// Recovered from BUTTON_PAUSE callback RVA 1B1A8D0. This is the native
// UserPause API, not synthetic input. Called only on the game window thread.
bool nativeTimeState(std::uintptr_t& object, bool& paused, LONG64& timeMs) {
    std::uintptr_t vtable = 0;
    unsigned char flag = 0;
    std::int64_t ticks = 0;
    if (!safeCopyMemory(g_gameBase + 0x2B6A710, &object, sizeof(object)) || !object ||
        !safeCopyMemory(object, &vtable, sizeof(vtable)) || vtable != g_gameBase + 0x1DFCB80 ||
        !safeCopyMemory(object + 0xE0, &flag, sizeof(flag)) || flag > 1 ||
        !safeCopyMemory(object + 0xB0, &ticks, sizeof(ticks)) || ticks < 0) return false;
    paused = flag != 0;
    // Same scale used by the game's calendar conversion at RVA 11AE360.
    // 64-bit calendar milliseconds remain valid for cities longer than 24 days.
    int hourScale = 0;
    if (!safeCopyMemory(g_gameBase + 0x2B70CC0, &hourScale, sizeof(hourScale)) ||
        hourScale <= 0 || hourScale > 1000000) return false;
    timeMs = static_cast<LONG64>((ticks / 2147483648.0) * 3600000.0 / hourScale);
    return true;
}

// Timer reasons are pointer-identity tokens. This token belongs only to networking.
const char g_networkPauseReason[] = "FrostBridgeNetworkPause";
bool readTimerPause(std::uintptr_t& timer, bool& local, bool& network) {
    std::uintptr_t vtable = 0, reasons = 0;
    int count = 0;
    if (!safeCopyMemory(g_gameBase + 0x2B68510, &timer, sizeof(timer)) || !timer ||
        !safeCopyMemory(timer, &vtable, sizeof(vtable)) || vtable != g_gameBase + 0x1D716F0 ||
        !safeCopyMemory(timer + 0xB8, &reasons, sizeof(reasons)) ||
        !safeCopyMemory(timer + 0xC0, &count, sizeof(count)) || count < 0 || count > 128) return false;
    local = network = false;
    for (int i = 0; i < count; ++i) {
        std::uintptr_t reason = 0;
        if (!safeCopyMemory(reasons + i * sizeof(reason), &reason, sizeof(reason))) return false;
        if (reason == reinterpret_cast<std::uintptr_t>(g_networkPauseReason)) network = true;
        else local = true;
    }
    return true;
}

bool applyNativePause(bool pause) {
    std::uintptr_t timer = 0;
    bool local = false, network = false;
    if (!readTimerPause(timer, local, network)) return false;
    if (network == pause) return true;
    using TimerReason = void(__fastcall*)(void*, const void*);
    const auto rva = pause ? 0xF6C530 : 0xF6CA80;
    constexpr unsigned char expected[]{0x48,0x89,0x5C,0x24,0x18};
    unsigned char code[sizeof(expected)]{};
    if (!safeCopyMemory(g_gameBase + rva, code, sizeof(code)) ||
        std::memcmp(code, expected, sizeof(code))) return false;
    __try {
        reinterpret_cast<TimerReason>(g_gameBase + rva)(
            reinterpret_cast<void*>(timer), g_networkPauseReason);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return readTimerPause(timer, local, network) && network == pause;
}

void observeSessionInput(const MSG&) {
    // Native state is observed after the engine handles input, including hotkeys.
    // Coordinates/key guesses used to report a pause which had never happened.
}

int nativeSpeed() {
    std::uintptr_t timer = 0;
    bool local = false, network = false;
    float current = 0, modes[3]{};
    if (!readTimerPause(timer, local, network) ||
        !safeCopyMemory(timer + 0xD0, &current, sizeof(current)) ||
        !safeCopyMemory(g_gameBase + 0x2B70CB4, modes, sizeof(modes))) return -1;
    for (int i = 0; i < 3; ++i)
        if (modes[i] > 0 && modes[i] <= 1000 && current == modes[i]) return i;
    return -1;
}

bool applyNativeSpeed(int mode) {
    if (mode < 0 || mode > 2) return false;
    std::uintptr_t object = 0;
    bool paused = false;
    LONG64 time = 0;
    if (!nativeTimeState(object, paused, time)) return false;
    constexpr unsigned char expected[]{0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20};
    unsigned char code[sizeof(expected)]{};
    if (!safeCopyMemory(g_gameBase + 0x11BD080, code, sizeof(code)) ||
        std::memcmp(code, expected, sizeof(code))) return false;
    __try {
        using SetSpeed = void(__fastcall*)(void*, int);
        reinterpret_cast<SetSpeed>(g_gameBase + 0x11BD080)(reinterpret_cast<void*>(object), mode);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true; // the engine publishes and applies the speed event asynchronously
}

void advanceSpeedControl(bool connected) {
    int speed = nativeSpeed();
    if (speed < 0) return;
    InterlockedExchange(&g_sessionControl->currentSpeed, speed);
    if (g_pendingSpeedCommand) {
        if (speed == g_pendingSpeedMode) {
            g_lastNativeSpeed = speed;
            g_lastSpeedCommand = g_pendingSpeedCommand;
            InterlockedExchange(&g_sessionControl->speedResultSequence, g_pendingSpeedCommand);
            g_pendingSpeedCommand = 0;
            g_pendingSpeedMode = -1;
        } else if (GetTickCount64() >= g_pendingSpeedRetry && applyNativeSpeed(g_pendingSpeedMode)) {
            g_pendingSpeedRetry = GetTickCount64() + 500;
        }
        return; // an inbound engine event must never become outbound player input
    }
    // Observe player input before applying commands. Do not echo applied changes
    // or treat the initial city speed as a new client request.
    if (g_lastNativeSpeed >= 0 && speed != g_lastNativeSpeed) {
        InterlockedExchange(&g_sessionControl->localSpeedValue, speed);
        MemoryBarrier();
        InterlockedIncrement(&g_sessionControl->localSpeedSequence);
    }
    g_lastNativeSpeed = speed;
    const LONG sequence = InterlockedCompareExchange(&g_sessionControl->speedCommandSequence, 0, 0);
    if (connected && sequence && sequence != g_lastSpeedCommand) {
        MemoryBarrier();
        const int requested = g_sessionControl->speedCommandValue;
        if (applyNativeSpeed(requested)) {
            g_pendingSpeedCommand = sequence;
            g_pendingSpeedMode = requested;
            g_pendingSpeedRetry = GetTickCount64() + 500;
        }
    }
}

void advanceSessionControl() {
    if (!g_sessionControl || g_sessionControl->signature != frostsession::magic ||
        g_sessionControl->layoutVersion != frostsession::version) return;
    const ULONGLONG now = GetTickCount64();
    bool justLoaded = false;
    if (now >= g_sessionNextProbe) {
        g_sessionNextProbe = now + 100;
        auto reader = [](std::uintptr_t address, void* destination, std::size_t size) {
            return safeCopyMemory(address, destination, size);
        };
        // Resource objects exist during deserialization, before the city is ready.
        // IsLoadingScreenActive (native predicate at RVA F283C0) reads this byte.
        unsigned char loadingScreen = 1;
        const bool loaded = safeCopyMemory(g_gameBase + 0x2A602DD,
            &loadingScreen, sizeof(loadingScreen)) && !loadingScreen &&
            frostbridge::resources::readResolved(reader, g_gameBase).has_value();
        if (loaded != g_sessionLoaded) {
            g_sessionLoaded = loaded;
            justLoaded = loaded;
            // Publish loaded only after native pause/time can actually be read.
            if (!loaded) {
                InterlockedExchange(&g_sessionControl->gameLoaded, 0);
                InterlockedExchange(&g_sessionControl->localSpeedValue, -1);
                InterlockedExchange(&g_sessionControl->currentSpeed, -1);
                g_lastNativeSpeed = -1;
                g_pendingSpeedCommand = 0;
                g_pendingSpeedMode = -1;
                g_pendingInitialPause = false;
                // A loaded world may replace the timer. An old acknowledgement
                // must never suppress applying the same command to the new one.
                g_lastPauseCommand = 0;
            }
            InterlockedIncrement(&g_sessionControl->loadGeneration);
        }
    }
    if (!g_sessionLoaded) return;
    const bool connected = g_overlayControl &&
        InterlockedCompareExchange(&g_overlayControl->connection, 0, 0) ==
            static_cast<LONG>(frostoverlay::Connection::connected);
    const LONG sequence = InterlockedCompareExchange(
        &g_sessionControl->pauseCommandSequence, 0, 0);
    advanceSpeedControl(connected);
    if (justLoaded && connected) {
        g_pendingInitialPause = true;
        g_waitReloadPause = true;
        g_reloadPauseSequence = sequence;
    }
    if (!connected) {
        applyNativePause(false); // Release only our reason when the bridge disconnects.
        g_pendingInitialPause = false;
        g_waitReloadPause = false;
    }
    if (g_pendingInitialPause && applyNativePause(true)) {
        g_pendingInitialPause = false;
    }
    if (g_waitReloadPause && sequence != g_reloadPauseSequence) g_waitReloadPause = false;
    if (connected && sequence && !g_waitReloadPause) {
        MemoryBarrier();
        if (applyNativePause(g_sessionControl->pauseCommandValue != 0)) {
            g_lastPauseCommand = sequence;
            InterlockedExchange(&g_sessionControl->pauseResultSequence, sequence);
        } // Unavailable during loading: retry; never acknowledge a synthetic click.
    }
    std::uintptr_t object = 0;
    bool paused = false;
    LONG64 nativeTime = 0;
    if (!nativeTimeState(object, paused, nativeTime)) {
        InterlockedExchange(&g_sessionControl->gameLoaded, 0);
        return;
    }
    std::uintptr_t timer = 0;
    bool localPause = false, networkPause = false;
    if (!readTimerPause(timer, localPause, networkPause)) {
        InterlockedExchange(&g_sessionControl->gameLoaded, 0);
        return;
    }
    // Only locally owned reasons are sent to the peer; network holds never echo.
    if (justLoaded || localPause != g_sessionPaused) publishLocalPause(localPause);
    g_sessionPaused = localPause;
    InterlockedExchange(&g_sessionControl->paused, (localPause || networkPause) ? 1 : 0);
    InterlockedExchange64(&g_sessionControl->gameTimeMs, nativeTime);
    InterlockedExchange(&g_sessionControl->gameLoaded, 1);
}

std::wstring utf8Name(const char* input, const wchar_t* fallback) {
    if (!input || !*input) return fallback;
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1,
                                         nullptr, 0);
    if (size <= 1 || size > 128) return fallback;
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1,
                        result.data(), size);
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}

void overlayNames(std::wstring& local, std::wstring& peer) {
    if (!g_overlayControl) { local = L"Вы"; peer = L"Игрок 2"; return; }
    char localBytes[64]{}, peerBytes[64]{};
    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG before = InterlockedCompareExchange(
            &g_overlayControl->namesSequence, 0, 0);
        if (before & 1) continue;
        MemoryBarrier();
        std::memcpy(localBytes, g_overlayControl->localName, sizeof(localBytes));
        std::memcpy(peerBytes, g_overlayControl->peerName, sizeof(peerBytes));
        MemoryBarrier();
        if (before == InterlockedCompareExchange(&g_overlayControl->namesSequence, 0, 0)) break;
    }
    localBytes[63] = peerBytes[63] = '\0';
    local = utf8Name(localBytes, L"Вы");
    peer = utf8Name(peerBytes, L"Игрок 2");
}

LONG selectedAmount(LONG resource) {
    wchar_t text[16]{};
    GetWindowTextW(g_amountEdits[resource], text, 16);
    if (!*text) return 0;
    LONG value = 0;
    for (const wchar_t* p = text; *p; ++p) {
        if (*p < L'0' || *p > L'9' || value > frostoverlay::maxTransferAmount / 10) return 0;
        value = value * 10 + (*p - L'0');
        if (value > frostoverlay::maxTransferAmount) return 0;
    }
    return value;
}

bool canSend(LONG resource, const frostoverlay::Values& local) {
    const LONG amount = selectedAmount(resource);
    return g_overlayControl && frostoverlay::validAmount(amount) &&
        local.item[resource] >= amount &&
        InterlockedCompareExchange(&g_overlayControl->connection, 0, 0) ==
            static_cast<LONG>(frostoverlay::Connection::connected) &&
        !InterlockedCompareExchange(&g_overlayControl->transferBusy, 0, 0);
}

std::uint64_t overlayPaintRevision() {
    if (!g_overlayControl) return 0;
    std::uint64_t value = 1469598103934665603ULL;
    auto mix = [&](LONG item) {
        value ^= static_cast<std::uint32_t>(item);
        value *= 1099511628211ULL;
    };
    mix(InterlockedCompareExchange(&g_overlayControl->local.sequence, 0, 0));
    mix(InterlockedCompareExchange(&g_overlayControl->peer.sequence, 0, 0));
    mix(InterlockedCompareExchange(&g_overlayControl->namesSequence, 0, 0));
    mix(InterlockedCompareExchange(&g_overlayControl->notificationSequence, 0, 0));
    mix(InterlockedCompareExchange(&g_overlayControl->transferBusy, 0, 0));
    mix(InterlockedCompareExchange(&g_overlayControl->localHope, 0, 0));
    mix(InterlockedCompareExchange(&g_overlayControl->localDiscontent, 0, 0));
    mix(InterlockedCompareExchange(&g_overlayControl->peerHope, 0, 0));
    mix(InterlockedCompareExchange(&g_overlayControl->peerDiscontent, 0, 0));
    mix(g_overlayControl->localState); mix(g_overlayControl->peerState);
    mix(g_overlayControl->skewSeconds); mix(g_overlayControl->historySequence);
    return value;
}

void placeOverlayWindow(HWND window, int x, int y, int width, int height) {
    RECT current{};
    const bool visible = IsWindowVisible(window) != FALSE;
    if (!GetWindowRect(window, &current) || current.left != x || current.top != y ||
        current.right - current.left != width || current.bottom - current.top != height) {
        SetWindowPos(window, HWND_TOPMOST, x, y, width, height,
                     SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | (visible ? 0 : SWP_SHOWWINDOW));
    } else if (!visible) {
        ShowWindow(window, SW_SHOWNOACTIVATE);
    }
}

void refreshAmountControls(const frostoverlay::Values& local) {
    for (LONG i = 0; i < static_cast<LONG>(frostoverlay::resourceCount); ++i) {
        if (!g_amountSliders[i]) continue;
        const LONG maximum = std::clamp(local.item[i], 0L, frostoverlay::maxTransferAmount);
        SendMessageW(g_amountSliders[i], TBM_SETRANGEMAX, TRUE, maximum);
        SendMessageW(g_amountSliders[i], TBM_SETPOS, TRUE, (std::min)(selectedAmount(i), maximum));
        EnableWindow(g_amountSliders[i], maximum > 0);
    }
}


constexpr int overlayWidth = 760, overlayHeight = 604;
int g_selectedResource = 0;
RECT g_quickButtons[4]{};
constexpr const wchar_t* resourceLabels[] = {
    L"Уголь", L"Древесина", L"Сталь", L"Паровые ядра", L"Сырая еда", L"Пайки"};

void selectOverlayResource(HWND window, int resource) {
    g_selectedResource = resource;
    for (int i = 0; i < 6; ++i)
        ShowWindow(g_amountSliders[i], i == resource ? SW_SHOWNOACTIVATE : SW_HIDE);
    InvalidateRect(window, nullptr, FALSE);
}

void overlayText(HDC dc, const std::wstring& value, RECT rect, COLORREF color,
                 UINT align = DT_LEFT) {
    SetTextColor(dc, color);
    DrawTextW(dc, value.c_str(), -1, &rect,
        align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void overlayFill(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush); DeleteObject(brush);
}

void resourceIcon(HDC dc, int x, int y, int resource) {
    const int saved = SaveDC(dc);
    SetViewportOrgEx(dc, x, y, nullptr);
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(235,192,85));
    HBRUSH brush = CreateSolidBrush(RGB(113,90,42));
    SelectObject(dc, pen); SelectObject(dc, brush);
    if (resource == 0) {
        POINT points[]{{2,9},{9,2},{19,5},{23,16},{15,23},{4,20}};
        Polygon(dc, points, 6);
    } else if (resource == 1) {
        Rectangle(dc,4,6,21,19); Ellipse(dc,1,6,10,19); Ellipse(dc,4,10,7,15);
        MoveToEx(dc,11,10,nullptr); LineTo(dc,19,10);
    } else if (resource == 2) {
        POINT points[]{{2,17},{6,7},{19,7},{23,17}};
        Polygon(dc,points,4); MoveToEx(dc,2,20,nullptr); LineTo(dc,23,20);
    } else if (resource == 3) {
        RoundRect(dc,5,3,20,23,5,5); Rectangle(dc,9,0,16,4);
        Ellipse(dc,9,9,16,17);
    } else if (resource == 4) {
        Ellipse(dc,2,7,19,19);
        POINT tail[]{{18,13},{24,6},{24,20}}; Polygon(dc,tail,3);
        SetPixel(dc,6,12,RGB(255,255,255));
    } else {
        Ellipse(dc,2,3,23,24); Ellipse(dc,6,7,19,20);
        MoveToEx(dc,0,2,nullptr); LineTo(dc,0,23);
    }
    RestoreDC(dc,saved); DeleteObject(pen); DeleteObject(brush);
}

void drawPlayerCard(HDC dc, int x, const std::wstring& name, LONG state,
                    LONG hope, LONG discontent) {
    constexpr const wchar_t* states[]{L"нет данных",L"загружается",L"играет",L"пауза"};
    overlayText(dc,name,{x,40,x+350,64},RGB(246,197,70));
    overlayText(dc,states[std::clamp(state,0L,3L)],{x,65,x+350,85},RGB(185,198,207));
    const LONG values[]{hope,discontent};
    for (int i=0;i<2;++i) {
        RECT bar{x+i*178,91,x+i*178+166,110};
        overlayFill(dc,bar,RGB(38,48,55));
        if(values[i]>=0 && values[i]<=10000) {
            RECT fill=bar; fill.right=fill.left+166*values[i]/10000;
            overlayFill(dc,fill,i ? RGB(151,49,58) : RGB(35,115,153));
        }
        wchar_t text[64]{};
        if(values[i]<0) _snwprintf_s(text,_TRUNCATE,L"%s: —",i?L"Недов.":L"Надежда");
        else _snwprintf_s(text,_TRUNCATE,L"%s: %.1f%%",i?L"Недов.":L"Надежда",values[i]/100.0);
        overlayText(dc,text,bar,RGB(240,245,247),DT_CENTER);
    }
}

void drawResourceTable(HDC dc, const frostoverlay::Values& local,
                        const frostoverlay::Values& peer) {
    constexpr COLORREF white=RGB(227,235,240), muted=RGB(153,170,180), gold=RGB(246,197,70);
    overlayText(dc,L"РЕСУРС",{52,145,240,166},muted);
    overlayText(dc,L"У тебя → У друга",{244,145,452,166},muted,DT_CENTER);
    overlayText(dc,L"Количество",{473,145,565,166},muted,DT_CENTER);
    for(int i=0;i<6;++i) {
        int y=174+i*42;
        overlayFill(dc,{16,y-2,744,y+37},i==g_selectedResource?RGB(35,46,52):RGB(20,28,33));
        resourceIcon(dc,24,y+4,i);
        overlayText(dc,resourceLabels[i],{58,y,235,y+32},white);
        overlayText(dc,std::to_wstring(local.item[i]),{235,y,323,y+32},white,DT_RIGHT);
        overlayText(dc,L"→",{328,y,360,y+32},gold,DT_CENTER);
        overlayText(dc,std::to_wstring(peer.item[i]),{365,y,450,y+32},white);
        RECT button{592,y+2,729,y+30};
        g_plusButtons[i]=button;
        const bool enabled=canSend(i,local);
        overlayFill(dc,button,enabled?RGB(112,86,31):RGB(43,49,54));
        overlayText(dc,L"Передать",button,enabled?gold:RGB(125,137,144),DT_CENTER);
    }
    overlayText(dc,std::wstring(resourceLabels[g_selectedResource])+L" · доступно "+
        std::to_wstring(local.item[g_selectedResource]),{20,432,358,455},muted);
    const wchar_t* quick[]{L"1",L"10",L"50",L"Всё"};
    for(int i=0;i<4;++i) {
        g_quickButtons[i]={446+i*74,435,512+i*74,464};
        overlayFill(dc,g_quickButtons[i],RGB(47,56,61));
        overlayText(dc,quick[i],g_quickButtons[i],gold,DT_CENTER);
    }
}
LRESULT CALLBACK overlayWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        for (int i = 0; i < static_cast<int>(frostoverlay::resourceCount); ++i) {
            const int left = 481, y = 176 + i * 42;
            g_amountEdits[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                left, y, 78, 27, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(100 + i)), g_module, nullptr);
            SendMessageW(g_amountEdits[i], EM_SETLIMITTEXT, 7, 0);
            SendMessageW(g_amountEdits[i], WM_SETFONT,
                reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            g_amountSliders[i] = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_TABSTOP | TBS_NOTICKS,
                20, 460, 396, 25, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(200 + i)), g_module, nullptr);
            SendMessageW(g_amountSliders[i], TBM_SETRANGEMIN, FALSE, 0);
        }
        return 0;
    case WM_NCHITTEST: return HTCLIENT; // Panel absorbs clicks; no accidental city actions.
    case WM_MOUSEACTIVATE: return MA_ACTIVATE; // Text fields need keyboard focus.
    case WM_CLOSE:
        g_overlayExpanded.store(false);
        ShowWindow(window, SW_HIDE);
        if (HWND game = findGameWindow()) SetForegroundWindow(game);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) >= 100 && LOWORD(wp) < 106 && HIWORD(wp) == EN_SETFOCUS)
            selectOverlayResource(window, LOWORD(wp) - 100);
        if (LOWORD(wp) >= 100 && LOWORD(wp) < 106 && HIWORD(wp) == EN_CHANGE &&
            !g_updatingAmounts && g_overlayControl) {
            frostoverlay::Values local{};
            if (frostoverlay::readSnapshot(g_overlayControl->local, local))
                refreshAmountControls(local);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_HSCROLL:
        for (int i = 0; i < static_cast<int>(frostoverlay::resourceCount); ++i) {
            if (reinterpret_cast<HWND>(lp) != g_amountSliders[i]) continue;
            const auto amount = static_cast<LONG>(SendMessageW(g_amountSliders[i], TBM_GETPOS, 0, 0));
            g_updatingAmounts = true;
            SetWindowTextW(g_amountEdits[i], std::to_wstring(amount).c_str());
            g_updatingAmounts = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (point.y >= 174 && point.y < 426 && point.x < 470)
            selectOverlayResource(window, (point.y - 174) / 42);
        if (g_overlayControl) {
            frostoverlay::Values stock{};
            if (frostoverlay::readSnapshot(g_overlayControl->local, stock)) {
                for (int i=0;i<4;++i) if (PtInRect(&g_quickButtons[i],point)) {
                    const LONG quick[]{1,10,50,std::clamp(stock.item[g_selectedResource],0L,frostoverlay::maxTransferAmount)};
                    SetWindowTextW(g_amountEdits[g_selectedResource],std::to_wstring(quick[i]).c_str());
                    selectOverlayResource(window,g_selectedResource);
                    return 0;
                }
            }
        }
        if (!g_overlayControl || InterlockedCompareExchange(
                &g_overlayControl->connection, 0, 0) !=
                static_cast<LONG>(frostoverlay::Connection::connected) ||
            InterlockedCompareExchange(&g_overlayControl->transferBusy, 0, 0)) return 0;
        frostoverlay::Values local{};
        if (!frostoverlay::readSnapshot(g_overlayControl->local, local)) return 0;
        for (LONG resource = 0; resource < static_cast<LONG>(frostoverlay::resourceCount); ++resource) {
            if (!canSend(resource, local) ||
                !PtInRect(&g_plusButtons[resource], point)) continue;
            g_overlayControl->outgoingResource = resource;
            g_overlayControl->outgoingAmount = selectedAmount(resource);
            MemoryBarrier();
            InterlockedIncrement(&g_overlayControl->outgoingSequence);
            InvalidateRect(window, nullptr, FALSE);
            break;
        }
        return 0;
    }
    case WM_TIMER: {
        HWND game = findGameWindow();
        const bool connected = g_overlayControl &&
            InterlockedCompareExchange(&g_overlayControl->connection, 0, 0) ==
                static_cast<LONG>(frostoverlay::Connection::connected);
        const bool cityLoaded = g_sessionControl &&
            InterlockedCompareExchange(&g_sessionControl->modReady, 0, 0) &&
            InterlockedCompareExchange(&g_sessionControl->gameLoaded, 0, 0);
        HWND foreground = GetForegroundWindow();
        DWORD foregroundProcess = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &foregroundProcess);
        const bool thisGameIsActive = foregroundProcess == GetCurrentProcessId();
        if (!game || IsIconic(game) || !connected || !cityLoaded || !thisGameIsActive ||
            !g_overlayExpanded.load(std::memory_order_acquire)) {
            if (IsWindowVisible(window)) ShowWindow(window, SW_HIDE);
        } else {
            if (GetWindow(window, GW_OWNER) != game) {
                SetWindowLongPtrW(window, GWLP_HWNDPARENT,
                                  reinterpret_cast<LONG_PTR>(game));
            }
            RECT client{};
            GetClientRect(game, &client);
            POINT origin{};
            ClientToScreen(game, &origin);
            constexpr int width = overlayWidth, height = overlayHeight;
            const int x = origin.x + (client.right - width) / 2;
            const int y = origin.y + (client.bottom - height) / 2;
            placeOverlayWindow(window, x, y, width, height);
        }
        const auto revision = overlayPaintRevision();
        if (revision != g_overlayPaintRevision) {
            g_overlayPaintRevision = revision;
            frostoverlay::Values stock{};
            if (g_overlayControl && frostoverlay::readSnapshot(g_overlayControl->local,stock)) refreshAmountControls(stock);
            if (IsWindowVisible(window)) InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_PRINTCLIENT:
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC target = message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(wp) : BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HDC buffer = message == WM_PAINT ? CreateCompatibleDC(target) : nullptr;
        HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, client.right, client.bottom) : nullptr;
        HGDIOBJ oldBitmap = buffer && bitmap ? SelectObject(buffer, bitmap) : nullptr;
        HDC dc = buffer && bitmap ? buffer : target;
        HBRUSH background = CreateSolidBrush(RGB(13, 19, 23));
        FillRect(dc, &client, background);
        DeleteObject(background);
        HPEN line = CreatePen(PS_SOLID, 1, RGB(138, 112, 44));
        const HGDIOBJ oldPen = SelectObject(dc, line);
        MoveToEx(dc, 12, 32, nullptr); LineTo(dc, client.right - 12, 32);
        SelectObject(dc, oldPen);
        DeleteObject(line);
        HFONT title = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT normal = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT small = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SetBkMode(dc, TRANSPARENT);
        const HGDIOBJ oldFont = SelectObject(dc, title);
        SetTextColor(dc, RGB(246, 197, 70));
        RECT heading{12, 5, overlayWidth - 12, 30};
        DrawTextW(dc, L"FROSTPUNK MULTIPLAYER  •  ОБМЕН РЕСУРСАМИ", -1,
                  &heading, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        frostoverlay::Values local{}, peer{};
        if (g_overlayControl) {
            frostoverlay::readSnapshot(g_overlayControl->local, local);
            frostoverlay::readSnapshot(g_overlayControl->peer, peer);
        }
        std::wstring localName, peerName;
        overlayNames(localName, peerName);
        SelectObject(dc, small);
        drawPlayerCard(dc,20,localName+L" (вы)",g_overlayControl?g_overlayControl->localState:0,
            g_overlayControl?g_overlayControl->localHope:-1,g_overlayControl?g_overlayControl->localDiscontent:-1);
        drawPlayerCard(dc,394,peerName,g_overlayControl?g_overlayControl->peerState:0,
            g_overlayControl?g_overlayControl->peerHope:-1,g_overlayControl?g_overlayControl->peerDiscontent:-1);
        const bool clocks = g_overlayControl && g_overlayControl->localState >= 2 && g_overlayControl->peerState >= 2;
        const LONG skew = g_overlayControl ? g_overlayControl->skewSeconds : 0;
        std::wstring clockText = !clocks ? L"Разница времени: ждём данные обоих городов" :
            L"Разница игрового времени: " + std::to_wstring(skew) + L" с (вы − друг)";
        overlayText(dc,clockText,{20,116,740,139},RGB(176,194,204),DT_CENTER);
        drawResourceTable(dc,local,peer);
        overlayText(dc,L"ПОСЛЕДНИЕ ПЕРЕДАЧИ",{20,492,740,512},RGB(246,197,70));
        wchar_t history[3][160]{};
        if (g_overlayControl) {
            const LONG before=InterlockedCompareExchange(&g_overlayControl->historySequence,0,0);
            if (!(before&1)) {
                MemoryBarrier(); std::memcpy(history,g_overlayControl->history,sizeof(history)); MemoryBarrier();
                if(before!=InterlockedCompareExchange(&g_overlayControl->historySequence,0,0))
                    ZeroMemory(history,sizeof(history));
            }
        }
        for(int i=0;i<3;++i) {
            history[i][159]=0;
            overlayText(dc,history[i][0]?history[i]:(i==0?L"Пока нет передач":L""),
                {20,514+i*20,740,534+i*20},RGB(191,204,213));
        }

        wchar_t notification[160]{};
        if (g_overlayControl) {
            const LONG before = InterlockedCompareExchange(
                &g_overlayControl->notificationSequence, 0, 0);
            std::memcpy(notification, g_overlayControl->notification, sizeof(notification));
            MemoryBarrier();
            if (before != InterlockedCompareExchange(
                    &g_overlayControl->notificationSequence, 0, 0)) notification[0] = L'\0';
        }
        SetTextColor(dc, RGB(184, 195, 202));
        SelectObject(dc, small);
        RECT status{20, 579, 740, 600};
        DrawTextW(dc, notification[0] ? notification : L"Выберите ресурс для ползунка. MULTIPLAYER закрывает панель.",
                  -1, &status, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
        SelectObject(dc, oldFont);
        DeleteObject(title); DeleteObject(normal); DeleteObject(small);
        if (message == WM_PAINT) {
            if (buffer && bitmap) BitBlt(target, 0, 0, client.right, client.bottom,
                                         buffer, 0, 0, SRCCOPY);
            if (oldBitmap) SelectObject(buffer, oldBitmap);
            if (bitmap) DeleteObject(bitmap);
            if (buffer) DeleteDC(buffer);
            EndPaint(window, &paint);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(window, message, wp, lp);
}

void openConnectionForm(int index);

LRESULT CALLBACK overlayButtonWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONUP:
        if(window==g_sessionButtonWindow) {
            const int selected=g_selectedConnection.load(std::memory_order_acquire);
            openConnectionForm(selected>=0 && selected<=1?selected:0);
            return 0;
        }
        g_overlayExpanded.store(!g_overlayExpanded.load(std::memory_order_acquire),
                                std::memory_order_release);
        InvalidateRect(window, nullptr, FALSE);
        if (g_overlayWindow) {
            // Both windows belong to this UI thread. Update visibility now,
            // independently of menu scanning and of the network polling loop.
            SendMessageW(g_overlayWindow, WM_TIMER, 1, 0);
            InvalidateRect(g_overlayWindow, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER: {
        HWND game = findGameWindow();
        const bool connected = g_overlayControl &&
            InterlockedCompareExchange(&g_overlayControl->connection, 0, 0) ==
                static_cast<LONG>(frostoverlay::Connection::connected);
        const bool cityLoaded = g_sessionControl &&
            InterlockedCompareExchange(&g_sessionControl->modReady, 0, 0) &&
            InterlockedCompareExchange(&g_sessionControl->gameLoaded, 0, 0);
        HWND foreground = GetForegroundWindow();
        DWORD foregroundProcess = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &foregroundProcess);
        if (!game || IsIconic(game) || !connected || !cityLoaded ||
            foregroundProcess != GetCurrentProcessId()) {
            if (IsWindowVisible(window)) ShowWindow(window, SW_HIDE);
            if (g_overlayWindow && IsWindowVisible(g_overlayWindow))
                ShowWindow(g_overlayWindow, SW_HIDE);
        } else {
            if (GetWindow(window, GW_OWNER) != game) {
                SetWindowLongPtrW(window, GWLP_HWNDPARENT,
                                  reinterpret_cast<LONG_PTR>(game));
            }
            RECT client{};
            GetClientRect(game, &client);
            POINT origin{};
            ClientToScreen(game, &origin);
            const bool sessionButton=window==g_sessionButtonWindow;
            const int width = sessionButton?190:166, height = 30;
            constexpr int gap=8;
            const int total=166+gap+190;
            const int x = origin.x + client.right / 2 - total / 2 +
                (sessionButton?166+gap:0);
            const int y = origin.y + 128;
            placeOverlayWindow(window, x, y, width, height);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH background = CreateSolidBrush(RGB(13, 19, 23));
        FillRect(dc, &client, background);
        DeleteObject(background);
        HPEN border = CreatePen(PS_SOLID, 1, RGB(151, 124, 50));
        HGDIOBJ oldPen = SelectObject(dc, border);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, client.right, client.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(border);
        HFONT font = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(246, 197, 70));
        DrawTextW(dc, window==g_sessionButtonWindow?L"CHAT/SAVE":L"TRADE", -1, &client,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(window, message, wp, lp);
}

DWORD WINAPI overlayThread(void*) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW cls{};
    cls.lpfnWndProc = overlayWindowProc;
    cls.hInstance = g_module;
    cls.lpszClassName = L"FrostBridgeInGameOverlay";
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)); // IDC_HAND
    RegisterClassW(&cls);
    WNDCLASSW buttonClass{};
    buttonClass.lpfnWndProc = overlayButtonWindowProc;
    buttonClass.hInstance = g_module;
    buttonClass.lpszClassName = L"FrostBridgeInGameButton";
    buttonClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32649));
    RegisterClassW(&buttonClass);
    HWND owner = nullptr;
    for (int attempt = 0; attempt < 200 && !owner; ++attempt) {
        owner = findGameWindow();
        if (!owner) Sleep(25);
    }
    g_overlayWindow = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        cls.lpszClassName, L"FrostBridge Overlay", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, overlayWidth, overlayHeight, owner, nullptr, g_module, nullptr);
    if (!g_overlayWindow) {
        logLine(L"Could not create in-game resource overlay window.");
        return 1;
    }
    SetLayeredWindowAttributes(g_overlayWindow, 0, 235, LWA_ALPHA);
    g_overlayButtonWindow = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        buttonClass.lpszClassName, L"FrostBridge Multiplayer", WS_POPUP,
        0, 0, 166, 30, owner, nullptr, g_module, nullptr);
    if (!g_overlayButtonWindow) {
        logLine(L"Could not create in-game multiplayer button.");
        DestroyWindow(g_overlayWindow);
        g_overlayWindow = nullptr;
        return 1;
    }
    SetLayeredWindowAttributes(g_overlayButtonWindow, 0, 225, LWA_ALPHA);
    g_sessionButtonWindow = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        buttonClass.lpszClassName, L"FrostBridge Session", WS_POPUP,
        0, 0, 190, 30, owner, nullptr, g_module, nullptr);
    if (!g_sessionButtonWindow) {
        logLine(L"Could not create in-game chat/save button.");
        DestroyWindow(g_overlayButtonWindow);
        g_overlayButtonWindow = nullptr;
        DestroyWindow(g_overlayWindow);
        g_overlayWindow = nullptr;
        return 1;
    }
    SetLayeredWindowAttributes(g_sessionButtonWindow, 0, 225, LWA_ALPHA);
    SetTimer(g_overlayWindow, 1, 100, nullptr);
    SetTimer(g_overlayButtonWindow, 1, 100, nullptr);
    SetTimer(g_sessionButtonWindow, 1, 100, nullptr);
    logLine(L"In-game multiplayer resource overlay initialized.");
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
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
        const auto otherTitle = L"FrostBridge — " + std::wstring(lan?L"Steam":L"LAN") +
            L" — PID " + std::to_wstring(GetCurrentProcessId());
        if(HWND other=FindWindowW(L"FrostBridgeConnectionUI",otherTitle.c_str())) {
            DWORD_PTR ignored=0;
            SendMessageTimeoutW(other,kCloseForTransportSwitch,0,0,
                SMTO_ABORTIFHUNG,2000,&ignored);
            for(int attempt=0;attempt<100 &&
                FindWindowW(L"FrostBridgeConnectionUI",otherTitle.c_str());++attempt) Sleep(10);
        }
        std::wstring command = L"\"" + executable + L"\" --ui --overlay " +
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
    const auto title = L"FrostBridge — " + std::wstring(index?L"LAN":L"Steam") +
        L" — PID " + std::to_wstring(GetCurrentProcessId());
    if (HWND window = FindWindowW(L"FrostBridgeConnectionUI", title.c_str())) {
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        return;
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

void cancelPendingMapSelection() {
    if(!g_launchControl || g_launchStage<1 || g_launchStage>3) return;
    const LONG state=InterlockedCompareExchange(&g_launchControl->state,0,0);
    if(state!=frostlaunch::prepareRequested && state!=frostlaunch::prepared) return;
    InterlockedExchange(&g_launchControl->state,frostlaunch::failed);
    g_launchStage=0;
    g_nextMapIndex=0;
    g_selectedMapIndex=-1;
    g_mapSelectionIssued=false;
    g_endlessSelection=nullptr;
    g_endlessConfig=nullptr;
    logLine(L"Multiplayer map selection cancelled; another mode can be selected.");
}

void __fastcall menuPanelCallbackHook(void* owner, void* event) {
    std::uint32_t panelId = 0;
    if (event && safeRead(static_cast<std::uint8_t*>(event) + 0x18, panelId) &&
        panelId == kMultiplayerSourcePanelId) {
        cancelPendingMapSelection();
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
    if (g_launchControl && frostlaunch::isStory(g_launchControl->mapIndex) &&
        g_launchStage >= 1 && g_launchStage <= 3) {
        if (g_launchStage == 1 && g_launchControl->mapIndex == frostlaunch::chooseStory) {
            int index = -1, count = 0;
            auto* panel = static_cast<unsigned char*>(owner);
            if (!safeRead(panel + 0x1B0, index) || !safeRead(panel + 0x1A0, count) ||
                index < 0 || index >= count || count > 64) return;
            g_endlessConfig = owner;
            g_selectedMapIndex = frostlaunch::storyBase + index;
            InterlockedExchange(&g_launchControl->mapIndex, g_selectedMapIndex);
            g_launchStage = 3;
            InterlockedExchange(&g_launchControl->state, frostlaunch::prepared);
        }
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

void __fastcall endlessStartHook(void* panel, void* event) {
    if (g_launchControl && g_launchStage >= 2 && g_launchStage <= 3) {
        // Only the host's real Start click finalizes the selected map. Client
        // clicks cannot race the network commit or silently select another map.
        if (g_launchStage == 2 && g_launchControl->mapIndex < 0 && panel == g_endlessConfig) {
            int index = -1, count = 0;
            auto* bytes = static_cast<unsigned char*>(panel);
            if (!safeRead(bytes + 0x110, index) || !safeRead(bytes + 0x100, count) ||
                index < 0 || index >= count || count > 64) return;
            g_selectedMapIndex = index;
            InterlockedExchange(&g_launchControl->mapIndex, index);
            g_launchStage = 3;
            InterlockedExchange(&g_launchControl->state, frostlaunch::prepared);
            logLine(L"Host confirmed selected map; waiting for client preparation.");
        }
        return;
    }
    g_originalEndlessStart(panel, event);
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
        if (state == frostlaunch::prepared || state == frostlaunch::dispatched ||
            state == frostlaunch::failed) return;
        if (frostlaunch::isStory(g_launchControl->mapIndex)) {
            if (state == frostlaunch::prepareRequested && !g_launchStage) {
                if (!g_multiplayerMode.load() || !g_connectionPanel.load()) {
                    InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
                    return;
                }
                g_launchStage = 1;
                void* scenarioPanel = g_connectionPanel.load();
                g_endlessConfig = scenarioPanel;
                g_mapSelectionIssued = false;
                g_launchDeadline = GetTickCount64() + 600000;
                g_multiplayerMode.store(false);
                restoreConnectionButtons();
                g_connectionPanel.store(nullptr);
                alignas(8) unsigned char event[0x20]{};
                *reinterpret_cast<DWORD*>(event + 0x18) = 0x5B;
                g_originalMenuPanelCallback(nullptr, event);
                // This screen is already open as the transport selector. Merely
                // showing it again retains our labels and hidden rows. Native
                // SetView(0) clears/recreates scenario rows and localized labels.
                using SetScenarioView = void(__fastcall*)(void*, int);
                reinterpret_cast<SetScenarioView>(g_gameBase + 0x1A79390)(scenarioPanel, 0);
                return;
            }
            if (GetTickCount64() > g_launchDeadline) {
                InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
                return;
            }
            if (state == frostlaunch::prepareRequested && g_endlessConfig &&
                g_launchControl->mapIndex >= frostlaunch::storyBase) {
                const int requested = g_launchControl->mapIndex - frostlaunch::storyBase;
                int count = 0, selected = -1;
                auto* panel = static_cast<unsigned char*>(g_endlessConfig);
                if (!safeRead(panel + 0x1A0, count) || requested >= count || count > 64) {
                    InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
                    return;
                }
                if (!g_mapSelectionIssued) {
                    alignas(8) unsigned char event[0x20]{};
                    *reinterpret_cast<DWORD*>(event + 0x18) = requested;
                    g_originalScenarioRowCallback(g_endlessConfig, event);
                    g_mapSelectionIssued = true;
                    return;
                }
                void* start = panelElement(g_endlessConfig, kScenarioStartButtonOffset);
                std::uint32_t flags = 0;
                if (!safeRead(panel + 0x1B0, selected) || selected != requested ||
                    !start || !safeRead(static_cast<unsigned char*>(start) + 0x270, flags) || !(flags & 8)) return;
                g_selectedMapIndex = g_launchControl->mapIndex;
                g_launchStage = 3;
                InterlockedExchange(&g_launchControl->state, frostlaunch::prepared);
            } else if (state == frostlaunch::commitRequested && g_launchStage == 3 && g_endlessConfig) {
                g_launchStage = 4;
                g_launchDeadline = GetTickCount64() + 90000;
                // Retain the game's entitlement and progression checks.
                g_originalScenariosStartCallback(g_endlessConfig, nullptr);
            } else if (state == frostlaunch::commitRequested && g_launchStage == 4 && g_sessionLoaded) {
                InterlockedExchange(&g_launchControl->state, frostlaunch::dispatched);
            }
            return;
        }
        if (state == frostlaunch::commitRequested) {
            if (g_launchStage == 4) {
                if (g_sessionLoaded) {
                    InterlockedExchange(&g_launchControl->state, frostlaunch::dispatched);
                    logLine(L"Endless launch: city load confirmed.");
                } else if (GetTickCount64() > g_launchDeadline) {
                    InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
                    logLine(L"Endless launch: native callback did not load a city.");
                }
                return;
            }
            if (g_launchStage != 3 || !g_endlessConfig) {
                InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
                return;
            }
            g_launchStage = 4; // exactly once, including reentrant callbacks
            int actual = -1;
            if (!safeRead(static_cast<unsigned char*>(g_endlessConfig) + 0x110, actual) ||
                actual != g_selectedMapIndex) {
                InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
                return;
            }
            g_originalEndlessStart(g_endlessConfig, nullptr);
            g_launchDeadline = GetTickCount64() + 90000;
            logLine(L"Endless launch: synchronized native start callback dispatched.");
            return;
        }
        if (state != frostlaunch::prepareRequested) return;
        if (!g_launchStage) {
            if (!g_multiplayerMode.load() || !g_connectionPanel.load()) {
                InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
                return;
            }
            g_launchStage = 1;
            g_nextMapIndex = 0;
            g_selectedMapIndex = -1;
            g_mapSelectionIssued = false;
            g_launchDeadline = GetTickCount64() + 600000;
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
            g_mapSelectionIssued = false;
            reinterpret_cast<MenuPanelCallback>(g_gameBase + 0x1A879C0)(g_endlessSelection, event);
            logLine(L"Endless launch: first native mode selected.");
        } else if (g_launchStage == 2 && g_endlessConfig) {
            auto* panel = static_cast<unsigned char*>(g_endlessConfig);
            int count = 0, index = -1;
            void* definition = nullptr;
            if (!safeRead(panel + 0xF0, definition) || !definition ||
                !safeRead(panel + 0x100, count) || !safeRead(panel + 0x110, index) ||
                index < 0 || index >= count) return;
            LONG requestedMap = InterlockedCompareExchange(&g_launchControl->mapIndex, 0, 0);
            if (requestedMap < 0) return; // host chooses and confirms via native Start
            if (count > 64 || (requestedMap >= 0 && requestedMap >= count) ||
                (requestedMap < 0 && g_nextMapIndex >= count)) {
                InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
                logLine(L"Endless launch: requested map is unavailable.");
                return;
            }
            if (!g_mapSelectionIssued) {
                // The host discovers its first enabled map. The client receives
                // that exact index over the network instead of guessing again.
                g_selectedMapIndex = requestedMap >= 0 ? requestedMap : g_nextMapIndex++;
                using SelectMap = void(__fastcall*)(void*, int);
                reinterpret_cast<SelectMap>(g_gameBase + 0x1A8B880)(
                    g_endlessConfig, g_selectedMapIndex);
                g_mapSelectionIssued = true;
                return; // let the native panel refresh Start enabled state
            }
            if (index != g_selectedMapIndex) { g_mapSelectionIssued = false; return; }
            void* root = panelElement(g_endlessConfig, kPanelRootOffset);
            void* start = root ? findElement(root, "START_BUTTON") : nullptr;
            std::uint32_t flags = 0;
            if (!start || !safeRead(static_cast<unsigned char*>(start) + 0x270, flags)) return;
            if (!(flags & 8)) {
                if (requestedMap >= 0) {
                    InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
                    logLine(L"Endless launch: host-selected map is unavailable on this client.");
                } else {
                    g_mapSelectionIssued = false;
                }
                return;
            }
            InterlockedExchange(&g_launchControl->mapIndex, g_selectedMapIndex);
            g_launchStage = 3;
            InterlockedExchange(&g_launchControl->state, frostlaunch::prepared);
            logLine(L"Endless launch: synchronized map prepared; waiting for commit.");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_launchControl->state, frostlaunch::failed);
        logLine(L"Endless launch rejected an invalid native state.");
    }
}

frostsave::Control* g_saveControl = nullptr;
HANDLE g_saveMapping = nullptr;
InlineHook g_saveHook, g_loadHook;
using NativeSave = bool(__fastcall*)(void*, const wchar_t*, bool, bool);
using NativeLoad = void(__fastcall*)(void*, const wchar_t*);
NativeSave g_originalSave = nullptr;
NativeLoad g_originalLoad = nullptr;

bool publishSaveEvent(LONG operation, const wchar_t* name) {
    if (!g_saveControl || g_saveControl->role != 1 || !name ||
        GetTickCount64()-static_cast<ULONGLONG>(InterlockedCompareExchange64(&g_saveControl->heartbeat,0,0))>2000) return false;
    // Hook may run inside engine callbacks: bounded copy only, no allocation/I/O.
    wchar_t copy[frostsave::nameCapacity]{};
    size_t i=0;
    for (; i+1<std::size(copy); ++i) {
        if(!safeRead(name+i,copy[i])) return true;
        if(!copy[i]) break;
    }
    if(i+1==std::size(copy)) copy[0]=0; // bridge reports invalid/too long
    g_saveControl->eventOperation=operation;
    std::memcpy(g_saveControl->eventName,copy,sizeof(copy));
    MemoryBarrier(); InterlockedIncrement(&g_saveControl->eventSequence);
    return true;
}

bool __fastcall saveRequestHook(void* owner, const wchar_t* name, bool automatic, bool extra) {
    if(!automatic && !extra && publishSaveEvent(frostsave::save,name)) return true;
    return g_originalSave(owner,name,automatic,extra);
}
void __fastcall loadRequestHook(void* owner, const wchar_t* name) {
    if(publishSaveEvent(frostsave::load,name)) return;
    g_originalLoad(owner,name);
}

void advanceSaveControl() {
    if(!g_saveControl || !g_originalSave || !g_originalLoad) return;
    const LONG sequence=InterlockedCompareExchange(&g_saveControl->commandSequence,0,0);
    if(sequence==g_saveControl->ackSequence) return;
    MemoryBarrier();
    wchar_t name[frostsave::nameCapacity]{};
    std::memcpy(name,g_saveControl->commandName,sizeof(name)); name[std::size(name)-1]=0;
    const auto valid=frostsave::slotName(name);
    LONG result=-1;
    if(valid && *valid==name && g_saveControl->role) {
        void* owner=reinterpret_cast<void*>(g_gameBase+0x2B68120);
        std::uintptr_t vtable=0;
        if(safeRead(owner,vtable) && vtable==g_gameBase+0x1DF9AE0) {
            if(g_saveControl->commandOperation==frostsave::save && g_sessionControl && g_sessionControl->gameLoaded)
                result=g_originalSave(owner,name,false,false)?1:-1;
            else if(g_saveControl->commandOperation==frostsave::load) {
                g_originalLoad(owner,name); result=1;
            }
        }
    }
    InterlockedExchange(&g_saveControl->result,result);
    MemoryBarrier(); InterlockedExchange(&g_saveControl->ackSequence,sequence);
}

bool installSaveHooks() {
    constexpr unsigned char saveBytes[]{0x40,0x57,0x48,0x83,0xec,0x40,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff};
    constexpr unsigned char loadBytes[]{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff};
    if(!g_saveHook.install(reinterpret_cast<void*>(g_gameBase+0x11B5F80),reinterpret_cast<void*>(saveRequestHook),saveBytes,sizeof(saveBytes))) return false;
    g_originalSave=g_saveHook.original<NativeSave>();
    if(!g_loadHook.install(reinterpret_cast<void*>(g_gameBase+0x11B5E30),reinterpret_cast<void*>(loadRequestHook),loadBytes,sizeof(loadBytes))) return false;
    g_originalLoad=g_loadHook.original<NativeLoad>();
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(frostsave::Control),frostsave::name(GetCurrentProcessId()).c_str());
    g_saveMapping=mapping;
    if(!mapping) return false;
    const bool created=GetLastError()!=ERROR_ALREADY_EXISTS;
    g_saveControl=static_cast<frostsave::Control*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(frostsave::Control)));
    if(!g_saveControl) return false;
    if(created) {
        ZeroMemory(g_saveControl,sizeof(*g_saveControl));
        g_saveControl->layoutVersion=frostsave::version;
        MemoryBarrier(); g_saveControl->signature=frostsave::magic;
    }
    if(g_saveControl->signature!=frostsave::magic || g_saveControl->layoutVersion!=frostsave::version) return false;
    InterlockedExchange(&g_saveControl->modReady,1);
    return true;
}

LRESULT CALLBACK launchMessageHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && wp == PM_REMOVE) {
        auto* message = reinterpret_cast<MSG*>(lp);
        observeSessionInput(*message);
        if (message->message == g_launchMessage) {
            message->message = WM_NULL;
            advanceEndlessLaunch();
            advanceOverlayApply();
            advanceSessionControl();
            advanceSaveControl();
            g_sessionTickPending.store(false, std::memory_order_release);
        }
    }
    return CallNextHookEx(g_messageHook, code, wp, lp);
}

// Own the game-thread hook and its wakeups independently of expensive menu scans.
// A single outstanding message bounds the queue while the native loader is busy.
DWORD WINAPI sessionPumpThread(void*) {
    for (;;) {
        if (!g_messageHook) {
            if (HWND window = findGameWindow()) {
                g_uiThread = GetWindowThreadProcessId(window, nullptr);
                g_messageHook = SetWindowsHookExW(WH_GETMESSAGE, launchMessageHook,
                                                g_module, g_uiThread);
            }
        }
        if (g_messageHook && g_launchMessage &&
            !g_sessionTickPending.exchange(true, std::memory_order_acq_rel)) {
            if (!PostThreadMessageW(g_uiThread, g_launchMessage, 0, 0))
                g_sessionTickPending.store(false, std::memory_order_release);
        }
        Sleep(10);
    }
}

bool installHooks() {
    if(!installSaveHooks()) return false;
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
    if (!g_endlessStartHook.install(reinterpret_cast<void*>(g_gameBase + 0x1A8ADE0),
            reinterpret_cast<void*>(endlessStartHook), prologue, sizeof(prologue))) return false;
    g_originalEndlessStart = g_endlessStartHook.original<MenuPanelCallback>();
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
    const auto overlayName = frostoverlay::name(GetCurrentProcessId());
    g_overlayMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(frostoverlay::Control), overlayName.c_str());
    const bool overlayCreated = g_overlayMapping && GetLastError() != ERROR_ALREADY_EXISTS;
    if (g_overlayMapping) {
        g_overlayControl = static_cast<frostoverlay::Control*>(MapViewOfFile(
            g_overlayMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(frostoverlay::Control)));
    }
    if (g_overlayControl && overlayCreated) {
        ZeroMemory(g_overlayControl, sizeof(*g_overlayControl));
        g_overlayControl->localHope = g_overlayControl->localDiscontent = -1;
        g_overlayControl->peerHope = g_overlayControl->peerDiscontent = -1;
        g_overlayControl->layoutVersion = frostoverlay::version;
        MemoryBarrier();
        g_overlayControl->signature = frostoverlay::magic;
    }
    if (g_overlayControl && g_overlayControl->signature == frostoverlay::magic &&
        g_overlayControl->layoutVersion == frostoverlay::version) {
        InterlockedExchange(&g_overlayControl->modReady, 1);
        HANDLE thread = CreateThread(nullptr, 0, overlayThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else {
        logLine(L"Could not initialize overlay IPC mapping.");
    }
    const auto sessionName = frostsession::name(GetCurrentProcessId());
    g_sessionMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(frostsession::Control), sessionName.c_str());
    const bool sessionCreated = g_sessionMapping && GetLastError() != ERROR_ALREADY_EXISTS;
    if (g_sessionMapping) {
        g_sessionControl = static_cast<frostsession::Control*>(MapViewOfFile(
            g_sessionMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(frostsession::Control)));
    }
    if (g_sessionControl && sessionCreated) {
        ZeroMemory(g_sessionControl, sizeof(*g_sessionControl));
        g_sessionControl->layoutVersion = frostsession::version;
        g_sessionControl->paused = 1;
        g_sessionControl->localPauseValue = 1;
        g_sessionControl->pauseCommandValue = 1;
        g_sessionControl->localSpeedValue = -1;
        g_sessionControl->currentSpeed = -1;
        g_sessionControl->speedCommandValue = -1;
        MemoryBarrier();
        g_sessionControl->signature = frostsession::magic;
    }
    if (g_sessionControl && g_sessionControl->signature == frostsession::magic &&
        g_sessionControl->layoutVersion == frostsession::version) {
        InterlockedExchange(&g_sessionControl->modReady, 1);
    } else {
        logLine(L"Could not initialize multiplayer session IPC mapping.");
    }
    const auto mappingName = frostlaunch::name(GetCurrentProcessId());
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(frostlaunch::Control), mappingName.c_str());
    if (mapping) g_launchControl = static_cast<frostlaunch::Control*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(frostlaunch::Control)));
    if (g_launchControl) {
        g_launchControl->layoutVersion = frostlaunch::version;
        g_launchControl->mapIndex = -1;
        MemoryBarrier();
        g_launchControl->signature = frostlaunch::magic;
        InterlockedExchange(&g_launchControl->state, frostlaunch::unavailable);
    }
    g_launchMessage = RegisterWindowMessageW(L"FrostBridge.NativeLaunch.Tick.V1");
    HANDLE sessionPump = CreateThread(nullptr, 0, sessionPumpThread, nullptr, 0, nullptr);
    if (sessionPump) CloseHandle(sessionPump);
    else logLine(L"Could not start session synchronization pump.");

    // The panels can already be open when the DLL is injected. Hooks handle all
    // later updates; this worker supplies a persistent attach-time fallback.
    logLine(L"Waiting for Frostpunk main-menu panels.");
    ULONGLONG nextMaintenance = 0;
    for (;;) {
        if (GetTickCount64() < nextMaintenance) { Sleep(10); continue; }
        nextMaintenance = GetTickCount64() + 250;
        // Overlay timers run on their own UI thread; scanning a missing menu
        // panel must never delay the in-city Multiplayer button.
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
        Sleep(10);
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
