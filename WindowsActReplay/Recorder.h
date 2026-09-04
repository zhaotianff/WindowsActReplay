#pragma once

#include "WindowsActReplay.h"

// 全局输入录制器：在专属录制线程上安装 WH_MOUSE_LL + WH_KEYBOARD_LL 低级钩子
//（LL 钩子的回调在“安装线程”的消息循环里执行，因此该线程必须持续泵消息）。
// 事件带相对时间与目标窗口上下文；UI 收到 WM_APP_REC_EVENTS 后增量拉取。
class Recorder {
public:
    explicit Recorder(HWND hwndNotify);
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    bool Start(std::wstring& err);  // 启动线程、安装钩子（同步等待安装结果）
    void Stop();                    // 通知线程退出、卸钩并汇合；可安全重复调用
    bool IsRecording() const { return m_running.load(); }

    std::shared_ptr<Recording> GetRecording() const;
    // 线程安全地拷贝 [fromIndex, 末尾) 的事件，返回最新事件总数
    size_t CopyEventsFrom(size_t fromIndex, std::vector<RecEvent>& out) const;
    void ClearNotifyPending() { m_notifyPending = false; }

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void ThreadMain();
    static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam);
    void OnMouse(WPARAM wParam, LPARAM lParam);
    void OnKeyboard(WPARAM wParam, LPARAM lParam);
    WindowSnapshot CaptureContext(POINT pt);      // 鼠标：光标所在顶层窗口
    WindowSnapshot CaptureFocusContext();         // 键盘：当前前台（焦点）顶层窗口
    const std::wstring& ProcessNameForPid(DWORD pid);

    HWND m_hwndNotify = nullptr;
    std::atomic<bool> m_running{ false };
    std::atomic<DWORD> m_threadId{ 0 };
    HANDLE m_hThread = nullptr;
    HANDLE m_readyEvent = nullptr;   // 钩子安装完成（无论成败）信号

    std::shared_ptr<Recording> m_rec;
    mutable std::mutex m_mutex;      // 保护 m_rec 指针与 m_rec->events
    std::atomic<bool> m_notifyPending{ false };

    // 以下成员仅由钩子线程访问
    LARGE_INTEGER m_qpcStart{};
    RecEventType m_lastType = RecEventType::Move;
    POINT m_lastPt{};
    bool m_hasLast = false;
    bool m_hasLastSnap = false;
    WindowSnapshot m_lastSnap{};
    std::map<DWORD, std::wstring> m_procNameCache;
};
