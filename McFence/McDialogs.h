// McDialogs.h - 围栏菜单与托盘菜单打开的几个小对话框。
//
// 全部用纯 Win32 窗口手工搭建：不需要 .rc 模板，也不依赖框架。
#pragma once

#include "framework.h"
#include "McSettings.h"

#include <string>


// "标题高度" - trackbar 20..100 px with a restore button.
bool McShowHeightDialog(HWND owner, int current_height, int& new_height);

// 全局选项（开机启动、双击桌面、Win+D、吸附、热键）。确定返回 true；不负责保存与应用。
bool McShowSettingsDialog(HWND owner, McSettings& settings, bool& autostart);
