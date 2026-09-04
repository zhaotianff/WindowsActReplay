#pragma once

// 公共头：目标平台、Windows 头与 STL。
// 仅依赖 Windows SDK / STL，无第三方库。

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00            // Windows 10
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000002       // Win10 1607（GetDpiForWindow / PerMonitorV2）
#endif

#include <windows.h>
#include <commctrl.h>
#include <timeapi.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")      // timeBeginPeriod / timeEndPeriod
