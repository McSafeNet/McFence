#include "McDesktop.h"
#include "McLog.h"
#include "McUtil.h"

#include <exdisp.h>
#include <oleacc.h>
#include <shlobj.h>


namespace
{

// IFolderView2（Vista+）。XP 版 SDK 不声明它，按 IID 取接口后用 vtable 调用。
const IID IID_McFolderView2 = {
    0x1af3a467, 0x214f, 0x4298, {0x90, 0x8e, 0x06, 0xb0, 0x3e, 0x0b, 0x39, 0xf9}};

HHOOK g_mouse_hook = NULL;
HWND g_hook_notify = NULL;
DWORD g_last_down_time = 0;
POINT g_last_down_point = {0, 0};

bool ClassIs(HWND hwnd, const wchar_t* name)
{
    wchar_t buffer[64] = L"";
    GetClassNameW(hwnd, buffer, 64);
    return wcscmp(buffer, name) == 0;
}

// 同一进程是资源管理器（GetShellWindow 的进程）才算桌面，避免误认第三方同名窗口。
bool InShellProcess(HWND hwnd)
{
    HWND shell = GetShellWindow();
    if (!shell)
    {
        return true;
    }
    DWORD shell_pid = 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(shell, &shell_pid);
    GetWindowThreadProcessId(hwnd, &pid);
    return shell_pid == 0 || pid == 0 || shell_pid == pid;
}

BOOL CALLBACK FindDefViewInWorker(HWND hwnd, LPARAM lparam)
{
    if (ClassIs(hwnd, L"WorkerW"))
    {
        HWND view = FindWindowExW(hwnd, NULL, L"SHELLDLL_DefView", NULL);
        if (view)
        {
            *reinterpret_cast<HWND*>(lparam) = view;
            return FALSE;
        }
    }
    return TRUE;
}

typedef HRESULT(STDAPICALLTYPE* McPfnAccessibleObjectFromPoint)(POINT, IAccessible**, VARIANT*);

McPfnAccessibleObjectFromPoint AccessibleFromPointFn()
{
    static McPfnAccessibleObjectFromPoint fn = NULL;
    static bool probed = false;
    if (!probed)
    {
        probed = true;
        HMODULE m = McLoadSystemDll(L"oleacc.dll");
        fn = reinterpret_cast<McPfnAccessibleObjectFromPoint>(
            McResolveExport(m, "AccessibleObjectFromPoint"));
    }
    return fn;
}

//****************************************************************************
//	Method:		MouseHookProc
//	FullName:	MouseHookProc
//	author:
//	date:		2026/11/07
//	Function:   低级鼠标钩子：两次左键按下的间隔与距离都在系统双击阈值内时，把坐标投递给通知窗口。
//
//	Returns:	LRESULT    始终交给下一个钩子，从不吞掉输入
//	Parameter:	IN  int code    钩子代码
//              IN  WPARAM wparam    鼠标消息
//              IN  LPARAM lparam    MSLLHOOKSTRUCT*
//
//              钩子回调必须很快返回，所以这里只比较时间和坐标，其余判断在接收方做
//
//	changelog:  New
//****************************************************************************
LRESULT CALLBACK MouseHookProc(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION && wparam == WM_LBUTTONDOWN && g_hook_notify)
    {
        const MSLLHOOKSTRUCT* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
        const DWORD elapsed = info->time - g_last_down_time;
        const int dx = abs(info->pt.x - g_last_down_point.x);
        const int dy = abs(info->pt.y - g_last_down_point.y);
        if (g_last_down_time != 0 && elapsed <= GetDoubleClickTime() &&
            dx <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 && dy <= GetSystemMetrics(SM_CYDOUBLECLK) / 2)
        {
            g_last_down_time = 0;  // 第三下不再算双击
            PostMessageW(g_hook_notify, MCFENCE_WM_DESKTOP_DBLCLK, static_cast<WPARAM>(info->pt.x),
                         static_cast<LPARAM>(info->pt.y));
        }
        else
        {
            g_last_down_time = info->time;
            g_last_down_point = info->pt;
        }
    }
    return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
}

}  // namespace

//****************************************************************************
//	Method:		McFindDesktopDefView
//	FullName:	McFindDesktopDefView
//	author:
//	date:		2026/11/07
//	Function:   找到承载桌面图标的 SHELLDLL_DefView：通常在 Progman 下；Windows 7+ 换壁纸幻灯片后
//              会被挪到某个 WorkerW 下。
//
//	Returns:	HWND    找到的窗口；找不到返回 NULL
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
HWND McFindDesktopDefView()
{
    HWND progman = FindWindowW(L"Progman", NULL);
    HWND view = progman ? FindWindowExW(progman, NULL, L"SHELLDLL_DefView", NULL) : NULL;
    if (!view)
    {
        EnumWindows(&FindDefViewInWorker, reinterpret_cast<LPARAM>(&view));
    }
    return view;
}

//****************************************************************************
//	Method:		McIsDesktopBlankPoint
//	FullName:	McIsDesktopBlankPoint
//	author:
//	date:		2026/11/07
//	Function:   判断屏幕点是否是桌面空白处：窗口链上要有 SysListView32 / SHELLDLL_DefView / Progman /
//              WorkerW 且属于资源管理器进程；再用辅助功能接口确认该点不是一个桌面图标（列表项）。
//
//	Returns:	bool    是桌面空白处返回 true
//	Parameter:	IN  POINT screen    屏幕坐标
//
//              DeskBox 的做法是向资源管理器进程写内存发 LVM_HITTEST，容易被安全软件拦截；
//              这里改用 AccessibleObjectFromPoint，是屏幕阅读器的标准路径
//
//	changelog:  New
//****************************************************************************
bool McIsDesktopBlankPoint(POINT screen)
{
    HWND hwnd = WindowFromPoint(screen);
    bool desktop = false;
    for (HWND current = hwnd; current; current = GetParent(current))
    {
        if (ClassIs(current, L"SysListView32") || ClassIs(current, L"SHELLDLL_DefView") ||
            ClassIs(current, L"Progman") || ClassIs(current, L"WorkerW"))
        {
            desktop = InShellProcess(current);
            break;
        }
    }
    if (!desktop)
    {
        return false;
    }
    McPfnAccessibleObjectFromPoint from_point = AccessibleFromPointFn();
    if (!from_point)
    {
        return true;
    }
    IAccessible* accessible = NULL;
    VARIANT child;
    VariantInit(&child);
    if (FAILED(from_point(screen, &accessible, &child)) || !accessible)
    {
        return true;
    }
    VARIANT role;
    VariantInit(&role);
    bool blank = true;
    if (SUCCEEDED(accessible->get_accRole(child, &role)) && role.vt == VT_I4)
    {
        blank = role.lVal != ROLE_SYSTEM_LISTITEM;
    }
    VariantClear(&role);
    VariantClear(&child);
    accessible->Release();
    return blank;
}

bool McDesktopHookInstall(HWND notify)
{
    if (g_mouse_hook)
    {
        g_hook_notify = notify;
        return true;
    }
    g_hook_notify = notify;
    g_last_down_time = 0;
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, &MouseHookProc, GetModuleHandleW(NULL), 0);
    if (!g_mouse_hook)
    {
        McLogLastError(L"SetWindowsHookExW(WH_MOUSE_LL)");
        return false;
    }
    return true;
}

void McDesktopHookRemove()
{
    if (g_mouse_hook)
    {
        UnhookWindowsHookEx(g_mouse_hook);
        g_mouse_hook = NULL;
    }
    g_hook_notify = NULL;
}

//****************************************************************************
//	Method:		McDesktopIconSize
//	FullName:	McDesktopIconSize
//	author:
//	date:		2026/11/07
//	Function:   读取桌面图标当前的大小（逻辑像素，96 DPI 基准）：Vista 起问桌面视图本身
//              （IShellWindows → IShellBrowser → IShellView → IFolderView2::GetViewModeAndIconSize），
//              它跟随「查看」菜单和 Ctrl+滚轮缩放；拿不到时读资源管理器保存的 IconSize，
//              再不行（XP）用系统大图标尺寸。
//
//	Returns:	int    16..256 之间的图标边长
//	Parameter:	无
//
//              openFrence 用 LVM_GETITEMRECT 跨进程读写资源管理器内存来量，容易被安全软件拦截；
//              这里走资源管理器公开的 COM 接口，结果相同（实测 125% 缩放下两者都是 60）
//
//	changelog:  New
//****************************************************************************
int McDesktopIconSize()
{
    int size = 0;
    if (McOsAtLeast(6, 0))
    {
        IShellWindows* windows = NULL;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, NULL, CLSCTX_ALL, IID_IShellWindows,
                                       reinterpret_cast<void**>(&windows))) &&
            windows)
        {
            VARIANT location;
            VARIANT root;
            VariantInit(&location);
            VariantInit(&root);
            long hwnd = 0;
            IDispatch* dispatch = NULL;
            if (SUCCEEDED(windows->FindWindowSW(&location, &root, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH,
                                                &dispatch)) &&
                dispatch)
            {
                IServiceProvider* provider = NULL;
                IShellBrowser* browser = NULL;
                IShellView* view = NULL;
                IUnknown* folder_view = NULL;
                if (SUCCEEDED(dispatch->QueryInterface(IID_IServiceProvider,
                                                       reinterpret_cast<void**>(&provider))) &&
                    SUCCEEDED(provider->QueryService(SID_STopLevelBrowser, IID_IShellBrowser,
                                                     reinterpret_cast<void**>(&browser))) &&
                    SUCCEEDED(browser->QueryActiveShellView(&view)) &&
                    SUCCEEDED(view->QueryInterface(IID_McFolderView2, reinterpret_cast<void**>(&folder_view))))
                {
                    // XP 版 SDK 里 IFolderView2 被 NTDDI_VISTA 挡住，只能按 vtable 顺序调用：
                    // IUnknown 3 个 + IFolderView 14 个 + IFolderView2 里排在前面的 19 个之后，
                    // 第 37 个是 GetViewModeAndIconSize(FOLDERVIEWMODE*, int*)。
                    typedef HRESULT(STDMETHODCALLTYPE * McPfnGetViewModeAndIconSize)(IUnknown*, int*, int*);
                    McPfnGetViewModeAndIconSize get_size =
                        reinterpret_cast<McPfnGetViewModeAndIconSize>((*reinterpret_cast<void***>(folder_view))[36]);
                    int mode = 0;
                    int icon = 0;
                    if (SUCCEEDED(get_size(folder_view, &mode, &icon)))
                    {
                        size = icon;
                    }
                }
                if (folder_view) folder_view->Release();
                if (view) view->Release();
                if (browser) browser->Release();
                if (provider) provider->Release();
                dispatch->Release();
            }
            windows->Release();
        }
    }
    if (size < 16 || size > 256)
    {
        // 资源管理器只在「查看」菜单切换或退出时写这个值，Ctrl+滚轮缩放期间会滞后，所以只作后备。
        DWORD logical = 0;
        DWORD bytes = sizeof(logical);
        HKEY key = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Desktop", 0,
                          KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
        {
            DWORD type = 0;
            if (RegQueryValueExW(key, L"IconSize", NULL, &type, reinterpret_cast<BYTE*>(&logical), &bytes) ==
                    ERROR_SUCCESS &&
                type == REG_DWORD)
            {
                size = static_cast<int>(logical);
            }
            RegCloseKey(key);
        }
    }
    if (size < 16 || size > 256)
    {
        // XP 的桌面图标就是系统大图标（可在「显示属性」里改），换回 96 DPI 下的逻辑尺寸。
        HDC dc = GetDC(NULL);
        const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc)
        {
            ReleaseDC(NULL, dc);
        }
        size = MulDiv(GetSystemMetrics(SM_CXICON), 96, dpi > 0 ? dpi : 96);
    }
    return size < 16 ? 16 : (size > 256 ? 256 : size);
}

//****************************************************************************
//	Method:		McEnsureRectVisible
//	FullName:	McEnsureRectVisible
//	author:
//	date:		2026/11/07
//	Function:   显示器拔掉或分辨率变化后，围栏可能整个跑出屏幕。标题栏中段不在任何显示器上时，
//              把矩形挪进最近显示器的工作区（尺寸超出时一并缩小）。
//
//	Returns:	bool    矩形被修改返回 true
//	Parameter:	IN OUTRECT& rect    窗口矩形（屏幕坐标）
//              IN  int title_height    标题栏高度（像素）
//
//	changelog:  New
//****************************************************************************
bool McEnsureRectVisible(RECT& rect, int title_height)
{
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    RECT title;
    title.left = rect.left + width / 4;
    title.right = rect.right - width / 4;
    title.top = rect.top;
    title.bottom = rect.top + max(4, title_height);
    if (MonitorFromRect(&title, MONITOR_DEFAULTTONULL) != NULL)
    {
        return false;
    }
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info))
    {
        return false;
    }
    const RECT& area = info.rcWork;
    const int new_width = min(width, static_cast<int>(area.right - area.left));
    const int new_height = min(height, static_cast<int>(area.bottom - area.top));
    int left = max(static_cast<int>(area.left), min(static_cast<int>(rect.left),
                                                    static_cast<int>(area.right) - new_width));
    int top = max(static_cast<int>(area.top), min(static_cast<int>(rect.top),
                                                  static_cast<int>(area.bottom) - new_height));
    rect.left = left;
    rect.top = top;
    rect.right = left + new_width;
    rect.bottom = top + new_height;
    return true;
}

McFolderWatch::McFolderWatch() : m_notify(NULL), m_thread(NULL), m_stop(NULL)
{
}

McFolderWatch::~McFolderWatch()
{
    Stop();
}

//****************************************************************************
//	Method:		Start
//	FullName:	McFolderWatch::Start
//	author:
//	date:		2026/11/07
//	Function:   启动后台线程监视文件夹的直接子项变化（增删改名、大小、修改时间）。
//
//	Returns:	bool    线程启动成功返回 true
//	Parameter:	IN  HWND notify    接收 MCFENCE_WM_FOLDER_CHANGED 的窗口
//              IN  const std::wstring& folder    要监视的文件夹
//
//	changelog:  New
//****************************************************************************
bool McFolderWatch::Start(HWND notify, const std::wstring& folder)
{
    Stop();
    m_notify = notify;
    m_folder = folder;
    m_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!m_stop)
    {
        return false;
    }
    m_thread = CreateThread(NULL, 0, &McFolderWatch::ThreadProc, this, 0, NULL);
    if (!m_thread)
    {
        CloseHandle(m_stop);
        m_stop = NULL;
        return false;
    }
    return true;
}

void McFolderWatch::Stop()
{
    if (m_thread)
    {
        SetEvent(m_stop);
        WaitForSingleObject(m_thread, 5000);
        CloseHandle(m_thread);
        m_thread = NULL;
    }
    if (m_stop)
    {
        CloseHandle(m_stop);
        m_stop = NULL;
    }
}

DWORD WINAPI McFolderWatch::ThreadProc(void* parameter)
{
    McFolderWatch* self = static_cast<McFolderWatch*>(parameter);
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                         FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                         FILE_NOTIFY_CHANGE_LAST_WRITE;
    for (;;)
    {
        HANDLE change = FindFirstChangeNotificationW(self->m_folder.c_str(), FALSE, filter);
        if (change == INVALID_HANDLE_VALUE)
        {
            // 文件夹暂时不可用（网络断开、U 盘拔出）：隔一会儿再试，同时让围栏刷新成空列表。
            PostMessageW(self->m_notify, MCFENCE_WM_FOLDER_CHANGED, 0, 0);
            if (WaitForSingleObject(self->m_stop, 5000) == WAIT_OBJECT_0)
            {
                return 0;
            }
            continue;
        }
        bool stop = false;
        while (!stop)
        {
            HANDLE handles[2] = {self->m_stop, change};
            const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0 + 1)
            {
                PostMessageW(self->m_notify, MCFENCE_WM_FOLDER_CHANGED, 0, 0);
                if (!FindNextChangeNotification(change))
                {
                    break;  // 文件夹被删掉之类：重新打开
                }
            }
            else
            {
                stop = true;
            }
        }
        FindCloseChangeNotification(change);
        if (stop)
        {
            return 0;
        }
        if (WaitForSingleObject(self->m_stop, 1000) == WAIT_OBJECT_0)
        {
            return 0;
        }
    }
}
