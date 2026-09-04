#pragma once

#include "WindowsActReplay.h"

// 回放器：回放前做环境一致性预检（窗口存在性/进程身份/完整性级别/DPI/窗口矩形/
// 虚拟屏幕），不通过则明确报错并拒绝回放；回放线程按 QPC 时序用 SendInput
// 以虚拟屏幕绝对坐标注入鼠标输入、以扫描码注入键盘输入；
// 点击/滚轮前校验落点窗口，键盘事件前校验焦点窗口与录制时一致。
class Player {
public:
    explicit Player(HWND hwndNotify);
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // 先执行预检（可能还原最小化窗口、把窗口移回录制时的位置），通过后启动回放线程
    bool Start(std::shared_ptr<const Recording> rec, std::wstring& err);
    void RequestStop();  // 仅置取消标志（线程安全、立即返回）
    void Stop();         // RequestStop + 等待回放线程退出
    bool IsPlaying() const { return m_playing.load(); }

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void ThreadMain();

    bool Validate(const Recording& rec, std::wstring& err); // 预检（UI 线程）
    static bool EnsureForeground(HWND hwnd);                // 尽力置前台并验证
    bool WaitUntil(double offsetMs);                        // 精确等待到相对时刻；false=被取消
    void Finish(PlayState state, const std::wstring& msg);  // 通知 UI 并结束线程

    HWND m_hwndNotify = nullptr;
    std::atomic<bool> m_playing{ false };
    HANDLE m_hThread = nullptr;
    HANDLE m_cancelEvent = nullptr;
    std::shared_ptr<const Recording> m_rec;
    double m_startMs = 0.0;
    RECT m_virtualScreen{};
};
