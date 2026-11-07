// McDesktop.h - 与桌面本身打交道：找 SHELLDLL_DefView、判断桌面空白处、
// 低级鼠标钩子检测桌面双击、多显示器变化后把围栏挪回可见区域、文件夹映射的目录监视。
#pragma once

#include "framework.h"

#include <string>


// 资源管理器承载桌面图标的 SHELLDLL_DefView（在 Progman 或某个 WorkerW 下）；找不到返回 NULL。
HWND McFindDesktopDefView();

// |screen| 落在桌面上且不在任何桌面图标上（Windows XP 起的 oleacc 判断，不跨进程读写内存）。
bool McIsDesktopBlankPoint(POINT screen);

// 安装 / 卸载 WH_MOUSE_LL：在任意窗口上的左键双击都会以 MCFENCE_WM_DESKTOP_DBLCLK
// （wParam=x, lParam=y）投递给 |notify|，是否是桌面空白处由接收方判断（钩子里不能做慢事）。
bool McDesktopHookInstall(HWND notify);
void McDesktopHookRemove();

// 桌面图标当前的边长（逻辑像素，96 DPI 基准，16..256）。围栏按它画图标，和桌面上的一样大。
int McDesktopIconSize();

// 把窗口矩形挪回可见区域：标题栏中段至少要落在某个显示器的工作区里，否则挪到最近的显示器。
// 返回 true 表示 |rect| 被修改。
bool McEnsureRectVisible(RECT& rect, int title_height);

// 一个映射文件夹的变化监视：后台线程 FindFirstChangeNotificationW，变化时向 |notify|
// 投递 MCFENCE_WM_FOLDER_CHANGED。Windows XP 就有，网络路径同样可用。
class McFolderWatch
{
public:
    McFolderWatch();
    ~McFolderWatch();

    bool Start(HWND notify, const std::wstring& folder);
    void Stop();

private:
    McFolderWatch(const McFolderWatch&);
    McFolderWatch& operator=(const McFolderWatch&);

    static DWORD WINAPI ThreadProc(void* parameter);

    HWND m_notify;
    std::wstring m_folder;
    HANDLE m_thread;
    HANDLE m_stop;
};
