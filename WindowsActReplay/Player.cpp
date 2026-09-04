#include "Player.h"

namespace {
    double QpcFreqMs() {
        static double f = [] {
            LARGE_INTEGER x{};
            QueryPerformanceFrequency(&x);
            return static_cast<double>(x.QuadPart) / 1000.0;
        }();
        return f;
    }

    double NowMs() {
        LARGE_INTEGER n{};
        QueryPerformanceCounter(&n);
        return static_cast<double>(n.QuadPart) / QpcFreqMs();
    }

    // 虚拟屏幕物理像素坐标 -> SendInput 归一化绝对坐标 (0..65535)
    long ToAbsoluteNorm(long v, long origin, long size) {
        if (size <= 1) return 0;
        long long r = static_cast<long long>(v - origin) * 65535;
        r = (r + (size - 1) / 2) / (size - 1);
        if (r < 0) r = 0;
        if (r > 65535) r = 65535;
        return static_cast<long>(r);
    }

    // 查询进程令牌完整性级别（RID）。权限不足等失败返回 false。
    bool GetProcessIntegrityLevel(DWORD pid, DWORD& il) {
        il = 0;
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hp) return false;
        HANDLE tok = nullptr;
        BOOL ok = OpenProcessToken(hp, TOKEN_QUERY, &tok);
        CloseHandle(hp);
        if (!ok) return false;
        DWORD needed = 0;
        GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &needed);
        std::vector<BYTE> buf(needed);
        ok = needed && GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), needed, &needed);
        CloseHandle(tok);
        if (!ok) return false;
        auto* til = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
        DWORD count = *GetSidSubAuthorityCount(til->Label.Sid);
        il = *GetSidSubAuthority(til->Label.Sid, count - 1);
        return true;
    }

    const wchar_t* IntegrityLevelName(DWORD il) {
        if (il >= SECURITY_MANDATORY_SYSTEM_RID) return L"系统";
        if (il >= SECURITY_MANDATORY_HIGH_RID)   return L"高（管理员）";
        if (il >= SECURITY_MANDATORY_MEDIUM_RID) return L"中";
        return L"低";
    }

    bool SameRect(const RECT& a, const RECT& b, long tol) {
        return labs(a.left - b.left) <= tol && labs(a.top - b.top) <= tol &&
               labs(a.right - b.right) <= tol && labs(a.bottom - b.bottom) <= tol;
    }
}

Player::Player(HWND hwndNotify) : m_hwndNotify(hwndNotify) {}

Player::~Player() { Stop(); }

bool Player::Start(std::shared_ptr<const Recording> rec, std::wstring& err) {
    if (m_playing.load()) { err = L"正在回放中。"; return false; }
    Stop(); // 清理上一次运行的句柄

    if (!rec || !Validate(*rec, err)) return false;

    m_rec = std::move(rec);
    m_cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_playing = true;
    m_hThread = CreateThread(nullptr, 0, &Player::ThreadProc, this, 0, nullptr);
    if (!m_hThread) {
        m_playing = false;
        CloseHandle(m_cancelEvent);
        m_cancelEvent = nullptr;
        m_rec.reset();
        err = L"无法创建回放线程。";
        return false;
    }
    return true;
}

void Player::RequestStop() {
    if (m_cancelEvent) SetEvent(m_cancelEvent);
}

void Player::Stop() {
    RequestStop();
    if (m_hThread) {
        WaitForSingleObject(m_hThread, INFINITE);
        CloseHandle(m_hThread);
        m_hThread = nullptr;
    }
    if (m_cancelEvent) {
        CloseHandle(m_cancelEvent);
        m_cancelEvent = nullptr;
    }
    m_playing = false;
    m_rec.reset();
}

bool Player::Validate(const Recording& rec, std::wstring& err) {
    if (rec.events.empty()) {
        err = L"没有可回放的记录。";
        return false;
    }

    // 1) 虚拟屏幕（多显示器布局/分辨率）必须与录制时一致
    RECT vs{};
    vs.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vs.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vs.right = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (!SameRect(vs, rec.virtualScreen, 0)) {
        wchar_t b[512];
        swprintf_s(b,
            L"显示器布局或分辨率与录制时不一致，已安全终止。\n"
            L"录制时虚拟屏幕：(%ld, %ld) - (%ld, %ld)\n"
            L"当前虚拟屏幕：  (%ld, %ld) - (%ld, %ld)",
            rec.virtualScreen.left, rec.virtualScreen.top, rec.virtualScreen.right, rec.virtualScreen.bottom,
            vs.left, vs.top, vs.right, vs.bottom);
        err = b;
        return false;
    }
    UINT dpi = GetDpiForSystem();
    if (dpi != rec.systemDpi) {
        wchar_t b[256];
        swprintf_s(b, L"系统 DPI 与录制时不一致（录制时 %u，当前 %u），坐标已失真，已安全终止。",
                   rec.systemDpi, dpi);
        err = b;
        return false;
    }
    m_virtualScreen = vs;

    // 2) 逐个校验录制中出现过的顶层窗口
    DWORD myIL = 0;
    bool myILok = GetProcessIntegrityLevel(GetCurrentProcessId(), myIL);

    std::map<HWND, bool> checked;
    for (const auto& ev : rec.events) {
        HWND h = ev.wnd.hwnd;
        if (!h || checked.count(h)) continue;
        checked[h] = true;

        if (!IsWindow(h)) {
            err = L"目标窗口已不存在：\"" + ev.wnd.title + L"\" [" + ev.wnd.className + L"]\n"
                  L"已安全终止，未执行任何操作。";
            return false;
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != ev.wnd.pid) {
            err = L"窗口句柄已被其他进程复用，与录制时不一致：\"" + ev.wnd.title + L"\"\n"
                  L"已安全终止，未执行任何操作。";
            return false;
        }

        // 完整性级别 / UIPI：无法访问或操作比自己权限更高的进程
        DWORD il = 0;
        if (!GetProcessIntegrityLevel(pid, il)) {
            err = L"无法访问目标进程 \"" + ev.wnd.processName + L"\"（PID " + std::to_wstring(pid) + L"）。\n"
                  L"目标很可能以更高权限（如管理员）运行。受 Windows 完整性级别/UIPI 限制，\n"
                  L"请以与目标相同的权限级别重新运行本程序。已安全终止。";
            return false;
        }
        if (myILok && il > myIL) {
            err = L"目标进程 \"" + ev.wnd.processName + L"\" 的完整性级别（" + IntegrityLevelName(il) +
                  L"）高于本程序（" + IntegrityLevelName(myIL) + L"）。\n"
                  L"受 UIPI 限制无法向其注入输入。如需操作它，请以相同权限级别运行本程序。已安全终止。";
            return false;
        }

        // 目标窗口所在显示器 DPI 变化会导致坐标失真
        UINT wdpi = GetDpiForWindow(h);
        if (ev.wnd.dpi && wdpi && wdpi != ev.wnd.dpi) {
            err = L"目标窗口 \"" + ev.wnd.title + L"\" 所在显示器的 DPI 已变化"
                  L"（录制时 " + std::to_wstring(ev.wnd.dpi) + L"，当前 " + std::to_wstring(wdpi) +
                  L"），坐标已失真，已安全终止。";
            return false;
        }

        // 窗口位置/大小必须匹配；不匹配则尝试还原最小化窗口并移回原位
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        RECT now{};
        GetWindowRect(h, &now);
        if (!SameRect(now, ev.wnd.rect, 4)) {
            SetWindowPos(h, nullptr,
                         ev.wnd.rect.left, ev.wnd.rect.top,
                         ev.wnd.rect.right - ev.wnd.rect.left,
                         ev.wnd.rect.bottom - ev.wnd.rect.top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            GetWindowRect(h, &now);
            if (!SameRect(now, ev.wnd.rect, 4)) {
                err = L"无法将目标窗口恢复为录制时的位置/大小：\"" + ev.wnd.title + L"\"\n"
                      L"（窗口可能拒绝了移动。）已安全终止，未执行任何操作。";
                return false;
            }
        }
    }
    return true;
}

DWORD WINAPI Player::ThreadProc(LPVOID param) {
    static_cast<Player*>(param)->ThreadMain();
    return 0;
}

void Player::ThreadMain() {
    timeBeginPeriod(1);
    m_startMs = NowMs();

    PlayState state = PlayFinished;
    std::wstring msg = L"回放完成，共注入 " + std::to_wstring(m_rec->events.size()) + L" 个操作。";

    for (size_t i = 0; i < m_rec->events.size(); ++i) {
        const RecEvent& ev = m_rec->events[i];

        if (WaitForSingleObject(m_cancelEvent, 0) == WAIT_OBJECT_0) {
            state = PlayCancelled;
            msg = L"回放已被用户中止（已执行 " + std::to_wstring(i) + L"/" +
                  std::to_wstring(m_rec->events.size()) + L" 个操作）。";
            break;
        }
        if (!WaitUntil(ev.offsetMs)) {
            state = PlayCancelled;
            msg = L"回放已被用户中止。";
            break;
        }

        HWND h = ev.wnd.hwnd;
        if (h) {
            if (!IsWindow(h)) {
                state = PlayAborted;
                msg = L"目标窗口 \"" + ev.wnd.title + L"\" 在回放过程中被关闭，已安全终止。";
                break;
            }
            // 录制时是前台窗口 -> 回放前恢复前台，避免点击落到后台窗口不产生预期效果
            if (ev.wnd.foreground && !EnsureForeground(h)) {
                state = PlayAborted;
                msg = L"无法将目标窗口置为前台：\"" + ev.wnd.title + L"\"。\n"
                      L"为避免在错误的窗口上操作，已安全终止。";
                break;
            }
        }

        // 点击/滚轮类事件：校验屏幕落点窗口与录制时一致，杜绝“默默点在错误窗口”
        if (ev.type != RecEventType::Move) {
            HWND at = WindowFromPoint(ev.pt);
            if (at) at = GetAncestor(at, GA_ROOT);
            if (at != h) {
                wchar_t b[512];
                swprintf_s(b,
                    L"坐标 (%ld, %ld) 处当前的窗口与录制时不一致（目标 \"%s\" 可能被遮挡）。\n"
                    L"已安全终止，未执行此次点击。",
                    ev.pt.x, ev.pt.y, ev.wnd.title.c_str());
                state = PlayAborted;
                msg = b;
                break;
            }
        }

        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = ToAbsoluteNorm(ev.pt.x, m_virtualScreen.left, m_virtualScreen.right - m_virtualScreen.left);
        in.mi.dy = ToAbsoluteNorm(ev.pt.y, m_virtualScreen.top, m_virtualScreen.bottom - m_virtualScreen.top);
        in.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        switch (ev.type) {
        case RecEventType::Move:       in.mi.dwFlags |= MOUSEEVENTF_MOVE; break;
        case RecEventType::LeftDown:   in.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN; break;
        case RecEventType::LeftUp:     in.mi.dwFlags |= MOUSEEVENTF_LEFTUP; break;
        case RecEventType::RightDown:  in.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN; break;
        case RecEventType::RightUp:    in.mi.dwFlags |= MOUSEEVENTF_RIGHTUP; break;
        case RecEventType::MiddleDown: in.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN; break;
        case RecEventType::MiddleUp:   in.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP; break;
        case RecEventType::WheelV:     in.mi.dwFlags |= MOUSEEVENTF_WHEEL;  in.mi.mouseData = static_cast<DWORD>(ev.delta); break;
        case RecEventType::WheelH:     in.mi.dwFlags |= MOUSEEVENTF_HWHEEL; in.mi.mouseData = static_cast<DWORD>(ev.delta); break;
        case RecEventType::XDown:      in.mi.dwFlags |= MOUSEEVENTF_XDOWN;  in.mi.mouseData = static_cast<DWORD>(ev.delta); break;
        case RecEventType::XUp:        in.mi.dwFlags |= MOUSEEVENTF_XUP;    in.mi.mouseData = static_cast<DWORD>(ev.delta); break;
        }
        in.mi.dwExtraInfo = kInjectMagic;
        SetMessageExtraInfo(kInjectMagic); // 双保险：即便钩子还挂着也会被跳过
        SendInput(1, &in, sizeof(INPUT));
    }

    Finish(state, msg);
    timeEndPeriod(1);
}

bool Player::EnsureForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) return true;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);

    // 附加到当前前台线程的输入队列，以满足 SetForegroundWindow 的前置条件
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD tid = GetCurrentThreadId();
    bool attached = fgTid && fgTid != tid && AttachThreadInput(tid, fgTid, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (attached) AttachThreadInput(tid, fgTid, FALSE);

    Sleep(40);
    return GetForegroundWindow() == hwnd;
}

bool Player::WaitUntil(double offsetMs) {
    for (;;) {
        double remain = offsetMs - (NowMs() - m_startMs);
        if (remain <= 0) return true;
        if (remain > 30) {
            // 粗等：可被取消事件立即打断
            if (WaitForSingleObject(m_cancelEvent, static_cast<DWORD>(remain - 20.0)) == WAIT_OBJECT_0)
                return false;
        } else if (remain > 2) {
            if (WaitForSingleObject(m_cancelEvent, 1) == WAIT_OBJECT_0)
                return false;
        }
        // 剩余 <2ms：自旋，保证时序精度
    }
}

void Player::Finish(PlayState state, const std::wstring& msg) {
    m_playing = false;
    if (m_hwndNotify && IsWindow(m_hwndNotify))
        PostMessageW(m_hwndNotify, WM_APP_PLAY_STATE, static_cast<WPARAM>(state), reinterpret_cast<LPARAM>(new std::wstring(msg)));
}
