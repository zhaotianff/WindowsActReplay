#include "Common.h"
#include "WindowsActReplay.h"
#include "Recorder.h"
#include "Player.h"
#include "resource.h"

namespace {
    constexpr int IDC_BTN_RECORD = 1001;
    constexpr int IDC_BTN_PLAY   = 1002;
    constexpr int IDC_LIST       = 1003;
    constexpr int IDC_STATUS     = 1004;

    constexpr int kLeftColWidth = 150;  // 左侧按钮栏宽度
    constexpr int kMargin = 8;
    constexpr int kStatusHeight = 24;

    HINSTANCE g_hInst = nullptr;
    std::unique_ptr<Recorder> g_recorder;
    std::unique_ptr<Player>   g_player;
    size_t g_shownCount = 0;            // 已显示到列表的事件数
    HFONT g_font = nullptr;

    HFONT CreateUiFont(UINT dpi) {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);
        lf.lfWeight = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
        return CreateFontIndirectW(&lf);
    }

    void SetStatus(HWND hwnd, const std::wstring& text) {
        SetDlgItemTextW(hwnd, IDC_STATUS, text.c_str());
    }

    std::wstring Truncate(const std::wstring& s, size_t n) {
        return s.size() <= n ? s : s.substr(0, n) + L"…";
    }

    std::wstring FormatEvent(const RecEvent& e) {
        std::wstring action;
        switch (e.type) {
        case RecEventType::Move:       action = L"移动"; break;
        case RecEventType::LeftDown:   action = L"左键按下"; break;
        case RecEventType::LeftUp:     action = L"左键抬起"; break;
        case RecEventType::RightDown:  action = L"右键按下"; break;
        case RecEventType::RightUp:    action = L"右键抬起"; break;
        case RecEventType::MiddleDown: action = L"中键按下"; break;
        case RecEventType::MiddleUp:   action = L"中键抬起"; break;
        case RecEventType::WheelV:     action = L"滚轮(纵)"; break;
        case RecEventType::WheelH:     action = L"滚轮(横)"; break;
        case RecEventType::XDown:      action = L"侧键" + std::to_wstring(e.delta) + L"按下"; break;
        case RecEventType::XUp:        action = L"侧键" + std::to_wstring(e.delta) + L"抬起"; break;
        }
        wchar_t buf[256];
        swprintf_s(buf, L"[+%.3fs] %s (%ld, %ld)", e.offsetMs / 1000.0, action.c_str(), e.pt.x, e.pt.y);
        std::wstring line = buf;

        if (e.type == RecEventType::WheelV || e.type == RecEventType::WheelH)
            line += L"  Δ=" + std::to_wstring(e.delta);

        // 移动事件太密集，仅点击/滚轮类事件附带窗口上下文
        if (e.type != RecEventType::Move && e.wnd.hwnd) {
            line += L"  |  \"" + Truncate(e.wnd.title, 24) + L"\" [" + e.wnd.className + L"] " + e.wnd.processName;
            if (e.wnd.foreground) line += L"（前台）";
        }
        return line;
    }

    // 拉取录制事件并追加到列表（与 Recorder 的通知标志配合，先清标志再拉取，避免丢通知）
    void DrainRecordedEvents(HWND hwnd) {
        if (!g_recorder) return;
        g_recorder->ClearNotifyPending();
        std::vector<RecEvent> evs;
        size_t total = g_recorder->CopyEventsFrom(g_shownCount, evs);
        if (evs.empty()) return;

        HWND hList = GetDlgItem(hwnd, IDC_LIST);
        SendMessageW(hList, WM_SETREDRAW, FALSE, 0);
        for (const auto& e : evs) {
            std::wstring line = FormatEvent(e);
            SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        }
        SendMessageW(hList, LB_SETHORIZONTALEXTENT, 2000, 0);
        int count = static_cast<int>(SendMessageW(hList, LB_GETCOUNT, 0, 0));
        if (count > 0) SendMessageW(hList, LB_SETTOPINDEX, count - 1, 0); // 自动滚动到底部
        SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hList, nullptr, FALSE);

        g_shownCount = total;
        if (g_recorder->IsRecording())
            SetStatus(hwnd, L"正在记录… 已捕获 " + std::to_wstring(total) + L" 个操作");
    }

    void OnRecordClicked(HWND hwnd) {
        if (!g_recorder) return;
        if (!g_recorder->IsRecording()) {
            SendDlgItemMessageW(hwnd, IDC_LIST, LB_RESETCONTENT, 0, 0);
            g_shownCount = 0;
            std::wstring err;
            if (!g_recorder->Start(err)) {
                MessageBoxW(hwnd, err.c_str(), L"无法开始记录", MB_ICONERROR);
                SetStatus(hwnd, err);
                return;
            }
            SetDlgItemTextW(hwnd, IDC_BTN_RECORD, L"停止");
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_PLAY), FALSE);
            SetStatus(hwnd, L"正在记录全局鼠标操作…（完成后点击“停止”）");
        } else {
            g_recorder->Stop();
            DrainRecordedEvents(hwnd); // 兜底：把剩余事件全部显示出来
            SetDlgItemTextW(hwnd, IDC_BTN_RECORD, L"记录");
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_PLAY), TRUE);
            SetStatus(hwnd, L"记录结束，共 " + std::to_wstring(g_shownCount) +
                            L" 个操作。可点击“回放”按原始时序重现。");
        }
    }

    void OnPlayClicked(HWND hwnd) {
        if (!g_player || !g_recorder) return;
        if (!g_player->IsPlaying()) {
            auto rec = g_recorder->GetRecording();
            if (!rec || rec->events.empty()) {
                MessageBoxW(hwnd, L"还没有可回放的记录。\n请先点击“记录”并执行一些鼠标操作。",
                            L"无法回放", MB_ICONINFORMATION);
                return;
            }
            std::wstring err;
            if (!g_player->Start(rec, err)) {
                // 预检失败：窗口不存在 / 权限不足 / 显示器或 DPI 变化等
                MessageBoxW(hwnd, err.c_str(), L"回放前检查未通过", MB_ICONWARNING);
                SetStatus(hwnd, L"回放未开始：环境一致性检查未通过。");
                return;
            }
            SetDlgItemTextW(hwnd, IDC_BTN_PLAY, L"停止回放");
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_RECORD), FALSE);
            SetStatus(hwnd, L"正在回放… 请勿移动鼠标或遮挡目标窗口；点击“停止回放”可中止。");
        } else {
            g_player->RequestStop();
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_PLAY), FALSE);
            SetStatus(hwnd, L"正在停止回放…");
        }
    }

    void LayoutControls(HWND hwnd) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        const int btnH = 34;
        const int listX = kMargin + kLeftColWidth + kMargin;
        SetWindowPos(GetDlgItem(hwnd, IDC_BTN_RECORD), nullptr,
                     kMargin, kMargin, kLeftColWidth, btnH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hwnd, IDC_BTN_PLAY), nullptr,
                     kMargin, kMargin + btnH + kMargin, kLeftColWidth, btnH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hwnd, IDC_LIST), nullptr,
                     listX, kMargin, w - listX - kMargin, h - kMargin * 3 - kStatusHeight, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hwnd, IDC_STATUS), nullptr,
                     kMargin, h - kMargin - kStatusHeight, w - kMargin * 2, kStatusHeight, SWP_NOZORDER);
    }

    LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CREATE: {
            UINT dpi = GetDpiForWindow(hwnd);
            g_font = CreateUiFont(dpi);
            DWORD btnStyle = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP;
            CreateWindowExW(0, L"BUTTON", L"记录", btnStyle,
                            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_RECORD)), g_hInst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"回放", btnStyle,
                            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_PLAY)), g_hInst, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), g_hInst, nullptr);
            CreateWindowExW(0, L"STATIC", L"就绪。点击“记录”开始捕获全局鼠标操作。",
                            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), g_hInst, nullptr);
            for (int id : { IDC_BTN_RECORD, IDC_BTN_PLAY, IDC_LIST, IDC_STATUS })
                SendDlgItemMessageW(hwnd, id, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
            g_recorder = std::make_unique<Recorder>(hwnd);
            g_player = std::make_unique<Player>(hwnd);
            LayoutControls(hwnd);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IDC_BTN_RECORD: OnRecordClicked(hwnd); return 0;
            case IDC_BTN_PLAY:   OnPlayClicked(hwnd); return 0;
            }
            break;

        case WM_APP_REC_EVENTS:
            DrainRecordedEvents(hwnd);
            return 0;

        case WM_APP_PLAY_STATE: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            SetDlgItemTextW(hwnd, IDC_BTN_PLAY, L"回放");
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_RECORD), TRUE);
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_PLAY), TRUE);
            if (text) {
                SetStatus(hwnd, *text);
                if (wParam == PlayAborted)
                    MessageBoxW(hwnd, text->c_str(), L"回放已安全终止", MB_ICONWARNING);
                delete text;
            }
            return 0;
        }

        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) LayoutControls(hwnd);
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 560;
            mmi->ptMinTrackSize.y = 360;
            return 0;
        }

        case WM_DPICHANGED: {
            auto* prc = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, prc->left, prc->top,
                         prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_font) DeleteObject(g_font);
            g_font = CreateUiFont(HIWORD(wParam));
            for (int id : { IDC_BTN_RECORD, IDC_BTN_PLAY, IDC_LIST, IDC_STATUS })
                SendDlgItemMessageW(hwnd, id, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
            return 0;
        }

        case WM_CLOSE:
            // 先安全停止工作线程（卸钩、取消回放并汇合），再销毁窗口
            if (g_recorder) g_recorder->Stop();
            if (g_player) g_player->Stop();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_recorder.reset();
            g_player.reset();
            if (g_font) { DeleteObject(g_font); g_font = nullptr; }
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    g_hInst = hInstance;

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
    wc.lpszClassName = L"WindowsActReplayMainWnd";
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"鼠标操作记录回放 - WindowsActReplay",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 980, 600,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}
