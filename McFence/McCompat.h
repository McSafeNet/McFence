// McCompat.h - Windows XP 安全边界：系统版本、动态 API 表、工程常量与呈现策略。
//
// 本程序要能在 Windows XP SP3 上启动并工作，因此：
//   * 工程以 _WIN32_WINNT=0x0501 编译（见 McFence.vcxproj）；
//   * 任何比 XP 新的入口都不能静态导入：dwmapi.dll、shcore.dll 以及未文档化 /
//     后 XP 的 user32 入口，全部用 LoadLibrary + GetProcAddress 解析；
//   * 系统版本走 ntdll!RtlGetVersion（NT 3.1 就有），因为清单声明了新版 supportedOS
//     之后 GetVersionEx 会不可靠。
#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#ifndef WINVER
#define WINVER 0x0501
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <string>

#if defined(NOMINMAX)
using std::max;
using std::min;
#endif


// Message ids the Windows 7.1A headers do not define.
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef CMIC_MASK_NOASYNC
#define CMIC_MASK_NOASYNC 0x00000100
#endif

// ---------------------------------------------------------------------------
// Project identity: data folder, log, mutex and window classes.
// ---------------------------------------------------------------------------
#define MCFENCE_APP_NAME            L"McFence"
#define MCFENCE_DATA_FOLDER         L"McFence"
#define MCFENCE_LOG_FILE            L"app_diag.log"
#define MCFENCE_MUTEX_NAME          L"McFence"
#define MCFENCE_ITEMS_FOLDER        L"Items"

#define MCFENCE_APP_CLASS           L"McFenceApp"
#define MCFENCE_FENCE_CLASS         L"McFenceFence"
#define MCFENCE_MENU_HOST_CLASS     L"McFenceShellMenuHost"
#define MCFENCE_HEIGHT_CLASS        L"McFenceHeightDialog"
#define MCFENCE_SETTINGS_CLASS      L"McFenceSettingsDialog"

#define MCFENCE_NEWMENU_HOST_CLASS  L"McFenceNewMenuHost"

#define MCFENCE_SETTINGS_FILE       L"settings.ini"
#define MCFENCE_RUN_VALUE           L"McFence"

#define MCFENCE_WM_RESTORE          (WM_APP + 0x50)
#define MCFENCE_WM_ICON_READY       (WM_APP + 0x51)
#define MCFENCE_WM_TRAY             (WM_APP + 0x52)   // 托盘图标回调
#define MCFENCE_WM_FOLDER_CHANGED   (WM_APP + 0x53)   // 映射文件夹有变化（监视线程投递）
#define MCFENCE_WM_RECREATE         (WM_APP + 0x54)   // 围栏被系统意外销毁，lParam = new McFenceInfo*
#define MCFENCE_WM_DESKTOP_DBLCLK   (WM_APP + 0x55)   // 桌面空白处双击（低级鼠标钩子投递）

struct McOsVersionInfo
{
    DWORD major;
    DWORD minor;
    DWORD build;
};

// RTL_OSVERSIONINFOW is not exposed by the Windows 7.1A SDK headers we build
// against, so declare the layout locally (it is stable since Windows NT 4).
struct McRtlOsVersion
{
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
};

//****************************************************************************
//	Method:		McOsVersion
//	FullName:	McOsVersion
//	author:
//	date:		2026/11/06
//	Function:   获取真实的操作系统版本号（不受应用清单 supportedOS 伪装影响），
//              优先通过 ntdll!RtlGetVersion 获取，
//              失败时回退 GetVersionExW；结果按静态变量缓存。
//
//	Returns:	const McOsVersionInfo&    包含 major/minor/build 的操作系统版本信息引用。
//	Parameter:	无
//
//              之所以不用 GetVersionEx 直接判断，是因为清单声明新版 supportedOS 后该 API 会返回虚假版本号。
//
//	changelog:  New
//****************************************************************************
// Real OS version (no manifest shimming). Windows XP SP3 reports 5.1.2600.
inline const McOsVersionInfo& McOsVersion()
{
    static McOsVersionInfo info = {0, 0, 0};
    if (info.major != 0)
    {
        return info;
    }
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
    {
        typedef LONG(WINAPI * rtl_get_version_t)(McRtlOsVersion*);
        rtl_get_version_t fn =
            reinterpret_cast<rtl_get_version_t>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn)
        {
            McRtlOsVersion vi;
            ZeroMemory(&vi, sizeof(vi));
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0)
            {
                info.major = vi.dwMajorVersion;
                info.minor = vi.dwMinorVersion;
                info.build = vi.dwBuildNumber;
                return info;
            }
        }
    }
    OSVERSIONINFOW vi;
    ZeroMemory(&vi, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    GetVersionExW(&vi);
    info.major = vi.dwMajorVersion;
    info.minor = vi.dwMinorVersion;
    info.build = vi.dwBuildNumber;
    return info;
}

//****************************************************************************
//	Method:		McOsAtLeast
//	FullName:	McOsAtLeast
//	author:
//	date:		2026/11/06
//	Function:   判断当前操作系统版本是否不低于给定的主版本号和次版本号。
//
//	Returns:	bool    当前系统版本大于等于指定版本返回 true，否则返回 false。
//	Parameter:	IN  DWORD major    要求的最低主版本号。
//              IN  DWORD minor    要求的最低次版本号。
//
//	changelog:  New
//****************************************************************************
inline bool McOsAtLeast(DWORD major, DWORD minor)
{
    const McOsVersionInfo& v = McOsVersion();
    if (v.major != major)
    {
        return v.major > major;
    }
    return v.minor >= minor;
}

inline bool McIsXpOrOlder()
{
    return !McOsAtLeast(6, 0);
}

// ---------------------------------------------------------------------------
// Lazily resolved entry points. Each helper keeps its own module handle and
// returns NULL when the OS does not provide the entry point.
// ---------------------------------------------------------------------------

inline FARPROC McResolveExport(HMODULE module, LPCSTR name)
{
    return module ? GetProcAddress(module, name) : NULL;
}

inline FARPROC McResolveOrdinal(HMODULE module, WORD ordinal)
{
    return module ? GetProcAddress(module, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(ordinal)))
                  : NULL;
}

//****************************************************************************
//	Method:		McLoadSystemDll
//	FullName:	McLoadSystemDll
//	author:
//	date:		2026/11/06
//	Function:   安全地加载一个系统 DLL，
//              避免从当前目录搜索：Windows 8 及以上使用 LOAD_LIBRARY_SEARCH_SYSTEM32 标志加载，
//              否则回退为普通 LoadLibraryW。
//
//	Returns:	HMODULE    加载得到的模块句柄；加载失败返回 NULL。
//	Parameter:	IN  const wchar_t* name    要加载的系统 DLL 文件名。
//
//              仅用于确定位于 system32 下的系统 DLL，避免 DLL 预加载劫持风险。
//
//	changelog:  New
//****************************************************************************
inline HMODULE McLoadSystemDll(const wchar_t* name)
{
    // Never search the current directory for a system DLL: this process may be
    // unpacked next to user files. LOAD_LIBRARY_SEARCH_SYSTEM32 needs Windows 8
    // (or the KB2533623 update), so older systems fall back to a bare load - the
    // DLLs we request here live in system32 and are already loaded.
    if (McOsAtLeast(8, 0))
    {
        HMODULE m = LoadLibraryExW(name, NULL, 0x00000800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
        if (m)
        {
            return m;
        }
    }
    return LoadLibraryW(name);
}

// -- dwmapi.dll (Vista+) -----------------------------------------------------
// Only composition state is queried; the layered window never uses DWM frames.

typedef HRESULT(WINAPI * McPfnDwmIsCompositionEnabled)(BOOL*);

//****************************************************************************
//	Method:		McDwmIsCompositionEnabledFn
//	FullName:	McDwmIsCompositionEnabledFn
//	author:
//	date:		2026/11/06
//	Function:   按需从 dwmapi.dll（Vista 及以上）解析并缓存 DwmIsCompositionEnabled 函数指针。
//
//	Returns:	McPfnDwmIsCompositionEnabled    解析到的函数指针；系统不支持或未加载成功时为 NULL。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
inline McPfnDwmIsCompositionEnabled McDwmIsCompositionEnabledFn()
{
    static McPfnDwmIsCompositionEnabled fn = NULL;
    static bool probed = false;
    if (!probed)
    {
        probed = true;
        HMODULE m = McOsAtLeast(6, 0) ? McLoadSystemDll(L"dwmapi.dll") : NULL;
        fn = reinterpret_cast<McPfnDwmIsCompositionEnabled>(
            McResolveExport(m, "DwmIsCompositionEnabled"));
    }
    return fn;
}

//****************************************************************************
//	Method:		McDwmCompositionEnabled
//	FullName:	McDwmCompositionEnabled
//	author:
//	date:		2026/11/06
//	Function:   查询当前系统是否启用了 DWM 桌面合成特效。
//
//	Returns:	bool    DWM 合成已启用返回 true；API 不可用或未启用返回 false。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
inline bool McDwmCompositionEnabled()
{
    McPfnDwmIsCompositionEnabled fn = McDwmIsCompositionEnabledFn();
    if (!fn)
    {
        return false;
    }
    BOOL enabled = FALSE;
    return SUCCEEDED(fn(&enabled)) && enabled != FALSE;
}

// -- user32: DPI + the undocumented accent API -------------------------------

enum { MCFENCE_WCA_ACCENT_POLICY = 19 };
enum
{
    MCFENCE_ACCENT_DISABLED = 0,
    MCFENCE_ACCENT_GRADIENT = 1,
    MCFENCE_ACCENT_TRANSPARENT_GRADIENT = 2,
    MCFENCE_ACCENT_BLURBEHIND = 3,
    MCFENCE_ACCENT_ACRYLIC_BLURBEHIND = 4
};
enum { MCFENCE_DRAW_ALL_BORDERS = 0x20, MCFENCE_DRAW_LEFT_BORDER = 0x20 };

struct McAccentPolicy
{
    DWORD AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;  // AABBGGRR
    DWORD AnimationId;
};

struct McWindowCompositionData
{
    DWORD Attrib;
    PVOID pvData;
    SIZE_T cbData;
};

typedef BOOL(WINAPI* McPfnSetWindowCompositionAttribute)(HWND, McWindowCompositionData*);
typedef UINT(WINAPI* McPfnGetDpiForWindow)(HWND);
typedef BOOL(WINAPI* McPfnSetProcessDPIAware)();
typedef HRESULT(WINAPI* McPfnSetProcessDpiAwareness)(int);
typedef BOOL(WINAPI* McPfnSetProcessDpiAwarenessContext)(void*);
typedef int(WINAPI* McPfnGetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
typedef int(WINAPI* McPfnSetPreferredAppMode)(int);

struct McUser32Api
{
    McPfnSetWindowCompositionAttribute set_window_composition_attribute;
    McPfnGetDpiForWindow get_dpi_for_window;
    McPfnSetProcessDPIAware set_process_dpi_aware;
};

//****************************************************************************
//	Method:		McUser32Ext
//	FullName:	McUser32Ext
//	author:
//	date:		2026/11/06
//	Function:   按需从 user32.dll 解析并缓存几个高版本才有的可选入口
//             （SetWindowCompositionAttribute、GetDpiForWindow、SetProcessDPIAware）。
//
//	Returns:	const McUser32Api&    包含已解析函数指针（不支持则为 NULL）的静态结构体引用。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
inline const McUser32Api& McUser32Ext()
{
    static McUser32Api api = {NULL, NULL, NULL};
    static bool probed = false;
    if (!probed)
    {
        probed = true;
        HMODULE m = GetModuleHandleW(L"user32.dll");
        api.set_window_composition_attribute = reinterpret_cast<McPfnSetWindowCompositionAttribute>(
            McResolveExport(m, "SetWindowCompositionAttribute"));
        api.get_dpi_for_window =
            reinterpret_cast<McPfnGetDpiForWindow>(McResolveExport(m, "GetDpiForWindow"));
        api.set_process_dpi_aware =
            reinterpret_cast<McPfnSetProcessDPIAware>(McResolveExport(m, "SetProcessDPIAware"));
    }
    return api;
}

// shcore.dll (Windows 8.1+): SetProcessDpiAwareness / GetDpiForMonitor.
struct McShcoreApi
{
    McPfnSetProcessDpiAwareness set_process_dpi_awareness;
    McPfnGetDpiForMonitor get_dpi_for_monitor;
    bool available() const
    {
        return set_process_dpi_awareness != NULL;
    }
};

//****************************************************************************
//	Method:		McShcoreExt
//	FullName:	McShcoreExt
//	author:
//	date:		2026/11/06
//	Function:   按需加载 shcore.dll（Windows 8.1+）并解析缓存 SetProcessDpiAwareness、
//              GetDpiForMonitor 入口。
//
//	Returns:	const McShcoreApi&  包含已解析函数指针的静态结构体引用；系统不支持时指针为 NULL。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
inline const McShcoreApi& McShcoreExt()
{
    static McShcoreApi api = {NULL, NULL};
    static bool probed = false;
    if (!probed)
    {
        probed = true;
        HMODULE m = McLoadSystemDll(L"shcore.dll");
        if (m)
        {
            api.set_process_dpi_awareness = reinterpret_cast<McPfnSetProcessDpiAwareness>(
                McResolveExport(m, "SetProcessDpiAwareness"));
            api.get_dpi_for_monitor =
                reinterpret_cast<McPfnGetDpiForMonitor>(McResolveExport(m, "GetDpiForMonitor"));
        }
    }
    return api;
}

//****************************************************************************
//	Method:		McUxthemePreferredMode
//	FullName:	McUxthemePreferredMode
//	author:
//	date:		2026/11/06
//	Function:   在 Windows 10 1809 及以上版本，按序号 135 从 uxtheme.dll 解析并缓存未文档化的 
//              SetPreferredAppMode 入口，用于启用深色菜单。
//
//	Returns:	McPfnSetPreferredAppMode    解析到的函数指针；系统版本过低或解析失败时为 NULL。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
// uxtheme.dll ordinal #135 SetPreferredAppMode (Windows 10 1809+): dark menus.
inline McPfnSetPreferredAppMode McUxthemePreferredMode()
{
    static McPfnSetPreferredAppMode fn = NULL;
    static bool probed = false;
    if (!probed)
    {
        probed = true;
        if (McOsAtLeast(10, 0))
        {
            HMODULE m = McLoadSystemDll(L"uxtheme.dll");
            fn = reinterpret_cast<McPfnSetPreferredAppMode>(McResolveOrdinal(m, 135));
        }
    }
    return fn;
}

// -- shell32: DPI-friendly icons --------------------------------------------

// SHGetImageList is exported by ordinal only (#727). It exists from Windows XP,
// but only Vista and later provide the extra-large (2) and jumbo (4) lists.
typedef HRESULT(WINAPI* McPfnSHGetImageList)(int, const GUID*, void**);

//****************************************************************************
//	Method:		McShellImageList
//	FullName:	McShellImageList
//	author:
//	date:		2026/11/06
//	Function:   按序号 727 从 shell32.dll 解析并缓存未文档化的 SHGetImageList 入口。
//
//	Returns:	McPfnSHGetImageList    解析到的函数指针；解析失败时为 NULL。
//	Parameter:	无
//
//              该入口自 Windows XP 起即存在，但超大/巨型图标列表仅 Vista 及以上支持。
//
//	changelog:  New
//****************************************************************************
inline McPfnSHGetImageList McShellImageList()
{
    static McPfnSHGetImageList fn = NULL;
    static bool probed = false;
    if (!probed)
    {
        probed = true;
        HMODULE m = GetModuleHandleW(L"shell32.dll");
        fn = reinterpret_cast<McPfnSHGetImageList>(McResolveOrdinal(m, 727));
    }
    return fn;
}

struct McStockIconInfo
{
    DWORD cbSize;
    HICON hIcon;
    int iSysIconIndex;
    int iIcon;
    WCHAR szPath[MAX_PATH];
};

typedef HRESULT(WINAPI* McPfnSHGetStockIconInfo)(UINT, UINT, McStockIconInfo*);

//****************************************************************************
//	Method:		McShellStockIcon
//	FullName:	McShellStockIcon
//	author:
//	date:		2026/11/06
//	Function:   在 Vista 及以上系统，从 shell32.dll 解析并缓存 SHGetStockIconInfo 入口。
//
//	Returns:	McPfnSHGetStockIconInfo    解析到的函数指针；系统版本过低或解析失败时为 NULL。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
inline McPfnSHGetStockIconInfo McShellStockIcon()
{
    static McPfnSHGetStockIconInfo fn = NULL;
    static bool probed = false;
    if (!probed)
    {
        probed = true;
        if (McOsAtLeast(6, 0))
        {
            HMODULE m = GetModuleHandleW(L"shell32.dll");
            fn = reinterpret_cast<McPfnSHGetStockIconInfo>(McResolveExport(m, "SHGetStockIconInfo"));
        }
    }
    return fn;
}

// ---------------------------------------------------------------------------
// Presentation policy
// ---------------------------------------------------------------------------
// SimpleGrid paints a per-pixel alpha panel. UpdateLayeredWindow is the only way
// to do that on every supported OS (XP through 11), so it is the universal
// presentation path. Aero / accent blur is an optional extra on top of it:
//   * Windows 7 + composition: DwmEnableBlurBehindWindow on a layered window is
//     ignored by DWM, so blur is reported as unavailable (the panel stays
//     translucent) - documented and accepted.
//   * Windows 10/11: the undocumented accent policy still blurs the backdrop
//     behind layered windows, which is what the original C# build relied on.
enum McPresentMode
{
    MCFENCE_PRESENT_LAYERED = 0,   // always correct, no blur
    MCFENCE_PRESENT_BLURRED   // layered + accent blur behind (Windows 10/11)
};

//****************************************************************************
//	Method:		McDetectPresentMode
//	FullName:	McDetectPresentMode
//	author:
//	date:		2026/11/06
//	Function:   根据系统是否支持 SetWindowCompositionAttribute 且版本不低于 Windows 10，
//              决定呈现模式是启用背景模糊还是普通分层呈现。
//
//	Returns:	McPresentMode    MCFENCE_PRESENT_BLURRED 表示可用背景模糊，
//                               MCFENCE_PRESENT_LAYERED 表示仅普通分层呈现。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
inline McPresentMode McDetectPresentMode()
{
    const McUser32Api& u = McUser32Ext();
    if (u.set_window_composition_attribute && McOsAtLeast(10, 0))
    {
        return MCFENCE_PRESENT_BLURRED;
    }
    return MCFENCE_PRESENT_LAYERED;
}

//****************************************************************************
//	Method:		McEnableBlur
//	FullName:	McEnableBlur
//	author:
//	date:		2026/11/06
//	Function:   通过未文档化的 SetWindowCompositionAttribute 接口为窗口启用背景模糊
//             （ACCENT_ENABLE_BLURBEHIND），仅在检测到支持模糊的呈现模式时尝试。
//
//	Returns:	bool    成功设置模糊特效返回 true；当前呈现模式不支持模糊、句柄为空或调用失败时返回 false。
//	Parameter:	IN  HWND hwnd    要启用背景模糊的窗口句柄。
//
//	changelog:  New
//****************************************************************************
// BlurUtil.EnableBlur: ACCENT_ENABLE_BLURBEHIND through the undocumented accent
// policy. Only attempted on Windows 10/11; returns false when blur is not on.
inline bool McEnableBlur(HWND hwnd)
{
    if (McDetectPresentMode() != MCFENCE_PRESENT_BLURRED || !hwnd)
    {
        return false;
    }
    McAccentPolicy policy;
    ZeroMemory(&policy, sizeof(policy));
    policy.AccentState = MCFENCE_ACCENT_BLURBEHIND;
    McWindowCompositionData data;
    data.Attrib = MCFENCE_WCA_ACCENT_POLICY;
    data.pvData = &policy;
    data.cbData = sizeof(policy);
    return McUser32Ext().set_window_composition_attribute(hwnd, &data) != FALSE;
}

