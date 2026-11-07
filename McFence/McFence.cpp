// McFence.cpp : 定义应用程序的入口点。
//

#include "framework.h"
#include "McFence.h"
#include "McDesktop.h"
#include "McDialogs.h"
#include "McFenceWnd.h"
#include "McIcons.h"
#include "McLog.h"
#include "McModel.h"
#include "McSettings.h"
#include "McShellOps.h"
#include "McShortcut.h"
#include "McStore.h"
#include "McUtil.h"

#include <commctrl.h>
#include <gdiplus.h>

// 全局变量:
HINSTANCE hInst;                                // 当前实例
WCHAR szTitle[MAX_LOADSTRING];                  // 标题栏文本
WCHAR szWindowClass[MAX_LOADSTRING];            // 主窗口类名

// 进程级状态：单实例互斥体、GDI+ 令牌、隐藏的接收后台通知的窗口
static HANDLE    g_hInstanceMutex = nullptr;
static ULONG_PTR g_gdiplusToken = 0;
static HWND      g_hAppWindow = nullptr;
static bool      g_bTrayAdded = false;
static bool      g_bHotkeyRegistered = false;
static UINT      g_uTaskbarCreated = 0;

const UINT MCFENCE_TRAY_ID = 1;
const int  MCFENCE_HOTKEY_ID = 1;
const UINT MCFENCE_TIMER_ICON_SIZE = 1;
const UINT MCFENCE_ICON_SIZE_POLL_MS = 1500;   // 桌面 Ctrl+滚轮缩放没有通知，只能定时看一眼

// 此代码模块中包含的函数的前向声明:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

static void ConfigureDpiAwareness();
static bool InitSingleInstance();
static int  LoadFencesAndShow();
static int  RunVerificationSwitches();
static void McFenceShutdown();
static bool AddTrayIcon();
static void RemoveTrayIcon();
static void ShowTrayMenu();
static void ApplySettings();
static void ToggleAllFences();

//****************************************************************************
//	Method:		wWinMain
//	FullName:	wWinMain
//	author:
//	date:		2026/11/06
//	Function:   程序入口：加载标题/窗口类字符串，调用 InitInstance 完成初始化，运行标准消息循环直到收到 WM_QUIT，再执行 McFenceShutdown 做清理。
//
//	Returns:	int    进程退出码（WM_QUIT 的 wParam）
//	Parameter:	IN  HINSTANCE hInstance    当前实例句柄
//              IN  HINSTANCE hPrevInstance    未使用
//              IN  LPWSTR lpCmdLine    未使用
//              IN  int nCmdShow    窗口显示方式（本应用无可见主窗口，未使用）
//
//	changelog:  New
//****************************************************************************
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 初始化全局字符串
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_MCFENCE, szWindowClass, MAX_LOADSTRING);

    // 执行应用程序初始化:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    MSG msg;

    // 主消息循环:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    McFenceShutdown();
    return (int) msg.wParam;
}

//****************************************************************************
//	Method:		MyRegisterClass
//	FullName:	MyRegisterClass
//	author:
//	date:		2026/11/06
//	Function:   注册主（隐藏）消息窗口的窗口类。
//
//	Returns:	ATOM    注册成功返回非零的类原子
//	Parameter:	IN  HINSTANCE hInstance    模块实例句柄
//
//	changelog:  New
//****************************************************************************
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = 0;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MCFENCE));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = nullptr;
    wcex.lpszMenuName   = nullptr;                  // 主窗口从不显示，没有菜单
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//****************************************************************************
//	Method:		InitInstance
//	FullName:	InitInstance
//	author:
//	date:		2026/11/06
//	Function:   保存实例句柄；用互斥体保证单实例运行；配置 DPI 感知；按系统支持情况启用跟随系统的暗色右键菜单；初始化公共控件、GDI+、OLE；注册窗口类并创建隐藏的消息专用窗口用于接收图标加载完成通知；加载并显示所有围栏；处理开发用验证开关。
//
//	Returns:	BOOL    初始化成功返回 TRUE，任一关键步骤失败返回 FALSE
//	Parameter:	IN  HINSTANCE hInstance    模块实例句柄
//              IN  int nCmdShow    未使用
//
//              本应用没有可见主框架窗口，每个围栏都是独立的分层弹出窗口
//
//	changelog:  New
//****************************************************************************
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    UNREFERENCED_PARAMETER(nCmdShow);

    hInst = hInstance;                              // 将实例句柄存储在全局变量中

    if (!InitSingleInstance())
    {
        return FALSE;
    }

    ConfigureDpiAwareness();

    // Windows 10 1809 起可以用暗色右键菜单；更早的系统上该导出不存在，会静默跳过。
    McPfnSetPreferredAppMode pfnPreferredMode = McUxthemePreferredMode();
    if (pfnPreferredMode != nullptr)
    {
        pfnPreferredMode(1);                        // AllowDark：跟随系统暗色设置
    }

    INITCOMMONCONTROLSEX controls;
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok)
    {
        McLog(L"GdiplusStartup 失败");
        return FALSE;
    }

    if (FAILED(OleInitialize(nullptr)))
    {
        McLog(L"OleInitialize 失败");
    }

    if (MyRegisterClass(hInstance) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        McLogLastError(L"RegisterClassExW(主窗口)");
        return FALSE;
    }

    // 隐藏的顶层窗口（不是 HWND_MESSAGE）：托盘回调、全局热键与 TaskbarCreated 广播都需要它。
    g_uTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_hAppWindow = CreateWindowExW(WS_EX_TOOLWINDOW, szWindowClass, szTitle, WS_POPUP, 0, 0, 0, 0,
                                  nullptr, nullptr, hInstance, nullptr);
    if (g_hAppWindow == nullptr)
    {
        McLogLastError(L"CreateWindowExW(主窗口)");
    }
    McIconCache::Instance().SetNotifyWindow(g_hAppWindow);
    McFenceWnd::McSetAppWindow(g_hAppWindow);
    McFenceWnd::McSetIconSize(McDesktopIconSize());  // 围栏建出来就和桌面图标一样大

    const int nViews = LoadFencesAndShow();
    if (nViews <= 0)
    {
        McLog(L"没有可用的围栏窗口，退出");
        McFenceShutdown();
        return FALSE;
    }

    // 开发用开关：MCFENCE_SELFTEST / MCFENCE_DUMP_SURFACE。
    const int nVerify = RunVerificationSwitches();
    if (nVerify >= 0)
    {
        McFenceShutdown();
        return FALSE;                               // 验证模式：做完就退，不进消息循环
    }

    AddTrayIcon();
    ApplySettings();
    if (g_hAppWindow != nullptr)
    {
        SetTimer(g_hAppWindow, MCFENCE_TIMER_ICON_SIZE, MCFENCE_ICON_SIZE_POLL_MS, nullptr);
    }
    return TRUE;
}

//****************************************************************************
//	Method:		WndProc
//	FullName:	WndProc
//	author:
//	date:		2026/11/06
//	Function:   隐藏主窗口的窗口过程：图标就绪重绘、托盘菜单、全局热键、桌面双击、围栏重建、
//              TaskbarCreated 后重新添加托盘图标并重新挂接围栏；WM_DESTROY 发出退出消息。
//
//	Returns:	LRESULT    消息处理结果
//	Parameter:	IN  HWND hWnd    窗口句柄
//              IN  UINT message    消息编号
//              IN  WPARAM wParam    消息参数
//              IN  LPARAM lParam    消息参数
//
//	changelog:  2026/11/07 托盘、热键、双击桌面、围栏重建
//****************************************************************************
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case MCFENCE_WM_ICON_READY:
        McFenceWnd::McInvalidateAll();
        return 0;

    case MCFENCE_WM_TRAY:
        switch (LOWORD(lParam))
        {
        case WM_LBUTTONDBLCLK:
            ToggleAllFences();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            break;
        default:
            break;
        }
        return 0;

    case WM_HOTKEY:
        if (wParam == MCFENCE_HOTKEY_ID)
        {
            ToggleAllFences();
        }
        return 0;

    case MCFENCE_WM_DESKTOP_DBLCLK:
    {
        // 钩子只报告“双击”，是否落在桌面空白处在这里判断（钩子回调里不能做慢事）。
        POINT point;
        point.x = static_cast<LONG>(static_cast<INT_PTR>(wParam));
        point.y = static_cast<LONG>(lParam);
        if (McSettingsGet().double_click_desktop && McIsDesktopBlankPoint(point))
        {
            ToggleAllFences();
        }
        return 0;
    }

    case MCFENCE_WM_RECREATE:
    {
        McFenceInfo* pInfo = reinterpret_cast<McFenceInfo*>(lParam);
        if (pInfo != nullptr)
        {
            // 资源管理器可能还没把新桌面建好：围栏先不挂所有者，TaskbarCreated 时再挂。
            McFenceWnd::Create(*pInfo);
            delete pInfo;
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == MCFENCE_TIMER_ICON_SIZE)
        {
            McFenceWnd::McSetIconSize(McDesktopIconSize());
        }
        return 0;

    case WM_SETTINGCHANGE:
        // 「显示属性」改了图标大小（XP）等，立即跟上，不等下一次轮询。
        McFenceWnd::McSetIconSize(McDesktopIconSize());
        break;

    case WM_DESTROY:
        KillTimer(hWnd, MCFENCE_TIMER_ICON_SIZE);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    if (message == g_uTaskbarCreated && g_uTaskbarCreated != 0)
    {
        // 资源管理器重启：托盘图标要重新添加，围栏重新挂到新的 SHELLDLL_DefView 名下。
        g_bTrayAdded = false;
        AddTrayIcon();
        McFenceWnd::McApplyDesktopOwner();
        return 0;
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

//****************************************************************************
//	Method:		About
//	FullName:	About
//	author:
//	date:		2026/11/06
//	Function:   “关于”对话框的消息处理过程：WM_INITDIALOG 返回 TRUE；WM_COMMAND 中 IDOK/IDCANCEL 关闭对话框。
//
//	Returns:	INT_PTR    消息是否被处理
//	Parameter:	IN  HWND hDlg    对话框句柄
//              IN  UINT message    消息编号
//              IN  WPARAM wParam    消息参数
//              IN  LPARAM lParam    未使用
//
//	changelog:  New
//****************************************************************************
// “关于”框的消息处理程序。
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    // 网站链接的下划线字体与捐赠徽标的红底画刷，只在对话框存活期间有效（关于框同一时间只有一个）。
    static HFONT s_hLinkFont = nullptr;
    static HBRUSH s_hDonateBrush = nullptr;

    switch (message)
    {
    case WM_INITDIALOG:
    {
        UNREFERENCED_PARAMETER(lParam);
        // 对话框模板是中文的；英文界面时在这里逐个替换控件文本。
        SetWindowTextW(hDlg, McLoc(L"About McFence", L"关于 McFence"));
        SetDlgItemTextW(hDlg, IDC_MCFENCE_ABOUT_FORUM, McLoc(L"Forum:", L"官方论坛："));
        SetDlgItemTextW(hDlg, IDC_MCFENCE_ABOUT_VERSION, McLoc(L"Version: v1.0", L"版本：v1.0"));
        SetDlgItemTextW(hDlg, IDC_MCFENCE_ABOUT_COPYRIGHT, McLoc(L"Copyright © Edward", L"版权所有© Edward"));
        SetDlgItemTextW(hDlg, IDC_MCFENCE_ABOUT_EMAIL, McLoc(L"Email: mcsafenet@163.com", L"联系邮箱：mcsafenet@163.com"));
        SetDlgItemTextW(hDlg, IDC_MCFENCE_ABOUT_DESC,
                        McLoc(L"Lightweight desktop icon organizer, open-source Stardock Fences alternative",
                              L"一个轻量级的Windows桌面图标分组管理工具，Stardock Fences的开源替代"));
        SetDlgItemTextW(hDlg, IDC_MCFENCE_ABOUT_DONATE, McLoc(L"Donate", L"捐赠"));
        SetDlgItemTextW(hDlg, IDC_MCFENCE_ABOUT_DONATE_TEXT,
                        McLoc(L"If this tool really helps you, you can buy the author a coffee.",
                              L"如果你觉得该工具对你真的有帮助，可以请作者喝杯咖啡。"));
        SetDlgItemTextW(hDlg, IDC_MCFENCE_ABOUT_WECHAT_TEXT, McLoc(L"WeChat", L"微信"));
        SetDlgItemTextW(hDlg, IDC_MCFENCE_ABOUT_ALIPAY_TEXT, McLoc(L"Alipay", L"支付宝"));
        SetDlgItemTextW(hDlg, IDOK, McLoc(L"OK", L"确定"));

        // 网站链接：在对话框字体基础上加下划线。
        LOGFONTW lf;
        ZeroMemory(&lf, sizeof(lf));
        HFONT hBase = reinterpret_cast<HFONT>(SendMessageW(hDlg, WM_GETFONT, 0, 0));
        if (hBase == nullptr || GetObjectW(hBase, sizeof(lf), &lf) == 0)
        {
            lf.lfHeight = -12;
            lf.lfCharSet = DEFAULT_CHARSET;
            lstrcpynW(lf.lfFaceName, L"MS Shell Dlg", LF_FACESIZE);
        }
        lf.lfUnderline = TRUE;
        s_hLinkFont = CreateFontIndirectW(&lf);
        if (s_hLinkFont != nullptr)
        {
            SendDlgItemMessageW(hDlg, IDC_MCFENCE_ABOUT_WEBSITE, WM_SETFONT,
                                reinterpret_cast<WPARAM>(s_hLinkFont), TRUE);
        }
        s_hDonateBrush = CreateSolidBrush(RGB(178, 34, 34));

        // ICON 控件按系统图标尺寸（32x32）载入；显式按 64x64 取一张，左上角才是完整的大图标。
        // LR_SHARED：句柄归系统，不用 DestroyIcon。
        HICON hIcon = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_MCFENCE), IMAGE_ICON, 64, 64,
                                                    LR_SHARED));
        if (hIcon != nullptr)
        {
            SendDlgItemMessageW(hDlg, IDC_MCFENCE_ABOUT_ICON, STM_SETICON, reinterpret_cast<WPARAM>(hIcon), 0);
        }
        return (INT_PTR)TRUE;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        const int nId = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        if (nId == IDC_MCFENCE_ABOUT_WEBSITE)
        {
            SetTextColor(hdc, RGB(0, 0, 238));          // 链接蓝字，背景透明
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
        }
        if (nId == IDC_MCFENCE_ABOUT_DONATE && s_hDonateBrush != nullptr)
        {
            SetTextColor(hdc, RGB(255, 255, 255));      // 红底白字徽标
            SetBkColor(hdc, RGB(178, 34, 34));
            return reinterpret_cast<INT_PTR>(s_hDonateBrush);
        }
        break;
    }

    case WM_SETCURSOR:
        // 鼠标移到网站链接上显示手形。
        if (reinterpret_cast<HWND>(wParam) == GetDlgItem(hDlg, IDC_MCFENCE_ABOUT_WEBSITE))
        {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, TRUE);
            return (INT_PTR)TRUE;
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        // 网站链接是 SS_NOTIFY 静态文本，单击发 STN_CLICKED。
        if (LOWORD(wParam) == IDC_MCFENCE_ABOUT_WEBSITE && HIWORD(wParam) == STN_CLICKED)
        {
            ShellExecuteW(hDlg, L"open", L"https://www.mcsafenet.com", nullptr, nullptr, SW_SHOWNORMAL);
            return (INT_PTR)TRUE;
        }
        break;

    case WM_DESTROY:
        if (s_hLinkFont != nullptr)
        {
            DeleteObject(s_hLinkFont);
            s_hLinkFont = nullptr;
        }
        if (s_hDonateBrush != nullptr)
        {
            DeleteObject(s_hDonateBrush);
            s_hDonateBrush = nullptr;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

//****************************************************************************
//	Method:		McFenceShowAbout
//	FullName:	McFenceShowAbout
//	author:
//	date:		2026/11/06
//	Function:   由围栏右键菜单弹出「关于 McFence」对话框
//
//	Returns:	void
//	Parameter:	IN  HWND hWndOwner    对话框的所有者窗口
//
//	changelog:  New
//****************************************************************************
void McFenceShowAbout(HWND hWndOwner)
{
    DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWndOwner, About);
}

//****************************************************************************
//	Method:		ConfigureDpiAwareness
//	FullName:	ConfigureDpiAwareness
//	author:
//	date:		2026/11/06
//	Function:   按操作系统版本选择可用的最高 DPI 感知级别：Windows 10 1703+ 用 Per-Monitor V2，8/8.1 用 PerMonitor，Vista/7 退化为系统级 DPI 感知声明，XP 不做处理。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
static void ConfigureDpiAwareness()
{
    if (!McOsAtLeast(6, 0))
    {
        return;
    }

    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    McPfnSetProcessDpiAwarenessContext pfnSetContext =
        reinterpret_cast<McPfnSetProcessDpiAwarenessContext>(McResolveExport(hUser32,
                                                                           "SetProcessDpiAwarenessContext"));
    if (pfnSetContext != nullptr && pfnSetContext(reinterpret_cast<void*>(-4)))
    {
        return;                                     // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }

    const McShcoreApi& shcore = McShcoreExt();
    if (shcore.set_process_dpi_awareness != nullptr &&
        SUCCEEDED(shcore.set_process_dpi_awareness(2)))
    {
        return;                                     // PROCESS_PER_MONITOR_DPI_AWARE
    }

    const McUser32Api& user32 = McUser32Ext();
    if (user32.set_process_dpi_aware != nullptr)
    {
        user32.set_process_dpi_aware();             // Vista / Windows 7
    }
}

//****************************************************************************
//	Method:		InitSingleInstance
//	FullName:	InitSingleInstance
//	author:
//	date:		2026/11/06
//	Function:   用命名互斥体保证同一会话只运行一个 McFence 实例；支持通过 MCFENCE_INSTANCE 环境变量区分开发/测试用的独立实例。
//
//	Returns:	bool    本实例可继续运行返回 true；已有实例在运行或创建互斥体失败返回 false
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
static bool InitSingleInstance()
{
    std::wstring strName(MCFENCE_MUTEX_NAME);

    wchar_t szInstance[128];
    DWORD dwLength = GetEnvironmentVariableW(L"MCFENCE_INSTANCE", szInstance, 128);
    if (dwLength > 0 && dwLength < 128)
    {
        strName += L".";
        strName += szInstance;                      // 开发/测试用的独立实例
    }

    g_hInstanceMutex = CreateMutexW(nullptr, TRUE, strName.c_str());
    if (g_hInstanceMutex == nullptr)
    {
        McLogLastError(L"CreateMutexW");
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        McLog(L"已有实例在运行(Mutex)，本次重复启动直接退出");
        return false;
    }
    return true;
}

//****************************************************************************
//	Method:		LoadFencesAndShow
//	FullName:	LoadFencesAndShow
//	author:
//	date:		2026/11/06
//	Function:   确保数据/条目目录存在，加载全部已保存的围栏并逐个创建窗口；
//              若没有任何已加载围栏且未成功创建窗口，则新建一个名为「我的栅栏」的默认围栏。
//
//	Returns:	int    成功创建的围栏窗口数量
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
static int LoadFencesAndShow()
{
    McEnsureDirectory(McDataRoot());
    McEnsureDirectory(McItemsRoot());

    std::vector<McFenceInfo> fences;
    std::vector<std::wstring>  errors;
    const int nLoaded = McStoreLoadAll(fences, errors);

    int nViews = 0;
    for (size_t i = 0; i < fences.size(); ++i)
    {
        if (McFenceWnd::Create(fences[i]) != nullptr)
        {
            ++nViews;
        }
    }

    if (nLoaded == 0 && nViews == 0)
    {
        McFenceInfo created = McStoreCreateFence(McLoc(L"My Fence", L"我的栅栏"));
        if (McFenceWnd::Create(created) != nullptr)
        {
            ++nViews;
        }
    }

    McLogFormat(L"已载入 %d 个围栏（%u 个失败）", nLoaded, static_cast<unsigned>(errors.size()));
    return nViews;
}

//****************************************************************************
//	Method:		RunVerificationSwitches
//	FullName:	RunVerificationSwitches
//	author:
//	date:		2026/11/06
//	Function:   检查开发用环境变量开关：MCFENCE_SELFTEST 命中时重写所有围栏文件，MCFENCE_DUMP_SURFACE 命中时把所有围栏的渲染表面导出到指定路径。
//
//	Returns:	int    命中任一验证开关返回 0（调用方应直接退出），否则返回 -1
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
static int RunVerificationSwitches()
{
    wchar_t szSelfTest[8];
    if (GetEnvironmentVariableW(L"MCFENCE_SELFTEST", szSelfTest, 8) > 0)
    {
        McFenceWnd::McRewriteAllFences();
        return 0;
    }

    wchar_t szDump[MAX_PATH];
    if (GetEnvironmentVariableW(L"MCFENCE_DUMP_SURFACE", szDump, MAX_PATH) > 0)
    {
        const bool bOk = McFenceWnd::McDumpAllSurfaces(szDump);
        McLogFormat(L"dump surface (%s)", bOk ? L"ok" : L"failed");
        return 0;
    }

    return -1;
}

//****************************************************************************
//	Method:		McFenceShutdown
//	FullName:	McFenceShutdown
//	author:
//	date:		2026/11/06
//	Function:   释放进程级资源：关闭所有围栏窗口、停止图标缓存后台线程、销毁隐藏消息窗口、释放单实例互斥体、关闭 GDI+ 和 OLE。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
static void McFenceShutdown()
{
    McDesktopHookRemove();
    if (g_bHotkeyRegistered)
    {
        UnregisterHotKey(g_hAppWindow, MCFENCE_HOTKEY_ID);
        g_bHotkeyRegistered = false;
    }
    RemoveTrayIcon();
    McFenceWnd::McCloseAll();
    McIconCache::Instance().Shutdown();

    if (g_hAppWindow != nullptr)
    {
        DestroyWindow(g_hAppWindow);
        g_hAppWindow = nullptr;
    }
    if (g_hInstanceMutex != nullptr)
    {
        ReleaseMutex(g_hInstanceMutex);
        CloseHandle(g_hInstanceMutex);
        g_hInstanceMutex = nullptr;
    }
    if (g_gdiplusToken != 0)
    {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
    OleUninitialize();
    McLog(L"退出");
}

//****************************************************************************
//	Method:		AddTrayIcon
//	FullName:	AddTrayIcon
//	author:
//	date:		2026/11/07
//	Function:   在通知区域添加 McFence 图标（单击右键出菜单，双击显示 / 隐藏全部栅栏）。
//
//	Returns:	bool    添加成功返回 true
//	Parameter:	无
//
//              用 NOTIFYICONDATA_V2_SIZE：XP 认得这个大小，更新的系统也兼容
//
//	changelog:  New
//****************************************************************************
static bool AddTrayIcon()
{
    if (g_hAppWindow == nullptr || g_bTrayAdded)
    {
        return g_bTrayAdded;
    }
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
    nid.hWnd = g_hAppWindow;
    nid.uID = MCFENCE_TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = MCFENCE_WM_TRAY;
    nid.hIcon = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_SMALL), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXSMICON),
                                              GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    lstrcpynW(nid.szTip, McLoc(L"McFence Desktop Fences", L"McFence 桌面栅栏"), ARRAYSIZE(nid.szTip));
    g_bTrayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (!g_bTrayAdded)
    {
        McLog(L"Shell_NotifyIcon(NIM_ADD) 失败（任务栏可能还没就绪，等 TaskbarCreated）");
    }
    return g_bTrayAdded;
}

static void RemoveTrayIcon()
{
    if (!g_bTrayAdded)
    {
        return;
    }
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
    nid.hWnd = g_hAppWindow;
    nid.uID = MCFENCE_TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_bTrayAdded = false;
}

//****************************************************************************
//	Method:		ShowTrayMenu
//	FullName:	ShowTrayMenu
//	author:
//	date:		2026/11/07
//	Function:   托盘右键菜单：显示 / 隐藏全部栅栏、新建栅栏、新建文件夹映射栅栏、开机启动、选项、
//              打开数据文件夹、关于、退出。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
static void ShowTrayMenu()
{
    HMENU hMenu = CreatePopupMenu();
    if (hMenu == nullptr)
    {
        return;
    }
    const McSettings& settings = McSettingsGet();
    std::wstring strToggle = McFenceWnd::McAllHidden() ? McLoc(L"&Show All Fences", L"显示全部栅栏(&S)")
                                                        : McLoc(L"&Hide All Fences", L"隐藏全部栅栏(&H)");
    const std::wstring strHotkey = McHotkeyText(settings.hotkey_modifiers, settings.hotkey_key);
    if (!strHotkey.empty())
    {
        strToggle += L"\t" + strHotkey;
    }
    AppendMenuW(hMenu, MF_STRING, IDM_MCFENCE_TOGGLE_ALL, strToggle.c_str());
    SetMenuDefaultItem(hMenu, IDM_MCFENCE_TOGGLE_ALL, FALSE);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_MCFENCE_NEW_FENCE, McLoc(L"&New Fence", L"新建栅栏(&N)"));
    AppendMenuW(hMenu, MF_STRING, IDM_MCFENCE_NEW_MAPPED,
                McLoc(L"New Folder &Portal Fence...", L"新建文件夹映射栅栏(&P)..."));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING | (McAutostartEnabled() ? MF_CHECKED : MF_UNCHECKED),
                IDM_MCFENCE_AUTOSTART, McLoc(L"Start with Windows(&R)", L"开机启动(&R)"));
    // 语言是全局设置，放在托盘菜单里（openFrence 同样如此），切换后菜单与对话框立即生效。
    HMENU hLangMenu = CreatePopupMenu();
    if (hLangMenu != nullptr)
    {
        const int nLanguage = settings.language;
        AppendMenuW(hLangMenu, MF_STRING | (nLanguage == MCFENCE_LANG_EN ? MF_CHECKED : MF_UNCHECKED),
                    IDM_MCFENCE_LANG_EN, L"English");
        AppendMenuW(hLangMenu, MF_STRING | (nLanguage == MCFENCE_LANG_ZH ? MF_CHECKED : MF_UNCHECKED),
                    IDM_MCFENCE_LANG_ZH, L"简体中文");
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hLangMenu), McLoc(L"&Language", L"语言(&L)"));
    }
    AppendMenuW(hMenu, MF_STRING, IDM_MCFENCE_SETTINGS, McLoc(L"Op&tions...", L"选项(&T)..."));
    AppendMenuW(hMenu, MF_STRING, IDM_MCFENCE_OPEN_MAPPED, McLoc(L"Open &Data Folder", L"打开数据文件夹(&D)"));
    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, McLoc(L"&About McFence...", L"关于 McFence(&A)..."));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, McLoc(L"E&xit", L"退出(&X)"));

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hAppWindow);
    const UINT uCommand = TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                           pt.x, pt.y, g_hAppWindow, nullptr);
    PostMessageW(g_hAppWindow, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    switch (uCommand)
    {
    case IDM_MCFENCE_TOGGLE_ALL:
        ToggleAllFences();
        break;
    case IDM_MCFENCE_NEW_FENCE:
        McFenceCreateFence(nullptr, false);
        break;
    case IDM_MCFENCE_NEW_MAPPED:
        McFenceCreateFence(nullptr, true);
        break;
    case IDM_MCFENCE_AUTOSTART:
        McAutostartSet(!McAutostartEnabled());
        break;
    case IDM_MCFENCE_SETTINGS:
        McFenceShowSettings(nullptr);
        break;
    case IDM_MCFENCE_LANG_EN:
        McFenceSetLanguage(MCFENCE_LANG_EN);
        break;
    case IDM_MCFENCE_LANG_ZH:
        McFenceSetLanguage(MCFENCE_LANG_ZH);
        break;
    case IDM_MCFENCE_OPEN_MAPPED:
        McOpenPathAsync(McDataRoot());
        break;
    case IDM_ABOUT:
        McFenceShowAbout(nullptr);
        break;
    case IDM_EXIT:
        // 关掉所有围栏后最后一个围栏的 WM_DESTROY 会 PostQuitMessage；保险起见这里也发一次。
        McFenceWnd::McCloseAll();
        PostQuitMessage(0);
        break;
    default:
        break;
    }
}

//****************************************************************************
//	Method:		ToggleAllFences
//	FullName:	ToggleAllFences
//	author:
//	date:		2026/11/07
//	Function:   显示 / 隐藏全部栅栏（托盘双击、全局热键、双击桌面空白处共用）。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
static void ToggleAllFences()
{
    McFenceWnd::McSetAllHidden(!McFenceWnd::McAllHidden());
}

//****************************************************************************
//	Method:		ApplySettings
//	FullName:	ApplySettings
//	author:
//	date:		2026/11/07
//	Function:   按当前选项（重新）注册全局热键、安装 / 卸载桌面双击钩子、挂接 / 摘离围栏所有者。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
static void ApplySettings()
{
    const McSettings& settings = McSettingsGet();

    if (g_bHotkeyRegistered)
    {
        UnregisterHotKey(g_hAppWindow, MCFENCE_HOTKEY_ID);
        g_bHotkeyRegistered = false;
    }
    if (settings.hotkey_key != 0 && g_hAppWindow != nullptr)
    {
        g_bHotkeyRegistered = RegisterHotKey(g_hAppWindow, MCFENCE_HOTKEY_ID, settings.hotkey_modifiers,
                                             settings.hotkey_key) != FALSE;
        if (!g_bHotkeyRegistered)
        {
            McLogFormat(L"热键 %s 注册失败（可能已被其他程序占用）",
                        McHotkeyText(settings.hotkey_modifiers, settings.hotkey_key).c_str());
        }
    }

    if (settings.double_click_desktop)
    {
        McDesktopHookInstall(g_hAppWindow);
    }
    else
    {
        McDesktopHookRemove();
    }

    McFenceWnd::McApplyDesktopOwner();
}

//****************************************************************************
//	Method:		McFenceShowSettings
//	FullName:	McFenceShowSettings
//	author:
//	date:		2026/11/07
//	Function:   弹出选项对话框；确定后写开机启动、保存 settings.ini 并立即应用。
//
//	Returns:	void
//	Parameter:	IN  HWND hWndOwner    对话框所有者（托盘调用时为 NULL）
//
//	changelog:  New
//****************************************************************************
void McFenceShowSettings(HWND hWndOwner)
{
    McSettings settings = McSettingsGet();
    bool bAutostart = McAutostartEnabled();
    const bool bWasAutostart = bAutostart;
    if (!McShowSettingsDialog(hWndOwner, settings, bAutostart))
    {
        return;
    }
    if (bAutostart != bWasAutostart)
    {
        McAutostartSet(bAutostart);
    }
    McSettingsGet() = settings;
    McSettingsSave();
    ApplySettings();
    if (settings.hotkey_key != 0 && !g_bHotkeyRegistered)
    {
        const std::wstring strText = McFormat(McLoc(L"The hotkey %s is already used by another program. Please choose a different combination.",
                                                    L"热键 %s 已被其他程序占用，请换一个组合。"),
                                              McHotkeyText(settings.hotkey_modifiers, settings.hotkey_key).c_str());
        MessageBoxW(hWndOwner, strText.c_str(), McLoc(L"McFence Options", L"McFence 选项"), MB_OK | MB_ICONWARNING);
    }
}

//****************************************************************************
//	Method:		McFenceSetLanguage
//	FullName:	McFenceSetLanguage
//	author:
//	date:		2026/11/07
//	Function:   切换界面语言并写入 settings.ini；托盘提示立即更新，菜单与对话框下次打开时使用新语言。
//
//	Returns:	void
//	Parameter:	IN  int nLanguage    McLanguage
//
//              已有栅栏的名称是用户数据，不随语言改名（openFrence 同样只影响新建时的默认名）
//
//	changelog:  New
//****************************************************************************
void McFenceSetLanguage(int nLanguage)
{
    McSettings& settings = McSettingsGet();
    if (settings.language == nLanguage)
    {
        return;
    }
    settings.language = nLanguage;
    McSettingsSave();
    if (g_bTrayAdded)
    {
        NOTIFYICONDATAW nid;
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
        nid.hWnd = g_hAppWindow;
        nid.uID = MCFENCE_TRAY_ID;
        nid.uFlags = NIF_TIP;
        lstrcpynW(nid.szTip, McLoc(L"McFence Desktop Fences", L"McFence 桌面栅栏"), ARRAYSIZE(nid.szTip));
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

//****************************************************************************
//	Method:		McFenceCreateFence
//	FullName:	McFenceCreateFence
//	author:
//	date:		2026/11/07
//	Function:   新建一个栅栏；bMapped 时先选文件夹，栅栏名取文件夹名。新栅栏若被隐藏也会随全局状态一起隐藏。
//
//	Returns:	void
//	Parameter:	IN  HWND hWndOwner    文件夹选择框的所有者（可为 NULL）
//              IN  bool bMapped    是否创建文件夹映射栅栏
//
//	changelog:  New
//****************************************************************************
void McFenceCreateFence(HWND hWndOwner, bool bMapped)
{
    if (McFenceWnd::McAllHidden())
    {
        McFenceWnd::McSetAllHidden(false);          // 否则新栅栏建出来也看不见
    }
    if (!bMapped)
    {
        McFenceWnd::Create(McStoreCreateFence(McLoc(L"New Fence", L"新建栅栏")));
        return;
    }
    std::wstring strFolder;
    if (!McPickFolder(hWndOwner, McLoc(L"Choose the folder to show in the fence:", L"选择要在栅栏中显示的文件夹："), strFolder))
    {
        return;
    }
    std::wstring strName = McFileName(strFolder);
    if (strName.empty())
    {
        strName = strFolder;                        // 盘符根目录 "D:\"
    }
    McFenceWnd::Create(McStoreCreateFence(strName, strFolder));
}
