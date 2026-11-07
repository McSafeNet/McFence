// McShellMenu.h - 单个文件/文件夹的真实 Windows 右键菜单（Shift+右键）。
//
// 用纯 COM 表达：隐藏的宿主窗口接收
// Shell 处理程序需要的菜单消息（IContextMenu2/3），所以自绘菜单项和“发送到”
// 子菜单都能正常工作。
#pragma once

#include "framework.h"

#include <shlobj.h>
#include <string>


class McShellMenu
{
public:
    McShellMenu();
    ~McShellMenu();

    // Shows the shell context menu for |path| at |screen_point|.
    bool Show(const std::wstring& path, POINT screen_point);

private:
    static ATOM EnsureWindowClass();
    static LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    void Release();

    HWND m_host;
    IContextMenu* m_context_menu;
    IContextMenu2* m_context_menu2;
    IContextMenu3* m_context_menu3;
    std::wstring m_folder;
};

