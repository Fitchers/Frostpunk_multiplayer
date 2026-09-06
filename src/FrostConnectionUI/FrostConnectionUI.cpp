#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include <stdexcept>
#include "../FrostBridgeNet/ConnectionSession.h"

namespace {
constexpr int kName=101, kAddress=102, kPort=103, kHost=104, kJoin=105,
              kStop=106, kLog=107, kStatus=108, kChat=109, kSend=110, kStart=111,
              kSaveName=112,kSave=113,kLoad=114,kStory=115;
constexpr wchar_t kClass[] = L"FrostBridgeConnectionUI";
constexpr UINT kCloseForTransportSwitch=WM_APP+42;
HWND g_window{};
HFONT g_font{};
bool g_steam=false;
DWORD g_pid=0;
bool g_menuOverlay=false;
bool g_panelOpen=true;
HWND g_gameWindow{};
HBRUSH g_panelBrush=CreateSolidBrush(RGB(17,25,31));
HWND findOwner() {
    HWND result=nullptr;
    EnumWindows([](HWND window,LPARAM param)->BOOL {
        DWORD pid=0; GetWindowThreadProcessId(window,&pid);
        wchar_t cls[64]{}; GetClassNameW(window,cls,64);
        if(pid==g_pid && wcscmp(cls,L"SDL_app")==0) { *reinterpret_cast<HWND*>(param)=window; return FALSE; }
        return TRUE;
    },reinterpret_cast<LPARAM>(&result));
    return result;
}
bool g_host=false, g_connected=false, g_startPending=false;
std::wstring g_initialName;
std::wstring g_initialAddress=L"127.0.0.1";
std::wstring g_initialPort=L"27020";
int g_autoAction=0; // 1 host, 2 join
HANDLE g_game{};
std::unique_ptr<frostbridge::ConnectionSession> g_session;

void closeHandle(HANDLE& handle) { if(handle) CloseHandle(handle); handle=nullptr; }
std::wstring fromUtf8(const std::string& value) {
    int n=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);
    std::wstring result(n,L'\0');
    MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),n);
    return result;
}
std::string toUtf8(const std::wstring& value) {
    int n=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(n,'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),n,nullptr,nullptr);
    return result;
}
std::wstring field(int id) {
    HWND control=GetDlgItem(g_window,id);
    std::wstring value(GetWindowTextLengthW(control)+1,L'\0');
    GetWindowTextW(control,value.data(),static_cast<int>(value.size()));
    value.resize(wcslen(value.c_str()));
    const auto first=value.find_first_not_of(L" \t\r\n");
    if(first==std::wstring::npos) return {};
    return value.substr(first,value.find_last_not_of(L" \t\r\n")-first+1);
}
void status(const wchar_t* text) { SetDlgItemTextW(g_window,kStatus,text); }
void log(const std::wstring& text) {
    HWND edit=GetDlgItem(g_window,kLog);
    if(GetWindowTextLengthW(edit)>50000) SetWindowTextW(edit,L"");
    SendMessageW(edit,EM_SETSEL,static_cast<WPARAM>(-1),static_cast<LPARAM>(-1));
    const auto line=text+L"\r\n";
    SendMessageW(edit,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(line.c_str()));
}
void controls(bool active) {
    for(int id:{kName,kAddress,kJoin}) EnableWindow(GetDlgItem(g_window,id),!active);
    EnableWindow(GetDlgItem(g_window,kPort),!active&&!g_steam);
    EnableWindow(GetDlgItem(g_window,kHost),!active&&!g_steam);
    EnableWindow(GetDlgItem(g_window,kStop),active);
    EnableWindow(GetDlgItem(g_window,kChat),active);
    EnableWindow(GetDlgItem(g_window,kSend),active);
    EnableWindow(GetDlgItem(g_window,kStart),active && g_host && g_connected && !g_startPending);
    EnableWindow(GetDlgItem(g_window,kStory),active && g_host && g_connected && !g_startPending);
    for(int id:{kSaveName,kSave,kLoad}) EnableWindow(GetDlgItem(g_window,id),active && g_host && g_connected);
}
void stopSession() {
    // Cancel our network thread and release sockets. No process termination.
    g_session.reset();
    g_connected=false; g_startPending=false; controls(false);
}
void pumpOutput() {
    if(!g_session) return;
    for(const auto& line : g_session->takeOutput()) {
            bool show=line.rfind("[you] ",0)==0 || line.rfind("[save]",0)==0 || line.rfind("[trade]",0)==0 ||
                line.rfind("[game]",0)==0 || line.rfind("error:",0)==0 ||
                line.rfind("[connection]",0)==0;
            if(line=="[role] host" || line=="[role] client") {
                g_host=line=="[role] host"; controls(true);
                status(g_host?L"You are the host. Choose a mode and start the game.":L"You are the client. Waiting for the host to start.");
                log(g_host?L"Role: host.":L"Role: client.");
            }
            if(line.rfind("[peer] player:",0)==0) {
                const bool first=!g_connected;
                g_connected=true; controls(true);
                status(g_host?L"Player connected. Choose a mode to start.":L"Connected. Waiting for the host to start.");
                if(first) log(L"Second player connected.");
            } else if(line.rfind("[peer] ",0)==0) {
                show=true;
            }
            if(line.rfind("[game] rejected:",0)==0) {
                g_startPending=false; controls(true); status(L"Start cancelled — see the log below");
            }
            if(line.rfind("[game] launching:",0)==0 || line.rfind("[game] loading:",0)==0)
                status(L"The map is starting. You can minimize this panel.");
            if(line.rfind("[game] failed:",0)==0 || line.rfind("[game] peer-failed:",0)==0)
                { g_startPending=false; controls(true); status(L"Map selection was cancelled or failed"); }
            if(line.rfind("[save] Waiting",0)==0)
                status(L"Saving or loading on both computers…");
            if(line.rfind("[save] Checkpoint complete",0)==0)
                status(L"Done. The save is available with the _multiplayer suffix.");
            if(line.rfind("[save] Failed",0)==0 || line.rfind("[save] Disconnected",0)==0)
                status(L"Save or load failed — see the message below");
            if(line.rfind("[connection] disconnected",0)==0) {
                g_connected=false; controls(true);
                status(L"Connection closed. Click Disconnect before reconnecting.");
            }
            if(line.rfind("error:",0)==0) status(L"Connection error — see details below");
            if(show) log(fromUtf8(line));
    }
}
void startSession(bool host) {
    if(g_session) return;
    const auto name=field(kName), address=field(kAddress), port=field(kPort);
    if(name.empty()||toUtf8(name).size()>63||name.find_first_of(L"\r\n")!=std::wstring::npos)
        throw std::runtime_error("Enter a player name from 1 to 63 UTF-8 bytes.");
    if(port.empty()||port.find_first_not_of(L"0123456789")!=std::wstring::npos||port.size()>5||std::stoul(port)==0||std::stoul(port)>65535)
        throw std::runtime_error("Port must be a number from 1 to 65535.");
    if(!host) {
        if(g_steam) {
            if(address.size()!=17||address.find_first_not_of(L"0123456789")!=std::wstring::npos)
                throw std::runtime_error("Enter the other player's 17-digit SteamID64.");
        } else if(address.empty()||address.find_first_not_of(L"0123456789.")!=std::wstring::npos)
            throw std::runtime_error("Enter the host IPv4 address without a port. Same PC: 127.0.0.1.");
    }
    if(g_game&&WaitForSingleObject(g_game,0)==WAIT_OBJECT_0)
        throw std::runtime_error("This Frostpunk instance has already closed.");
    frostbridge::ConnectionOptions options;
    options.steam=g_steam; options.host=host; options.gamePid=g_pid;
    options.playerName=toUtf8(name); options.address=toUtf8(address);
    options.port=static_cast<std::uint16_t>(std::stoul(port));
    g_session=frostbridge::connect(std::move(options));
    g_host=host; g_connected=false; g_startPending=false; controls(true);
    status(host?L"Waiting for the second player…":L"Connecting…");
    log(L"Player: "+name+L" | Frostpunk PID: "+std::to_wstring(g_pid));
}
HWND control(const wchar_t* cls,const wchar_t* text,int id,int x,int y,int w,int h,DWORD style=0) {
    HWND window=CreateWindowExW(wcscmp(cls,L"EDIT")==0?WS_EX_CLIENTEDGE:0,cls,text,
        WS_CHILD|WS_VISIBLE|style,x,y,w,h,g_window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(g_font),TRUE); return window;
}
LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    switch(message) {
    case WM_CREATE:
        g_window=window;
        g_font=CreateFontW(-18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        control(L"STATIC",g_steam?L"STEAM P2P":L"LOCAL NETWORK — direct connection",0,20,16,650,28);
        control(L"STATIC",L"Player name",0,20,58,145,24);
        control(L"EDIT",g_initialName.c_str(),kName,170,54,490,30,WS_TABSTOP|ES_AUTOHSCROLL);
        control(L"STATIC",g_steam?L"Friend's SteamID64":L"Host address",0,20,100,145,24);
        control(L"EDIT",g_steam?L"":g_initialAddress.c_str(),kAddress,170,96,320,30,WS_TABSTOP|ES_AUTOHSCROLL);
        control(L"STATIC",L"Port",0,504,100,45,24);
        control(L"EDIT",g_initialPort.c_str(),kPort,553,96,107,30,WS_TABSTOP|ES_NUMBER);
        control(L"STATIC",g_steam?L"Both players enter each other's SteamID64. The first to connect becomes host.":L"Same PC: 127.0.0.1. Different PCs: use the host's local IPv4 address.",0,20,140,650,24);
        control(L"BUTTON",L"HOST",kHost,20,177,180,34,WS_TABSTOP);
        if(g_steam) ShowWindow(GetDlgItem(window,kHost),SW_HIDE);
        control(L"BUTTON",g_steam?L"CONNECT (automatic host)":L"CONNECT",kJoin,g_steam?20:217,177,g_steam?417:220,34,WS_TABSTOP);
        control(L"BUTTON",L"DISCONNECT",kStop,454,177,206,34,WS_TABSTOP);
        control(L"STATIC",L"Enter your name and connect",kStatus,20,220,260,52);
        control(L"BUTTON",L"ENDLESS MODE",kStart,290,225,180,38,WS_TABSTOP);
        control(L"BUTTON",L"STORY SCENARIO",kStory,480,225,180,38,WS_TABSTOP);
        control(L"EDIT",L"survival",kSaveName,20,280,280,30,WS_TABSTOP|ES_AUTOHSCROLL);
        control(L"BUTTON",L"SAVE FOR ALL",kSave,313,280,167,30,WS_TABSTOP);
        control(L"BUTTON",L"LOAD FOR ALL",kLoad,493,280,167,30,WS_TABSTOP);
        control(L"STATIC",L"Name + _multiplayer. Use the same player name when resuming.",0,20,314,650,24);
        control(L"EDIT",L"",kLog,20,342,640,183,WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL);
        control(L"EDIT",L"",kChat,20,539,480,30,WS_TABSTOP|ES_AUTOHSCROLL);
        control(L"BUTTON",L"SEND",kSend,515,539,145,30,WS_TABSTOP);
        if(g_menuOverlay) control(L"BUTTON",L"MINIMIZE",116,550,8,110,30,WS_TABSTOP);
        control(L"STATIC",L"You can minimize this panel and keep playing. Closing disconnects the session.",0,20,582,655,25);
        SendDlgItemMessageW(window,kName,EM_SETLIMITTEXT,63,0);
        SendDlgItemMessageW(window,kAddress,EM_SETLIMITTEXT,253,0);
        SendDlgItemMessageW(window,kPort,EM_SETLIMITTEXT,5,0);
        SendDlgItemMessageW(window,kChat,EM_SETLIMITTEXT,500,0);
        SendDlgItemMessageW(window,kSaveName,EM_SETLIMITTEXT,84,0);
        controls(false); SetTimer(window,1,100,nullptr);
        if(g_autoAction) PostMessageW(window,WM_COMMAND,
            MAKEWPARAM(g_autoAction==1?kHost:kJoin,BN_CLICKED),0);
        return 0;
    case WM_COMMAND:
        if(HIWORD(wp)!=BN_CLICKED) break;
        try {
            switch(LOWORD(wp)) {
            case 116: g_panelOpen=false; ShowWindow(window,SW_HIDE); break;
            case kHost: startSession(true); break;
            case kJoin: startSession(false); break;
            case kSave:
            case kLoad:
                if(g_session && g_host && g_connected)
                    g_session->send(std::string(LOWORD(wp)==kSave?"save ":"load ")+toUtf8(field(kSaveName)));
                break;
            case kStop: stopSession(); status(L"Disconnected"); log(L"Session stopped."); break;
            case kStory:
            case kStart: {
                if (!g_session || !g_host || !g_connected || g_startPending) break;
                g_session->send(LOWORD(wp) == kStory ? "start story" : "start");
                if(g_menuOverlay) { g_panelOpen=false; ShowWindow(window,SW_HIDE); }
                g_startPending=true; controls(true); status(L"Checking both games…");
                break;
            }
            case kSend: {
                auto messageText=field(kChat);
                if(g_session&&!messageText.empty()) {
                    g_session->send("chat "+toUtf8(messageText));
                    SetDlgItemTextW(window,kChat,L"");
                }
                break;
            }
            }
        } catch(const std::exception& error) { status(L"Error — check the connection details"); log(fromUtf8(error.what())); }
        return 0;
    case WM_CTLCOLORSTATIC:
        if(g_menuOverlay) {
            SetTextColor(reinterpret_cast<HDC>(wp),RGB(237,202,115));
            SetBkColor(reinterpret_cast<HDC>(wp),RGB(17,25,31));
            return reinterpret_cast<LRESULT>(g_panelBrush);
        }
        break;
    case WM_SHOWWINDOW:
        if(wp) g_panelOpen=true;
        break;
    case WM_TIMER:
        if(g_menuOverlay && g_gameWindow) {
            DWORD foregroundPid=0; GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);
            if(!g_panelOpen || IsIconic(g_gameWindow) || (foregroundPid!=g_pid && foregroundPid!=GetCurrentProcessId())) {
                ShowWindow(window,SW_HIDE);
            } else {
                RECT rect{}; GetClientRect(g_gameWindow,&rect);
                POINT origin{}; ClientToScreen(g_gameWindow,&origin);
                SetWindowPos(window,HWND_TOPMOST,origin.x+(rect.right-680)/2,
                    origin.y+(rect.bottom-620)/2,680,620,SWP_NOACTIVATE|SWP_SHOWWINDOW);
            }
        }
        pumpOutput();
        if(g_session&&g_session->finished()) {
            const bool failed=g_session->failed();
            pumpOutput(); stopSession();
            status(failed?L"Connection failed — see the log":L"Connection closed. You can reconnect.");
        }
        if(g_game&&WaitForSingleObject(g_game,0)==WAIT_OBJECT_0) {
            stopSession(); DestroyWindow(window);
        }
        return 0;
    case WM_CLOSE:
        if(g_menuOverlay) { g_panelOpen=false; ShowWindow(window,SW_HIDE); return 0; }
        if(g_session&&MessageBoxW(window,L"Disconnect the network session and close this window?",L"FrostBridge",MB_YESNO|MB_ICONQUESTION)!=IDYES) return 0;
        DestroyWindow(window); return 0;
    case kCloseForTransportSwitch:
        DestroyWindow(window); return 0;
    case WM_DESTROY:
        KillTimer(window,1); stopSession(); closeHandle(g_game);
        if(g_font) DeleteObject(g_font); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
}
int frostbridge::runConnectionUI(const std::vector<std::wstring>& args) {
    try {
        const HINSTANCE instance=GetModuleHandleW(nullptr);
        for(std::size_t i=0;i<args.size();++i) {
            if(args[i]==L"--overlay") g_menuOverlay=true;
            else if(args[i]==L"--steam") g_steam=true;
            else if(args[i]==L"--lan") g_steam=false;
            else if(args[i]==L"--name"&&i+1<args.size()) g_initialName=args[++i];
            else if(args[i]==L"--address"&&i+1<args.size()) g_initialAddress=args[++i];
            else if(args[i]==L"--port"&&i+1<args.size()) g_initialPort=args[++i];
            else if(args[i]==L"--auto-host") g_autoAction=1;
            else if(args[i]==L"--auto-join") g_autoAction=2;
            else if(args[i]==L"--pid"&&i+1<args.size()) {
                const auto value=args[++i];
                if(value.empty()||value.find_first_not_of(L"0123456789")!=std::wstring::npos) throw std::runtime_error("Invalid PID.");
                auto pid=std::stoull(value);
                if(!pid||pid>MAXDWORD) throw std::runtime_error("Invalid PID.");
                g_pid=static_cast<DWORD>(pid);
            } else throw std::runtime_error("Usage: [--lan | --steam] [--pid PID] [--name NAME] [--address IP] [--port PORT] [--auto-host | --auto-join]");
        }
        if(g_autoAction&&g_initialName.empty()) throw std::runtime_error("Automatic connection requires --name.");
        const auto title=std::wstring(L"FrostBridge — ")+(g_steam?L"Steam":L"LAN")+L" — PID "+std::to_wstring(g_pid);
        // One UI/bridge per city, including when called again from the menu.
        const auto mutexName=L"Local\\FrostBridgeUI-"+std::to_wstring(g_pid);
        HANDLE mutex=nullptr;
        for(int attempt=0;attempt<80;++attempt) {
            mutex=CreateMutexW(nullptr,FALSE,mutexName.c_str());
            if(!mutex) throw std::runtime_error("Could not create the session lock.");
            if(GetLastError()!=ERROR_ALREADY_EXISTS) break;
            for(const auto* transport:{L"Steam",L"LAN"}) {
                const auto other=L"FrostBridge — "+std::wstring(transport)+L" — PID "+std::to_wstring(g_pid);
                if(HWND existing=FindWindowW(kClass,other.c_str())) {
                    ShowWindow(existing,SW_RESTORE); SetForegroundWindow(existing);
                    CloseHandle(mutex); return 0;
                }
            }
            CloseHandle(mutex); mutex=nullptr;
            Sleep(25); // previous transport is between DestroyWindow and process exit
        }
        if(!mutex) throw std::runtime_error("The previous connection panel is still closing. Try again.");
        if(g_pid) {
            g_game=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,g_pid);
            wchar_t path[32768]{}; DWORD size=32768;
            if(!g_game||!QueryFullProcessImageNameW(g_game,0,path,&size)||
                _wcsicmp(std::filesystem::path(path).filename().c_str(),L"Frostpunk.exe")!=0)
                throw std::runtime_error("The selected PID is not an accessible Frostpunk process.");
        }
        WNDCLASSW cls{}; cls.lpfnWndProc=windowProc; cls.hInstance=instance;
        cls.lpszClassName=kClass; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground=g_menuOverlay?g_panelBrush:reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        RegisterClassW(&cls);
        RECT rect{0,0,680,620}; AdjustWindowRect(&rect,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);
        g_gameWindow=g_menuOverlay?findOwner():nullptr;
        HWND window=CreateWindowExW(g_menuOverlay?WS_EX_TOOLWINDOW:0,kClass,title.c_str(),
            g_menuOverlay?WS_POPUP:(WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX),
            CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,g_gameWindow,nullptr,instance,nullptr);
        if(!window) throw std::runtime_error("Could not open the connection panel.");
        ShowWindow(window,SW_SHOW); SetFocus(GetDlgItem(window,kName));
        MSG message{};
        while(GetMessageW(&message,nullptr,0,0)>0) {
            if(!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        CloseHandle(mutex); return 0;
    } catch(const std::exception& error) {
        MessageBoxW(nullptr,fromUtf8(error.what()).c_str(),L"FrostBridge",MB_OK|MB_ICONERROR); return 1;
    }
}
