// Exercise the real window procedure without injecting into/altering a game.
#include "../src/FrostMenuMod/FrostMenuMod.cpp"
#include <iostream>
#include <stdexcept>
#include <fstream>

void savePreview(HWND window) {
    SetWindowPos(window,nullptr,-3000,-3000,760,604,SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(window);
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen,760,604);
    HGDIOBJ old = SelectObject(dc,bitmap);
    SendMessageW(window,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND | PRF_NONCLIENT);
    for (HWND edit : g_amountEdits) {
        RECT r{}; GetWindowRect(edit,&r);
        POINT origin{r.left,r.top}; ScreenToClient(window,&origin);
        const int saved = SaveDC(dc);
        SetViewportOrgEx(dc,origin.x,origin.y,nullptr);
        SendMessageW(edit,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
        RestoreDC(dc,saved);
    }
    SelectObject(dc,old);
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=760; info.bmiHeader.biHeight=-604;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    std::vector<unsigned char> pixels(760*604*4);
    GetDIBits(dc,bitmap,0,604,pixels.data(),&info,DIB_RGB_COLORS);
    BITMAPFILEHEADER file{}; file.bfType=0x4D42;
    file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
    file.bfSize=file.bfOffBits+static_cast<DWORD>(pixels.size());
    std::ofstream out("artifacts/overlay-controls/preview.bmp",std::ios::binary);
    out.write(reinterpret_cast<char*>(&file),sizeof(file));
    out.write(reinterpret_cast<char*>(&info.bmiHeader),sizeof(BITMAPINFOHEADER));
    out.write(reinterpret_cast<char*>(pixels.data()),pixels.size());
    DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr,screen);
    ShowWindow(window,SW_HIDE);
}

void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        g_module = GetModuleHandleW(nullptr);
        INITCOMMONCONTROLSEX init{sizeof(init), ICC_BAR_CLASSES};
        InitCommonControlsEx(&init);
        frostoverlay::Control control{};
        control.signature = frostoverlay::magic; control.layoutVersion = frostoverlay::version;
        control.connection = 1; g_overlayControl = &control;
        WNDCLASSW cls{}; cls.lpfnWndProc = overlayWindowProc;
        cls.hInstance = g_module; cls.lpszClassName = L"FrostOverlayTest";
        RegisterClassW(&cls);
        HWND window = CreateWindowExW(0,cls.lpszClassName,L"Overlay test",WS_POPUP,
            0,0,760,604,nullptr,nullptr,g_module,nullptr);
        check(window != nullptr, "Test window not created");
        frostoverlay::Values local{{50,30,20,3,80,0}};
        frostoverlay::writeSnapshot(control.local, local);
        refreshAmountControls(local);
        check(canSend(3,local), "Single core should be sendable");
        check(!canSend(5,local), "No rations must be disabled");
        SetWindowTextW(g_amountEdits[0], L"17");
        check(selectedAmount(0) == 17 && canSend(0,local), "Manual amount failed");
        local.item[0] = 0;
        check(!canSend(0,local), "No local coal must be disabled");
        local.item[0] = 50; control.peer.values.item[0] = 0;
        check(canSend(0,local), "Returned local resources must re-enable even when peer has none");
        SendMessageW(g_amountSliders[0], TBM_SETPOS, TRUE, 23);
        SendMessageW(window, WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(g_amountSliders[0]));
        check(selectedAmount(0) == 23, "Slider did not update edit");
        SetWindowTextW(g_amountEdits[0], L"1000001");
        check(!canSend(0,local), "Unbounded amount accepted");
        SetWindowTextW(g_amountEdits[0], L"-1");
        check(!canSend(0,local), "Negative amount accepted");
        SetWindowTextW(g_amountEdits[0], L"51");
        check(!canSend(0,local), "More than local stock accepted");
        SetWindowTextW(g_amountEdits[0], L"17");
        control.transferBusy = 1;
        check(!canSend(0,local), "Busy transfer accepted");
        control.transferBusy = 0;
        g_plusButtons[0] = RECT{231,232,261,257};
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(240,240));
        check(control.outgoingSequence == 1 && control.outgoingAmount == 17,
            "Click did not publish selected amount");
        strcpy_s(control.localName,"Fitchers"); strcpy_s(control.peerName,"User");
        control.peer.values = frostoverlay::Values{{0,30,20,3,80,0}};
        control.localHope=4000; control.peerHope=4000;
        control.localDiscontent=81; control.peerDiscontent=1785;
        control.localState=3; control.peerState=2; control.skewSeconds=-2;
        wcscpy_s(control.history[0],L"You sent User 20 wood.");
        wcscpy_s(control.history[1],L"Received 10 coal from User.");
        selectOverlayResource(window,1);
        check((GetWindowLongW(g_amountSliders[1],GWL_STYLE)&WS_VISIBLE)!=0,"Selected slider hidden");
        check((GetWindowLongW(g_amountSliders[0],GWL_STYLE)&WS_VISIBLE)==0,"Other slider visible");
        savePreview(window);
        SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(690,447));
        check(selectedAmount(1)==30,"All must use local stock");
        SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(610,447));
        check(selectedAmount(1)==50 && !canSend(1,local),"Quick 50 must validate stock");
        // Closing must be immediate even if the maintenance worker is busy:
        // do not pump timers or send the worker's old WM_TIMER fallback here.
        g_overlayWindow = window;
        g_overlayExpanded.store(true);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        check(IsWindowVisible(window) != FALSE, "Toggle test panel not visible");
        overlayButtonWindowProc(window, WM_LBUTTONUP, 0, 0);
        check(!g_overlayExpanded.load() && !IsWindowVisible(window),
            "Collapse waited for maintenance instead of hiding immediately");
        g_overlayWindow = nullptr;
        DestroyWindow(window);
        std::cout << "PASS: actual edit/slider synchronization, sender-based eligibility, stock returns, bounds, busy and selected transfer.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
