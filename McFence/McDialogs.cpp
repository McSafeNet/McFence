#include "McDialogs.h"
#include "McLog.h"
#include "McUtil.h"
#include "McSettings.h"
#include "McResource.h"

#include <commctrl.h>

#include <vector>


namespace
{

// Control ids live in Resource.h. OK / Cancel use IDOK / IDCANCEL so that
// IsDialogMessage turns Enter and Esc into the matching button.

const wchar_t* const k_height_class = MCFENCE_HEIGHT_CLASS;

struct McDialogState
{
    bool accepted;
    HWND edit;
    HWND track;
    HWND label;
    HFONT font;  // created per dialog, deleted after the modal loop
    std::wstring text;
    int value;
    int dpi;
    McSettings settings;  // 选项对话框的工作副本
    bool autostart;
    HWND checks[4];
    HWND hotkey;
};

//****************************************************************************
//	Method:		MessageFont
//	FullName:	MessageFont
//	author:
//	date:		2026/11/06
//	Function:   根据系统消息框字体（NONCLIENTMETRICS）创建一个按指定 DPI 缩放的字体；
//              获取系统参数失败时返回默认 GUI 字体（stock object）。
//
//	Returns:	HFONT    创建的字体句柄，或失败时返回的默认 GUI stock 字体
//	Parameter:	IN  int dpi    目标 DPI，用于缩放字体高度（96 为基准不缩放）
//
//              当返回值是 GetStockObject(DEFAULT_GUI_FONT) 时调用方不应 DeleteObject
//
//	changelog:  New
//****************************************************************************
HFONT MessageFont(int dpi)
{
    NONCLIENTMETRICSW metrics;
    ZeroMemory(&metrics, sizeof(metrics));
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        LOGFONTW log_font = metrics.lfMessageFont;
        if (dpi > 0 && dpi != 96)
        {
            log_font.lfHeight = MulDiv(log_font.lfHeight, dpi, 96);
        }
        return CreateFontIndirectW(&log_font);
    }
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

//****************************************************************************
//	Method:		WindowDpi
//	FullName:	WindowDpi
//	author:
//	date:		2026/11/06
//	Function:   获取窗口所在显示器的 DPI：优先用 GetDpiForWindow，
//              不可用时退化为用窗口的设备上下文读取 LOGPIXELSX，仍失败则返回 96。
//
//	Returns:	int    窗口的 DPI 值
//	Parameter:	IN  HWND hwnd    要查询 DPI 的窗口，可为 NULL（此时使用屏幕 DC）
//
//	changelog:  New
//****************************************************************************
int WindowDpi(HWND hwnd)
{
    const McUser32Api& api = McUser32Ext();
    if (api.get_dpi_for_window && hwnd)
    {
        return static_cast<int>(api.get_dpi_for_window(hwnd));
    }
    HDC dc = GetDC(hwnd ? hwnd : NULL);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc)
    {
        ReleaseDC(hwnd ? hwnd : NULL, dc);
    }
    return dpi > 0 ? dpi : 96;
}

//****************************************************************************
//	Method:		CenterOver
//	FullName:	CenterOver
//	author:
//	date:		2026/11/06
//	Function:   把 hwnd 相对 owner（可见时）或当前工作区居中定位。
//
//	Returns:	void
//	Parameter:	IN  HWND hwnd    要定位的窗口
//              IN  HWND owner    作为居中参照的所有者窗口，可为 NULL
//
//	changelog:  New
//****************************************************************************
void CenterOver(HWND hwnd, HWND owner)
{
    RECT self;
    GetWindowRect(hwnd, &self);
    RECT area;
    if (owner && IsWindowVisible(owner))
    {
        GetWindowRect(owner, &area);
    }
    else
    {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    }
    const int width = self.right - self.left;
    const int height = self.bottom - self.top;
    const int x = area.left + ((area.right - area.left) - width) / 2;
    const int y = area.top + ((area.bottom - area.top) - height) / 2;
    SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);
}

//****************************************************************************
//	Method:		RunModal
//	FullName:	RunModal
//	author:
//	date:		2026/11/06
//	Function:   运行模态消息循环直到 hwnd 窗口被销毁；循环期间禁用 owner 窗口，结束后重新启用并前置。
//
//	Returns:	void
//	Parameter:	IN  HWND hwnd    要模态运行的窗口
//              IN  HWND owner    需要临时禁用的所有者窗口，可为 NULL
//
//              用 IsDialogMessageW 处理 Tab/Enter/Esc 等对话框导航键
//
//	changelog:  New
//****************************************************************************
// Runs the window modally; returns when the window stops existing.
void RunModal(HWND hwnd, HWND owner)
{
    if (owner)
    {
        EnableWindow(owner, FALSE);
    }
    MSG msg;
    while (IsWindow(hwnd))
    {
        if (GetMessageW(&msg, NULL, 0, 0) <= 0)
        {
            PostQuitMessage(0);
            break;
        }
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner && IsWindow(owner))
    {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
}

//****************************************************************************
//	Method:		CreateChild
//	FullName:	CreateChild
//	author:
//	date:		2026/11/06
//	Function:   创建一个子控件窗口（WS_CHILD|WS_VISIBLE），创建成功时为其设置字体。
//
//	Returns:	HWND    新建子控件句柄，失败为 NULL
//	Parameter:	IN  HWND parent    父窗口
//              IN  const wchar_t* cls    控件类名
//              IN  const wchar_t* text    初始文本
//              IN  DWORD style    额外窗口样式
//              IN  DWORD ex_style    扩展窗口样式
//              IN  int x    左边位置
//              IN  int y    顶部位置
//              IN  int cx    宽度
//              IN  int cy    高度
//              IN  int id    控件 ID
//              IN  HFONT font    要应用的字体，可为 NULL
//
//	changelog:  New
//****************************************************************************
HWND CreateChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex_style,
                  int x, int y, int cx, int cy, int id, HFONT font)
{
    HWND child = CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy,
                                 parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(NULL), NULL);
    if (child && font)
    {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return child;
}

McDialogState* StateOf(HWND hwnd)
{
    return reinterpret_cast<McDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// -- title height dialog -----------------------------------------------------

//****************************************************************************
//	Method:		UpdateHeightLabel
//	FullName:	UpdateHeightLabel
//	author:
//	date:		2026/11/06
//	Function:   把当前高度值格式化为“%d px”显示到高度对话框的标签控件上。
//
//	Returns:	void
//	Parameter:	IN  McDialogState* state    对话框状态，读取 value 和 label 句柄
//
//	changelog:  New
//****************************************************************************
void UpdateHeightLabel(McDialogState* state)
{
    if (state && state->label)
    {
        const std::wstring text = McFormat(L"%d px", state->value);
        SetWindowTextW(state->label, text.c_str());
    }
}

//****************************************************************************
//	Method:		HeightProc
//	FullName:	HeightProc
//	author:
//	date:		2026/11/06
//	Function:   标题栏高度对话框窗口过程：WM_CREATE 创建滑块（范围 20..100）、数值标签、
//              恢复默认与确定/取消按钮；WM_HSCROLL 把滑块位置同步到 state->value 并刷新标签；
//              WM_COMMAND 处理 IDOK（标记 accepted）、IDCANCEL、IDC_MCFENCE_RESTORE（恢复默认值 35）；
//              WM_CLOSE 销毁窗口。
//
//	Returns:	LRESULT    消息处理结果
//	Parameter:	IN  HWND hwnd    对话框窗口句柄
//              IN  UINT message    消息编号
//              IN  WPARAM wparam    消息参数
//              IN  LPARAM lparam    消息参数，WM_CREATE 时携带 McDialogState 指针
//
//	changelog:  New
//****************************************************************************
LRESULT CALLBACK HeightProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    McDialogState* state = StateOf(hwnd);
    switch (message)
    {
        case WM_CREATE:
        {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            state = reinterpret_cast<McDialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            const int dpi = state->dpi;
            HFONT font = MessageFont(dpi);
            state->font = font;
            const int pad = MulDiv(12, dpi, 96);
            CreateChild(hwnd, L"STATIC", McLoc(L"Title bar height", L"标题栏高度"), SS_LEFT, 0, pad, pad, MulDiv(200, dpi, 96),
                         MulDiv(18, dpi, 96), IDC_MCFENCE_LABEL, font);
            state->track = CreateChild(hwnd, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS, 0, pad,
                                        pad + MulDiv(22, dpi, 96), MulDiv(280, dpi, 96),
                                        MulDiv(34, dpi, 96), IDC_MCFENCE_TRACK, font);
            state->label = CreateChild(hwnd, L"STATIC", L"", SS_LEFT, 0, pad,
                                        pad + MulDiv(62, dpi, 96), MulDiv(80, dpi, 96),
                                        MulDiv(18, dpi, 96), IDC_MCFENCE_HEIGHT_TEXT, font);
            CreateChild(hwnd, L"BUTTON", McLoc(L"Default", L"恢复默认"), BS_PUSHBUTTON | WS_TABSTOP, 0, pad,
                         pad + MulDiv(86, dpi, 96), MulDiv(88, dpi, 96), MulDiv(26, dpi, 96),
                         IDC_MCFENCE_RESTORE, font);
            CreateChild(hwnd, L"BUTTON", McLoc(L"OK", L"确定"), BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
                         MulDiv(120, dpi, 96), pad + MulDiv(86, dpi, 96), MulDiv(72, dpi, 96),
                         MulDiv(26, dpi, 96), IDOK, font);
            CreateChild(hwnd, L"BUTTON", McLoc(L"Cancel", L"取消"), BS_PUSHBUTTON | WS_TABSTOP, 0,
                         MulDiv(200, dpi, 96), pad + MulDiv(86, dpi, 96), MulDiv(72, dpi, 96),
                         MulDiv(26, dpi, 96), IDCANCEL, font);
            if (state->track)
            {
                SendMessageW(state->track, TBM_SETRANGE, TRUE, MAKELPARAM(20, 100));
                SendMessageW(state->track, TBM_SETTICFREQ, 10, 0);
                SendMessageW(state->track, TBM_SETPOS, TRUE, state->value);
            }
            UpdateHeightLabel(state);
            return 0;
        }
        case WM_HSCROLL:
            if (state && state->track)
            {
                state->value = static_cast<int>(SendMessageW(state->track, TBM_GETPOS, 0, 0));
                UpdateHeightLabel(state);
            }
            return 0;
        case WM_COMMAND:
            if (!state)
            {
                break;
            }
            switch (LOWORD(wparam))
            {
                case IDOK:
                    state->accepted = true;
                    DestroyWindow(hwnd);
                    return 0;
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    return 0;
                case IDC_MCFENCE_RESTORE:
                    if (state->track)
                    {
                        SendMessageW(state->track, TBM_SETPOS, TRUE, 35);
                    }
                    state->value = 35;
                    UpdateHeightLabel(state);
                    return 0;
                default:
                    break;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

//****************************************************************************
//	Method:		RegisterDialogClass
//	FullName:	RegisterDialogClass
//	author:
//	date:		2026/11/06
//	Function:   注册自定义对话框窗口类；若类已存在（ERROR_CLASS_ALREADY_EXISTS）也视为成功。
//
//	Returns:	ATOM    非 0 表示注册成功或类已存在
//	Parameter:	IN  const wchar_t* name    窗口类名
//              IN  WNDPROC proc    窗口过程
//
//	changelog:  New
//****************************************************************************
ATOM RegisterDialogClass(const wchar_t* name, WNDPROC proc)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = name;
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    const ATOM atom = RegisterClassExW(&wc);
    // The second time a dialog opens its class already exists; that is fine.
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

//****************************************************************************
//	Method:		RunDialog
//	FullName:	RunDialog
//	author:
//	date:		2026/11/06
//	Function:   按指定客户区尺寸（随 DPI 缩放）创建弹出式对话框窗口并模态运行，结束后释放对话框期间创建的字体。
//
//	Returns:	bool    窗口创建成功返回 true，失败返回 false
//	Parameter:	IN  HWND owner    所有者窗口
//              IN  const wchar_t* cls    对话框窗口类名
//              IN  const wchar_t* title    标题栏文本
//              IN  int client_width    客户区宽度（逻辑像素）
//              IN  int client_height    客户区高度（逻辑像素）
//              IN OUTMcDialogState& state    对话框状态：提供 dpi 作为输入，窗口过程运行期间读写它，
//                                            结束后其 font 字段被清空
//
//              用 AdjustWindowRectEx 把标题栏和边框开销加到客户区尺寸上，避免高 DPI 下按钮被裁剪
//
//	changelog:  New
//****************************************************************************
// Creates the dialog with a |client_width| x |client_height| client area (logical
// pixels), runs it modally and releases the font it created.
bool RunDialog(HWND owner, const wchar_t* cls, const wchar_t* title, int client_width,
               int client_height, McDialogState& state)
{
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD ex_style = WS_EX_DLGMODALFRAME;
    RECT frame = {0, 0, MulDiv(client_width, state.dpi, 96), MulDiv(client_height, state.dpi, 96)};
    // The caption and borders come on top of the client area, otherwise the
    // bottom row of buttons is clipped at 125 % and above.
    AdjustWindowRectEx(&frame, style, FALSE, ex_style);
    HWND hwnd = CreateWindowExW(ex_style, cls, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                                frame.right - frame.left, frame.bottom - frame.top, owner, NULL,
                                GetModuleHandleW(NULL), &state);
    if (!hwnd)
    {
        McLogLastError(L"CreateWindowExW(dialog)");
        return false;
    }
    CenterOver(hwnd, owner);
    ShowWindow(hwnd, SW_SHOW);
    RunModal(hwnd, owner);
    if (state.font && state.font != GetStockObject(DEFAULT_GUI_FONT))
    {
        DeleteObject(state.font);
    }
    state.font = NULL;
    return true;
}

//****************************************************************************
//	Method:		InitState
//	FullName:	InitState
//	author:
//	date:		2026/11/06
//	Function:   把对话框状态结构体重置为默认值，并根据 owner 窗口计算 DPI。
//
//	Returns:	void
//	Parameter:	OUT McDialogState& state    待初始化的对话框状态
//              IN  HWND owner    用于计算 DPI 的所有者窗口
//
//	changelog:  New
//****************************************************************************
void InitState(McDialogState& state, HWND owner)
{
    state.accepted = false;
    state.edit = NULL;
    state.track = NULL;
    state.label = NULL;
    state.font = NULL;
    state.value = 0;
    state.dpi = WindowDpi(owner);
    state.autostart = false;
    state.hotkey = NULL;
    for (int i = 0; i < 4; ++i)
    {
        state.checks[i] = NULL;
    }
}

}  // namespace

//****************************************************************************
//	Method:		McShowHeightDialog
//	FullName:	McShowHeightDialog
//	author:
//	date:		2026/11/06
//	Function:   弹出标题栏高度调整的模态对话框（滑块范围 20-100 px）；
//              用户确定时把新高度写入 new_height。
//
//	Returns:	bool    用户点击确定返回 true，取消或注册/创建失败返回 false
//	Parameter:	IN  HWND owner    所有者窗口
//              IN  int current_height    当前高度，会被夹到 [20,100] 范围内
//              OUT int& new_height    用户确认后的高度值
//
//	changelog:  New
//****************************************************************************
bool McShowHeightDialog(HWND owner, int current_height, int& new_height)
{
    if (!RegisterDialogClass(k_height_class, &HeightProc))
    {
        return false;
    }
    McDialogState state;
    InitState(state, owner);
    state.value = current_height < 20 ? 20 : (current_height > 100 ? 100 : current_height);
    // Content: trackbar 34..68, value 74..92, buttons 98..124, 12 px bottom margin.
    if (!RunDialog(owner, k_height_class, McLoc(L"Title Bar Height", L"标题栏高度"), 304, 136, state))
    {
        return false;
    }
    new_height = state.value;
    return state.accepted;
}


namespace
{

// -- settings dialog -----------------------------------------------------------

const wchar_t* const k_settings_class = MCFENCE_SETTINGS_CLASS;

// 选项对话框里四个复选框的文本（英文 / 中文），顺序与 McDialogState::checks 对应。
const wchar_t* const k_setting_labels[4][2] = {
    {L"Start McFence with Windows", L"开机时自动启动 McFence"},
    {L"Double-click the desktop to show / hide fences", L"双击桌面空白处显示 / 隐藏全部栅栏"},
    {L"Keep fences on \"Show desktop\" (Win+D)", L"“显示桌面”(Win+D) 时保留栅栏"},
    {L"Snap to screen edges and other fences", L"拖动、缩放时吸附屏幕边缘和其他栅栏"},
};

//****************************************************************************
//	Method:		SettingsProc
//	FullName:	SettingsProc
//	author:
//	date:		2026/11/07
//	Function:   选项对话框窗口过程：WM_CREATE 创建四个复选框、热键控件与确定/取消按钮；
//              IDOK 把控件状态写回 state->settings / state->autostart 并标记 accepted。
//
//	Returns:	LRESULT    消息处理结果
//	Parameter:	IN  HWND hwnd    对话框窗口句柄
//              IN  UINT message    消息编号
//              IN  WPARAM wparam    消息参数
//              IN  LPARAM lparam    消息参数，WM_CREATE 时携带 McDialogState 指针
//
//	changelog:  New
//****************************************************************************
LRESULT CALLBACK SettingsProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    McDialogState* state = StateOf(hwnd);
    switch (message)
    {
        case WM_CREATE:
        {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            state = reinterpret_cast<McDialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            const int dpi = state->dpi;
            HFONT font = MessageFont(dpi);
            state->font = font;
            const int pad = MulDiv(12, dpi, 96);
            const bool values[4] = {state->autostart, state->settings.double_click_desktop,
                                    state->settings.keep_on_show_desktop, state->settings.snap};
            for (int i = 0; i < 4; ++i)
            {
                state->checks[i] = CreateChild(hwnd, L"BUTTON", McLoc(k_setting_labels[i][0], k_setting_labels[i][1]),
                                               BS_AUTOCHECKBOX | WS_TABSTOP, 0, pad,
                                               pad + MulDiv(26 * i, dpi, 96), MulDiv(330, dpi, 96),
                                               MulDiv(22, dpi, 96), IDC_MCFENCE_CHECK_FIRST + i, font);
                SendMessageW(state->checks[i], BM_SETCHECK, values[i] ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            const int hotkey_y = pad + MulDiv(26 * 4 + 8, dpi, 96);
            CreateChild(hwnd, L"STATIC", McLoc(L"Show / hide hotkey:", L"显示 / 隐藏全部栅栏的热键："), SS_LEFT, 0, pad,
                        hotkey_y + MulDiv(3, dpi, 96), MulDiv(170, dpi, 96), MulDiv(18, dpi, 96),
                        IDC_MCFENCE_LABEL, font);
            state->hotkey = CreateChild(hwnd, HOTKEY_CLASSW, L"", WS_TABSTOP, WS_EX_CLIENTEDGE,
                                        pad + MulDiv(176, dpi, 96), hotkey_y, MulDiv(154, dpi, 96),
                                        MulDiv(24, dpi, 96), IDC_MCFENCE_HOTKEY, font);
            if (state->hotkey)
            {
                // 热键控件的修饰键位与 MOD_* 不同（HOTKEYF_ALT=4、HOTKEYF_SHIFT=1）。
                WORD flags = 0;
                if (state->settings.hotkey_modifiers & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
                if (state->settings.hotkey_modifiers & MOD_ALT) flags |= HOTKEYF_ALT;
                if (state->settings.hotkey_modifiers & MOD_SHIFT) flags |= HOTKEYF_SHIFT;
                SendMessageW(state->hotkey, HKM_SETHOTKEY,
                             MAKEWORD(state->settings.hotkey_key, flags), 0);
            }
            CreateChild(hwnd, L"STATIC", McLoc(L"(Press Backspace in the hotkey box to clear it)", L"（在热键框里按 Backspace 可清除热键）"), SS_LEFT, 0, pad,
                        hotkey_y + MulDiv(30, dpi, 96), MulDiv(330, dpi, 96), MulDiv(18, dpi, 96),
                        IDC_STATIC, font);
            const int button_y = hotkey_y + MulDiv(58, dpi, 96);
            CreateChild(hwnd, L"BUTTON", McLoc(L"OK", L"确定"), BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
                        MulDiv(178, dpi, 96), button_y, MulDiv(72, dpi, 96), MulDiv(26, dpi, 96), IDOK,
                        font);
            CreateChild(hwnd, L"BUTTON", McLoc(L"Cancel", L"取消"), BS_PUSHBUTTON | WS_TABSTOP, 0, MulDiv(258, dpi, 96),
                        button_y, MulDiv(72, dpi, 96), MulDiv(26, dpi, 96), IDCANCEL, font);
            SetFocus(state->checks[0]);
            return 0;
        }
        case WM_COMMAND:
            if (!state)
            {
                break;
            }
            switch (LOWORD(wparam))
            {
                case IDOK:
                {
                    bool values[4];
                    for (int i = 0; i < 4; ++i)
                    {
                        values[i] = SendMessageW(state->checks[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
                    }
                    state->autostart = values[0];
                    state->settings.double_click_desktop = values[1];
                    state->settings.keep_on_show_desktop = values[2];
                    state->settings.snap = values[3];
                    if (state->hotkey)
                    {
                        const WORD hotkey = static_cast<WORD>(SendMessageW(state->hotkey, HKM_GETHOTKEY, 0, 0));
                        const BYTE flags = HIBYTE(hotkey);
                        UINT modifiers = 0;
                        if (flags & HOTKEYF_CONTROL) modifiers |= MOD_CONTROL;
                        if (flags & HOTKEYF_ALT) modifiers |= MOD_ALT;
                        if (flags & HOTKEYF_SHIFT) modifiers |= MOD_SHIFT;
                        state->settings.hotkey_key = LOBYTE(hotkey);
                        state->settings.hotkey_modifiers = state->settings.hotkey_key ? modifiers : 0;
                    }
                    state->accepted = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    break;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

//****************************************************************************
//	Method:		McShowSettingsDialog
//	FullName:	McShowSettingsDialog
//	author:
//	date:		2026/11/07
//	Function:   弹出选项对话框；用户确定时把结果写回 settings 与 autostart（不负责保存和应用）。
//
//	Returns:	bool    用户点击确定返回 true
//	Parameter:	IN  HWND owner    所有者窗口
//              IN OUTMcSettings& settings    输入当前选项，输出用户的选择
//              IN OUTbool& autostart    输入当前是否开机启动，输出用户的选择
//
//	changelog:  New
//****************************************************************************
bool McShowSettingsDialog(HWND owner, McSettings& settings, bool& autostart)
{
    if (!RegisterDialogClass(k_settings_class, &SettingsProc))
    {
        return false;
    }
    McDialogState state;
    InitState(state, owner);
    state.settings = settings;
    state.autostart = autostart;
    // Content: 4 checks 0..104, hotkey 112..136, hint 142..160, buttons 170..196, 12 px margins.
    if (!RunDialog(owner, k_settings_class, McLoc(L"McFence Options", L"McFence 选项"), 354, 220, state))
    {
        return false;
    }
    if (state.accepted)
    {
        settings = state.settings;
        autostart = state.autostart;
    }
    return state.accepted;
}
