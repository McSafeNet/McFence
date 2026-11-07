// McFence.h: 应用程序入口相关的全局声明。
//
// 窗口类名、自定义消息与工程常量集中在 McCompat.h，这里只暴露进程级状态与入口函数。
//

#pragma once

#include "framework.h"
#include "McResource.h"

#define MAX_LOADSTRING 100

// 全局变量:
extern HINSTANCE hInst;                                  // 当前实例
extern WCHAR     szTitle[MAX_LOADSTRING];                // 标题栏文本
extern WCHAR     szWindowClass[MAX_LOADSTRING];          // 主窗口类名

// 显示「关于 McFence」对话框（由围栏右键菜单调用）。
void McFenceShowAbout(HWND hWndOwner);

// 弹出「选项」对话框，确定后保存并立即应用（围栏右键菜单与托盘菜单共用）。
void McFenceShowSettings(HWND hWndOwner);

// 新建一个围栏；bMapped 为 true 时先让用户选一个文件夹，建成文件夹映射围栏。
void McFenceCreateFence(HWND hWndOwner, bool bMapped);

// 切换界面语言（McLanguage）并保存；托盘菜单「语言」子菜单调用。
void McFenceSetLanguage(int nLanguage);

