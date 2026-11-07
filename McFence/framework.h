// header.h: 标准系统包含文件的包含文件，
// 或特定于项目的包含文件
//

#pragma once

#include "targetver.h"
#define WIN32_LEAN_AND_MEAN             // 从 Windows 头文件中排除极少使用的内容
// Windows 头文件
#include <windows.h>

// C 运行时头文件
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>

// McFence: XP 兼容层（系统版本、动态 API 表、工程常量、窗口类名、自定义消息）
#include "McCompat.h"

// 标准库
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(NOMINMAX)
using std::max;
using std::min;
#endif
