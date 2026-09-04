#include "Recorder.h"

namespace {
    // LL 钩子回调只会在安装钩子的线程上执行，用全局指针路由即可。
    std::atomic<Recorder*> g_activeRecorder{ nullptr };
    HHOOK g_hook = nullptr;
    double g_qpcFreqMs = 0.0; // QPC 每毫秒计数
}

Recorder::Recorder(HWND hwndNotify) : m_hwndNotify(hwndNotify) {}

Recorder::~Recorder() { Stop(); }

bool Recorder::Start(std::wstring& err) {
    if (m_running.load()) return true;
    Stop(); // 防御：清理可能残留的旧线程句柄

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_rec = std::make_shared<Recording>();
    }
    // 记录起始显示环境（进程为 PerMonitorV2，坐标均为物理像素）
    m_rec->virtualScreen.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    m_rec->virtualScreen.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    m_rec->virtualScreen.right = m_rec->virtualScreen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_rec->virtualScreen.bottom = m_rec->virtualScreen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    m_rec->systemDpi = GetDpiForSystem();

    if (g_qpcFreqMs == 0.0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        g_qpcFreqMs = static_cast<double>(f.QuadPart) / 1000.0;
    }

    m_hasLast = false;
    m_hasLastSnap = false;
    m_notifyPending = false;

    m_readyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_hThread = CreateThread(nullptr, 0, &Recorder::ThreadProc, this, 0, nullptr);
    if (!m_hThread) {
        err = L"无法创建录制线程。";
        CloseHandle(m_readyEvent);
        m_readyEvent = nullptr;
        return false;
    }
    WaitForSingleObject(m_readyEvent, 5000);
    CloseHandle(m_readyEvent);
    m_readyEvent = nullptr;

    if (!m_running.load()) {
        WaitForSingleObject(m_hThread, INFINITE);
        CloseHandle(m_hThread);
        m_hThread = nullptr;
        err = L"安装全局鼠标钩子 (WH_MOUSE_LL) 失败。\n"
              L"可能是安全软件拦截了全局钩子，请检查后重试。";
        return false;
    }
    return true;
}

void Recorder::Stop() {
    if (!m_hThread) return;
    DWORD tid = m_threadId.load();
    if (tid) PostThreadMessageW(tid, WM_QUIT, 0, 0);
    WaitForSingleObject(m_hThread, INFINITE);
    CloseHandle(m_hThread);
    m_hThread = nullptr;
    m_running = false;
    m_hasLast = false;
    // 让 UI 做最后一次拉取
    if (m_hwndNotify && IsWindow(m_hwndNotify))
        PostMessageW(m_hwndNotify, WM_APP_REC_EVENTS, 0, 0);
}

DWORD WINAPI Recorder::ThreadProc(LPVOID param) {
    static_cast<Recorder*>(param)->ThreadMain();
    return 0;
}

void Recorder::ThreadMain() {
    m_threadId = GetCurrentThreadId();
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, &Recorder::HookProc, GetModuleHandleW(nullptr), 0);
    if (g_hook) {
        g_activeRecorder = this;
        QueryPerformanceCounter(&m_qpcStart);
        m_running = true;
    }
    SetEvent(m_readyEvent); // 无论成败都唤醒 Start
    if (!g_hook) return;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_activeRecorder = nullptr;
    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
}

LRESULT CALLBACK Recorder::HookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    Recorder* self = g_activeRecorder.load();
    if (nCode == HC_ACTION && self) self->OnHook(wParam, lParam);
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

void Recorder::OnHook(WPARAM wParam, LPARAM lParam) {
    auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

    // 跳过一切注入事件（含低完整性级别注入）：绝不把回放/其他工具注入的输入录入。
    if (m->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) return;
    if (m->dwExtraInfo == kInjectMagic || GetMessageExtraInfo() == kInjectMagic) return;

    RecEventType type;
    int delta = 0;
    switch (wParam) {
    case WM_MOUSEMOVE:    type = RecEventType::Move; break;
    case WM_LBUTTONDOWN:  type = RecEventType::LeftDown; break;
    case WM_LBUTTONUP:    type = RecEventType::LeftUp; break;
    case WM_RBUTTONDOWN:  type = RecEventType::RightDown; break;
    case WM_RBUTTONUP:    type = RecEventType::RightUp; break;
    case WM_MBUTTONDOWN:  type = RecEventType::MiddleDown; break;
    case WM_MBUTTONUP:    type = RecEventType::MiddleUp; break;
    case WM_MOUSEWHEEL:   type = RecEventType::WheelV; delta = static_cast<short>(HIWORD(m->mouseData)); break;
    case WM_MOUSEHWHEEL:  type = RecEventType::WheelH; delta = static_cast<short>(HIWORD(m->mouseData)); break;
    case WM_XBUTTONDOWN:  type = RecEventType::XDown; delta = HIWORD(m->mouseData); break;
    case WM_XBUTTONUP:    type = RecEventType::XUp;   delta = HIWORD(m->mouseData); break;
    default: return;
    }

    // 合并完全重复的移动点（同坐标连续 Move 没有信息量）
    if (type == RecEventType::Move && m_hasLast &&
        m_lastType == RecEventType::Move &&
        m_lastPt.x == m->pt.x && m_lastPt.y == m->pt.y)
        return;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    RecEvent ev;
    ev.type = type;
    ev.pt = m->pt;
    ev.delta = delta;
    ev.offsetMs = static_cast<double>(now.QuadPart - m_qpcStart.QuadPart) / g_qpcFreqMs;
    ev.wnd = CaptureContext(m->pt);

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_rec) m_rec->events.push_back(std::move(ev));
    }
    m_lastType = type;
    m_lastPt = m->pt;
    m_hasLast = true;

    // 若已有通知在队列里就不再重复 PostMessage，避免高频移动事件淹没 UI 消息队列
    if (!m_notifyPending.exchange(true) && m_hwndNotify)
        PostMessageW(m_hwndNotify, WM_APP_REC_EVENTS, 0, 0);
}

WindowSnapshot Recorder::CaptureContext(POINT pt) {
    HWND h = WindowFromPoint(pt);
    if (h) h = GetAncestor(h, GA_ROOT);
    if (!h) return {};

    WindowSnapshot s;
    if (m_hasLastSnap && m_lastSnap.hwnd == h) {
        // 同一顶层窗口：复用进程/类名/标题等需要跨进程获取的字段，降低钩子开销
        s = m_lastSnap;
    }
    s.hwnd = h;
    if (!s.pid) {
        GetWindowThreadProcessId(h, &s.pid);
        wchar_t cls[128]{};
        if (GetClassNameW(h, cls, _countof(cls))) s.className = cls;
        int len = GetWindowTextLengthW(h);
        if (len > 0) {
            std::vector<wchar_t> buf(static_cast<size_t>(len) + 1);
            GetWindowTextW(h, buf.data(), len + 1);
            s.title = buf.data();
        }
        s.processName = ProcessNameForPid(s.pid);
    }
    // 以下字段廉价（不发送窗口消息），每个事件都刷新
    GetWindowRect(h, &s.rect);
    s.foreground = (GetForegroundWindow() == h);
    s.dpi = GetDpiForWindow(h);

    m_lastSnap = s;
    m_hasLastSnap = true;
    return s;
}

const std::wstring& Recorder::ProcessNameForPid(DWORD pid) {
    auto it = m_procNameCache.find(pid);
    if (it != m_procNameCache.end()) return it->second;

    std::wstring name;
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hp) {
        wchar_t path[512];
        DWORD sz = _countof(path);
        if (QueryFullProcessImageNameW(hp, 0, path, &sz)) {
            const wchar_t* base = wcsrchr(path, L'\\');
            name = base ? base + 1 : path;
        }
        CloseHandle(hp);
    }
    if (name.empty()) name = L"<无访问权限>";
    return m_procNameCache.emplace(pid, std::move(name)).first->second;
}

std::shared_ptr<Recording> Recorder::GetRecording() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_rec;
}

size_t Recorder::CopyEventsFrom(size_t fromIndex, std::vector<RecEvent>& out) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_rec && fromIndex < m_rec->events.size())
        out.insert(out.end(),
                   m_rec->events.begin() + static_cast<std::ptrdiff_t>(fromIndex),
                   m_rec->events.end());
    return m_rec ? m_rec->events.size() : 0;
}
