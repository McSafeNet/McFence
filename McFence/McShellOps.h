// McShellOps.h - 与 Shell 打交道的几件事：把围栏里的文件拖出到资源管理器、
// 复制 / 移动 / 回收、以及资源管理器自己的「新建」子菜单。
//
// 全部只用 Windows XP 就有的接口：自己实现带 CF_HDROP 的 IDataObject（XP 没有
// SHCreateDataObject）、SHFileOperationW、CLSID_NewMenu（Windows 2000 起就有）。
#pragma once

#include "framework.h"

#include <shlobj.h>

#include <string>
#include <vector>


// 把 |paths| 作为 CF_HDROP 拖出去；阻塞到放下或取消，返回目标报告的效果。
// 通过 Preferred DropEffect 让资源管理器跨分区也默认移动（按住 Ctrl 复制）。
DWORD McDoDragOut(const std::vector<std::wstring>& paths, DWORD allowed);

// SHFileOperationW 包装：复制或移动到 |folder|（冲突时由系统询问），带撤销。
bool McShellCopyMove(HWND owner, const std::vector<std::wstring>& paths, const std::wstring& folder,
                     bool move);

// 删除到回收站（系统会弹确认框）。
bool McShellRecycle(HWND owner, const std::vector<std::wstring>& paths);

// 资源管理器的「新建」子菜单（新建文件夹 / 文本文档 / 第三方 ShellNew 模板）。
class McNewMenu
{
public:
    McNewMenu();
    ~McNewMenu();

    // 在 |menu| 的 |position| 处插入「新建」子菜单，新项目建在 |folder| 里。
    bool Attach(HMENU menu, UINT position, const std::wstring& folder, UINT first_id, UINT last_id);

    // 菜单期间把 WM_INITMENUPOPUP / WM_DRAWITEM / WM_MEASUREITEM / WM_MENUCHAR 转给处理程序，
    // 否则子菜单是空的、图标也画不出来。已处理返回 true。
    bool HandleMenuMessage(UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result);

    bool Owns(UINT command) const;

    // 执行命令，返回新建出来的项目路径（找不到返回空串）。
    std::wstring Invoke(HWND owner, UINT command);

    void Release();

private:
    McNewMenu(const McNewMenu&);
    McNewMenu& operator=(const McNewMenu&);

    // 处理程序按系统界面语言出字（只装了中文语言包时全是中文）；McFence 选了英文时把文字换掉。
    void LocalizeItems(HMENU menu);

    IContextMenu* m_menu;
    IContextMenu2* m_menu2;
    IContextMenu3* m_menu3;
    std::wstring m_folder;
    UINT m_first;
    UINT m_last;
    HMENU m_owner_menu;  // 插入「新建」的那个菜单：它自己的 WM_INITMENUPOPUP 不能转发
};

// 选一个文件夹（SHBrowseForFolderW，Windows XP 起可用新式对话框）；取消返回 false。
bool McPickFolder(HWND owner, const wchar_t* title, std::wstring& folder);
