#pragma once

#include "Common.h"

// 注入事件标记：回放线程在 SendInput 前 SetMessageExtraInfo()，
// 钩子回调据此（以及 LLMHF_INJECTED 标志）跳过注入事件，避免把回放重新录入。
constexpr ULONG_PTR kInjectMagic = 0x57415249; // "WARI"

enum class RecEventType {
    Move,
    LeftDown, LeftUp,
    RightDown, RightUp,
    MiddleDown, MiddleUp,
    WheelV, WheelH,      // 纵向 / 横向滚轮
    XDown, XUp,          // X1/X2 侧键
    KeyDown, KeyUp,      // 键盘按下 / 抬起
};

// 录制时抓取的目标窗口上下文快照（用于回放前的环境一致性校验）
struct WindowSnapshot {
    HWND hwnd = nullptr;       // 光标所在的顶层窗口 (GetAncestor GA_ROOT)
    DWORD pid = 0;
    std::wstring processName;  // 进程映像文件名（如 notepad.exe）；无权限时为占位文本
    std::wstring title;
    std::wstring className;
    RECT rect{};
    bool foreground = false;   // 录制瞬间该窗口是否为前台窗口
    UINT dpi = 0;              // 录制时窗口所在显示器的 DPI
};

struct RecEvent {
    RecEventType type = RecEventType::Move;
    POINT pt{};                // 虚拟屏幕绝对坐标（物理像素；多显示器时可能为负）
    int delta = 0;             // 滚轮增量（±120 的倍数）或 X 键编号（1/2）
    DWORD scanCode = 0;        // 键盘扫描码（仅 KeyDown/KeyUp）
    DWORD virtualKey = 0;      // 虚拟键码（展示用）
    bool keyExtended = false;  // 扩展键标志（LLKHF_EXTENDED，如右侧 Ctrl/方向键）
    double offsetMs = 0.0;     // 相对录制开始的时间偏移（毫秒，基于 QPC）
    WindowSnapshot wnd;
};

// 一次完整录制：事件序列 + 录制时的显示环境
struct Recording {
    std::vector<RecEvent> events;
    RECT virtualScreen{};      // 录制开始时的虚拟屏幕范围
    UINT systemDpi = 0;        // 录制开始时的系统 DPI
};

// 自定义窗口消息
#define WM_APP_REC_EVENTS  (WM_APP + 1)  // 有新录制事件可供 UI 拉取
#define WM_APP_PLAY_STATE  (WM_APP + 2)  // wParam=PlayState, lParam=std::wstring*（接收方 delete）

enum PlayState {
    PlayFinished  = 0,   // 正常回放完成
    PlayCancelled = 1,   // 用户中止
    PlayAborted   = 2,   // 环境/校验异常，已安全终止
};
