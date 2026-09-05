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
HWND g_window{};
HFONT g_font{};
bool g_steam=false;
bool g_host=false, g_connected=false, g_startPending=false;
std::wstring g_initialName;
std::wstring g_initialAddress=L"127.0.0.1";
std::wstring g_initialPort=L"27020";
int g_autoAction=0; // 1 host, 2 join
DWORD g_pid=0;
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
            log(fromUtf8(line));
            if(line=="[role] host" || line=="[role] client") { g_host=line=="[role] host"; controls(true); }
            if(line.rfind("[peer] player:",0)==0) {
                g_connected=true; controls(true);
                status(g_host?L"Игрок подключён. Нажмите «Начать игру».":L"Подключено. Ждём, когда хост начнёт игру.");
            }
            if(line.rfind("[game] rejected:",0)==0) {
                g_startPending=false; controls(true); status(L"Запуск отменён — подробности в чате");
            }
            if(line.rfind("[game] launching:",0)==0 || line.rfind("[game] loading:",0)==0)
                status(L"Карта запускается. Окно чата можно свернуть.");
            if(line.rfind("[game] failed:",0)==0 || line.rfind("[game] peer-failed:",0)==0)
                status(L"Ошибка запуска города — подробности в чате");
            if(line.rfind("[connection] disconnected",0)==0) {
                g_connected=false; controls(true);
                status(L"Соединение закрыто. Нажмите «Отключиться» для нового подключения.");
            }
            if(line.rfind("error:",0)==0) status(L"Ошибка подключения — подробности ниже");
    }
}
void startSession(bool host) {
    if(g_session) return;
    const auto name=field(kName), address=field(kAddress), port=field(kPort);
    if(name.empty()||toUtf8(name).size()>63||name.find_first_of(L"\r\n")!=std::wstring::npos)
        throw std::runtime_error("Введите имя игрока: от 1 до 63 байт UTF-8.");
    if(port.empty()||port.find_first_not_of(L"0123456789")!=std::wstring::npos||port.size()>5||std::stoul(port)==0||std::stoul(port)>65535)
        throw std::runtime_error("Порт должен быть числом от 1 до 65535.");
    if(!host) {
        if(g_steam) {
            if(address.size()!=17||address.find_first_not_of(L"0123456789")!=std::wstring::npos)
                throw std::runtime_error("Введите 17-значный SteamID64 второго игрока.");
        } else if(address.empty()||address.find_first_not_of(L"0123456789.")!=std::wstring::npos)
            throw std::runtime_error("Введите IPv4 хоста без порта. Для одного ПК: 127.0.0.1.");
    }
    if(g_game&&WaitForSingleObject(g_game,0)==WAIT_OBJECT_0)
        throw std::runtime_error("Этот экземпляр Frostpunk уже закрыт.");
    frostbridge::ConnectionOptions options;
    options.steam=g_steam; options.host=host; options.gamePid=g_pid;
    options.playerName=toUtf8(name); options.address=toUtf8(address);
    options.port=static_cast<std::uint16_t>(std::stoul(port));
    g_session=frostbridge::connect(std::move(options));
    g_host=host; g_connected=false; g_startPending=false; controls(true);
    status(host?L"Ожидание второго игрока…":L"Подключение…");
    log(L"Игрок: "+name+L" | Frostpunk PID: "+std::to_wstring(g_pid));
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
        control(L"STATIC",g_steam?L"STEAM P2P":L"ЛОКАЛЬНАЯ СЕТЬ — прямое соединение",0,20,16,650,28);
        control(L"STATIC",L"Имя игрока",0,20,58,145,24);
        control(L"EDIT",g_initialName.c_str(),kName,170,54,490,30,WS_TABSTOP|ES_AUTOHSCROLL);
        control(L"STATIC",g_steam?L"SteamID64 друга":L"Адрес хоста",0,20,100,145,24);
        control(L"EDIT",g_steam?L"":g_initialAddress.c_str(),kAddress,170,96,320,30,WS_TABSTOP|ES_AUTOHSCROLL);
        control(L"STATIC",L"Порт",0,504,100,45,24);
        control(L"EDIT",g_initialPort.c_str(),kPort,553,96,107,30,WS_TABSTOP|ES_NUMBER);
        control(L"STATIC",g_steam?L"Оба игрока вводят SteamID64 друг друга. AppID 480.":L"На одном ПК: 127.0.0.1. На другом ПК: локальный IPv4 хоста.",0,20,140,650,24);
        control(L"BUTTON",L"Создать",kHost,20,177,180,34,WS_TABSTOP);
        control(L"BUTTON",L"Подключиться",kJoin,217,177,220,34,WS_TABSTOP);
        control(L"BUTTON",L"Отключиться",kStop,454,177,206,34,WS_TABSTOP);
        control(L"STATIC",L"Введите имя и выберите действие",kStatus,20,220,260,52);
        control(L"BUTTON",L"Бесконечный",kStart,290,225,180,38,WS_TABSTOP);
        control(L"BUTTON",L"Сюжетный сценарий",kStory,480,225,180,38,WS_TABSTOP);
        control(L"EDIT",L"survival",kSaveName,20,280,280,30,WS_TABSTOP|ES_AUTOHSCROLL);
        control(L"BUTTON",L"Сохранить всем",kSave,313,280,167,30,WS_TABSTOP);
        control(L"BUTTON",L"Загрузить всем",kLoad,493,280,167,30,WS_TABSTOP);
        control(L"STATIC",L"Имя + _multiplayer. Для продолжения используйте то же имя игрока.",0,20,314,650,24);
        control(L"EDIT",L"",kLog,20,342,640,183,WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL);
        control(L"EDIT",L"",kChat,20,539,480,30,WS_TABSTOP|ES_AUTOHSCROLL);
        control(L"BUTTON",L"Отправить",kSend,515,539,145,30,WS_TABSTOP);
        control(L"STATIC",L"Окно можно свернуть и продолжить игру. Закрытие отключает сессию.",0,20,582,655,25);
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
            case kHost: startSession(true); break;
            case kJoin: startSession(false); break;
            case kSave:
            case kLoad:
                if(g_session && g_host && g_connected)
                    g_session->send(std::string(LOWORD(wp)==kSave?"save ":"load ")+toUtf8(field(kSaveName)));
                break;
            case kStop: stopSession(); status(L"Отключено"); log(L"Сессия остановлена."); break;
            case kStory:
            case kStart: {
                if (!g_session || !g_host || !g_connected || g_startPending) break;
                g_session->send(LOWORD(wp) == kStory ? "start story" : "start");
                g_startPending=true; controls(true); status(L"Проверка готовности двух игр…");
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
        } catch(const std::exception& error) { status(L"Ошибка — проверьте параметры"); log(fromUtf8(error.what())); }
        return 0;
    case WM_TIMER:
        pumpOutput();
        if(g_session&&g_session->finished()) {
            const bool failed=g_session->failed();
            pumpOutput(); stopSession();
            status(failed?L"Подключение не удалось — подробности в журнале":L"Соединение закрыто. Можно подключиться заново.");
        }
        if(g_game&&WaitForSingleObject(g_game,0)==WAIT_OBJECT_0) {
            stopSession(); DestroyWindow(window);
        }
        return 0;
    case WM_CLOSE:
        if(g_session&&MessageBoxW(window,L"Отключить сетевую сессию и закрыть окно?",L"FrostBridge",MB_YESNO|MB_ICONQUESTION)!=IDYES) return 0;
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
            if(args[i]==L"--steam") g_steam=true;
            else if(args[i]==L"--lan") g_steam=false;
            else if(args[i]==L"--name"&&i+1<args.size()) g_initialName=args[++i];
            else if(args[i]==L"--address"&&i+1<args.size()) g_initialAddress=args[++i];
            else if(args[i]==L"--port"&&i+1<args.size()) g_initialPort=args[++i];
            else if(args[i]==L"--auto-host") g_autoAction=1;
            else if(args[i]==L"--auto-join") g_autoAction=2;
            else if(args[i]==L"--pid"&&i+1<args.size()) {
                const auto value=args[++i];
                if(value.empty()||value.find_first_not_of(L"0123456789")!=std::wstring::npos) throw std::runtime_error("Некорректный PID.");
                auto pid=std::stoull(value);
                if(!pid||pid>MAXDWORD) throw std::runtime_error("Некорректный PID.");
                g_pid=static_cast<DWORD>(pid);
            } else throw std::runtime_error("Параметры: [--lan | --steam] [--pid PID] [--name NAME] [--address IP] [--port PORT] [--auto-host | --auto-join]");
        }
        if(g_autoAction&&g_initialName.empty()) throw std::runtime_error("Для автоматического подключения укажите --name.");
        const auto title=std::wstring(L"FrostBridge — ")+(g_steam?L"Steam":L"LAN")+L" — PID "+std::to_wstring(g_pid);
        // One UI/bridge per city, including when called again from the menu.
        const auto mutexName=L"Local\\FrostBridgeUI-"+std::to_wstring(g_pid);
        HANDLE mutex=CreateMutexW(nullptr,FALSE,mutexName.c_str());
        if(!mutex) throw std::runtime_error("Не удалось создать блокировку сессии.");
        if(GetLastError()==ERROR_ALREADY_EXISTS) {
            // Transport may differ; find the existing form for the same city.
            for(const auto* transport:{L"Steam",L"LAN"}) {
                const auto other=L"FrostBridge — "+std::wstring(transport)+L" — PID "+std::to_wstring(g_pid);
                if(HWND window=FindWindowW(kClass,other.c_str())) { ShowWindow(window,SW_RESTORE); SetForegroundWindow(window); }
            }
            CloseHandle(mutex); return 0;
        }
        if(g_pid) {
            g_game=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,g_pid);
            wchar_t path[32768]{}; DWORD size=32768;
            if(!g_game||!QueryFullProcessImageNameW(g_game,0,path,&size)||
                _wcsicmp(std::filesystem::path(path).filename().c_str(),L"Frostpunk.exe")!=0)
                throw std::runtime_error("Выбранный PID не является доступным процессом Frostpunk.");
        }
        WNDCLASSW cls{}; cls.lpfnWndProc=windowProc; cls.hInstance=instance;
        cls.lpszClassName=kClass; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        RegisterClassW(&cls);
        RECT rect{0,0,680,620}; AdjustWindowRect(&rect,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);
        HWND window=CreateWindowW(kClass,title.c_str(),WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
            CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,instance,nullptr);
        if(!window) throw std::runtime_error("Не удалось открыть окно подключения.");
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
