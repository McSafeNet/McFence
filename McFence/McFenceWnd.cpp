#include "McFence.h"
#include "McFenceWnd.h"
#include "McDialogs.h"
#include "McIcons.h"
#include "McLog.h"
#include "McSettings.h"
#include "McShortcut.h"
#include "McStore.h"
#include "McUtil.h"

#include <imm.h>
#include <ole2.h>
#include <oleidl.h>
#include <stdio.h>


namespace
{

const wchar_t* const k_fence_class = MCFENCE_FENCE_CLASS;

enum
{
    timer_save_position = 1,
    timer_save_size = 2,
    timer_folder_reload = 3,
    timer_rename_caret = 4
};

const UINT MCFENCE_FOLDER_DEBOUNCE_MS = 250;  // 一次复制常常触发一串变化通知


const UINT MCFENCE_SAVE_THROTTLE_MS = 4000;  // ThrottledExecution(TimeSpan.FromSeconds(4))

//****************************************************************************
//	Method:		class_lock
//	FullName:	class_lock
//	author:
//	date:		2026/11/06
//	Function:   返回一个进程级静态临界区对象（首次调用时惰性初始化），
//              用于保护围栏窗口类注册等操作的互斥。
//
//	Returns:	CRITICAL_SECTION&    对静态临界区对象的引用，供调用方加锁/解锁使用。
//	Parameter:	无
//
//              惰性初始化标志本身不是线程安全的，依赖首次调用发生在安全时机
//
//	changelog:  New
//****************************************************************************
CRITICAL_SECTION& class_lock()
{
    static CRITICAL_SECTION cs;
    static bool ready = false;
    if (!ready)
    {
        InitializeCriticalSection(&cs);
        ready = true;
    }
    return cs;
}

//****************************************************************************
//	Method:		ensure_fence_class
//	FullName:	ensure_fence_class
//	author:
//	date:		2026/11/06
//	Function:   确保围栏窗口类已向系统注册，如果尚未注册则在临界区保护下调用 RegisterClassExW 注册一次。
//
//	Returns:	ATOM    已注册窗口类的 ATOM 值；注册失败时为 0。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
ATOM ensure_fence_class()
{
    static ATOM atom = 0;
    EnterCriticalSection(&class_lock());
    if (!atom)
    {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &McFenceWnd::McFenceWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = k_fence_class;
        atom = RegisterClassExW(&wc);
    }
    LeaveCriticalSection(&class_lock());
    return atom;
}

//****************************************************************************
//	Method:		query_dropped_files
//	FullName:	query_dropped_files
//	author:
//	date:		2026/11/06
//	Function:   从 OLE 拖放数据对象中提取 CF_HDROP 格式的文件路径列表。
//
//	Returns:	std::vector<std::wstring>    拖放数据中包含的文件路径集合；获取失败或无数据时为空。
//	Parameter:	IN  IDataObject* data    OLE 拖放操作传入的数据对象
//
//	changelog:  New
//****************************************************************************
std::vector<std::wstring> query_dropped_files(IDataObject* data)
{
    std::vector<std::wstring> result;
    if (!data)
    {
        return result;
    }
    FORMATETC format = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium;
    ZeroMemory(&medium, sizeof(medium));
    if (FAILED(data->GetData(&format, &medium)))
    {
        return result;
    }
    HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
    if (drop)
    {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < count; ++i)
        {
            const UINT length = DragQueryFileW(drop, i, NULL, 0);
            std::vector<wchar_t> buffer(length + 1, L'\0');
            if (DragQueryFileW(drop, i, &buffer[0], length + 1) > 0)
            {
                result.push_back(std::wstring(&buffer[0]));
            }
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return result;
}

//****************************************************************************
//	Method:		data_has_files
//	FullName:	data_has_files
//	author:
//	date:		2026/11/06
//	Function:   检查给定的 OLE 数据对象是否包含 CF_HDROP（文件列表）格式的数据。
//
//	Returns:	bool    包含文件列表数据时为 true，否则为 false。
//	Parameter:	IN  IDataObject* data    待检测的 OLE 数据对象
//
//	changelog:  New
//****************************************************************************
bool data_has_files(IDataObject* data)
{
    if (!data)
    {
        return false;
    }
    FORMATETC format = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return data->QueryGetData(&format) == S_OK;
}

//****************************************************************************
//	Method:		drop_moves
//	FullName:	drop_moves
//	author:
//	date:		2026/11/07
//	Function:   判断外部拖入是移动还是复制：默认移动，按住 Ctrl 复制；来源不允许移动时只能复制。
//
//	Returns:	bool    移动返回 true，复制返回 false。
//	Parameter:	IN  DWORD keys    拖放时的按键状态（MK_*）
//              IN  DWORD allowed    拖放来源允许的效果（DROPEFFECT_*）
//
//	changelog:  New
//****************************************************************************
bool drop_moves(DWORD keys, DWORD allowed)
{
    if (!(allowed & DROPEFFECT_MOVE))
    {
        return false;
    }
    return (keys & MK_CONTROL) == 0 || !(allowed & DROPEFFECT_COPY);
}

}  // namespace

std::vector<McFenceWnd*> McFenceWnd::s_registry;
bool McFenceWnd::s_hidden = false;
McFenceWnd* McFenceWnd::s_outgoing_source = NULL;
std::vector<std::wstring> McFenceWnd::s_outgoing_paths;
bool McFenceWnd::s_outgoing_handled = false;
HWND McFenceWnd::s_app_window = NULL;
int McFenceWnd::s_icon_size = 48;

struct McFenceWnd::McDropTarget : public IDropTarget
{
    McFenceWnd* owner;
    LONG references;

    explicit McDropTarget(McFenceWnd* view) : owner(view), references(1) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** out)
    {
        if (!out)
        {
            return E_POINTER;
        }
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDropTarget))
        {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&references));
    }
    STDMETHODIMP_(ULONG) Release()
    {
        const LONG left = InterlockedDecrement(&references);
        if (left == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(left);
    }

    // IDropTarget
    STDMETHODIMP DragEnter(IDataObject* data, DWORD keys, POINTL point, DWORD* effect)
    {
        return DragOver(keys, point, effect) == S_OK && data_has_files(data) ? S_OK : S_OK;
    }
    STDMETHODIMP DragOver(DWORD keys, POINTL point, DWORD* effect)
    {
        if (!effect)
        {
            return E_POINTER;
        }
        const DWORD allowed = *effect;
        *effect = DROPEFFECT_NONE;
        if (!owner || !owner->m_hwnd)
        {
            return S_OK;
        }
        const bool internal = s_outgoing_source != NULL;
        if (owner->m_info.locked || (internal && s_outgoing_source == owner && owner->IsMapped()))
        {
            owner->SetDropIndex(-1);
            return S_OK;
        }
        POINT screen;
        screen.x = point.x;
        screen.y = point.y;
        POINT client = screen;
        ScreenToClient(owner->m_hwnd, &client);
        owner->SetDropIndex(owner->IsMapped() ? -1 : owner->DropInsertIndex(client));
        if (internal)
        {
            *effect = DROPEFFECT_MOVE;  // 从另一个围栏拖回来：按围栏间移动处理
        }
        else
        {
            // 拖进围栏默认移动（和 openFence 一致）；按住 Ctrl 或来源不允许移动时复制。
            *effect = drop_moves(keys, allowed) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        }
        return S_OK;
    }
    STDMETHODIMP DragLeave()
    {
        if (owner)
        {
            owner->SetDropIndex(-1);
        }
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject* data, DWORD keys, POINTL point, DWORD* effect)
    {
        const DWORD allowed = effect ? *effect : DROPEFFECT_COPY;
        if (effect)
        {
            *effect = DROPEFFECT_NONE;
        }
        if (!owner || !owner->m_hwnd)
        {
            return S_OK;
        }
        POINT screen;
        screen.x = point.x;
        screen.y = point.y;
        POINT client = screen;
        ScreenToClient(owner->m_hwnd, &client);
        const int index = owner->DropInsertIndex(client);
        owner->SetDropIndex(-1);
        if (owner->m_info.locked)
        {
            return S_OK;
        }
        if (s_outgoing_source)
        {
            if (s_outgoing_source == owner && owner->IsMapped())
            {
                return S_OK;
            }
            // 发起方在 McDoDragOut 返回后看到 handled，就不会再按“拖到外部”处理。
            s_outgoing_handled = true;
            const std::vector<std::wstring> paths = s_outgoing_paths;
            MoveFenceItems(s_outgoing_source, owner, paths, index);
            return S_OK;
        }
        std::vector<std::wstring> paths = query_dropped_files(data);
        if (paths.empty())
        {
            return S_OK;
        }
        const bool move = drop_moves(keys, allowed);
        owner->AcceptFileDrop(paths, index, move);
        if (effect)
        {
            *effect = move ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        }
        return S_OK;
    }
};

//****************************************************************************
//	Method:		McFenceWnd
//	FullName:	McFenceWnd::McFenceWnd
//	author:
//	date:		2026/11/06
//	Function:   构造函数，用传入的围栏信息初始化窗口句柄、度量值与各状态标志（此时尚未创建实际 HWND）。
//
//	Returns:	无
//	Parameter:	IN  const McFenceInfo& info    该围栏的持久化信息（名称、位置、大小、文件列表等）
//
//	changelog:  New
//****************************************************************************
McFenceWnd::McFenceWnd(const McFenceInfo& info)
    : m_hwnd(NULL),
      m_info(info),
      m_dpi(96),
      m_scale(1.0f),
      m_item_width(60),
      m_item_padding(10),
      m_text_height(22),
      m_icon_box(30),
      m_title_offset(4),
      m_title_height(35),
      m_logical_title_height(35),
      m_title_font(NULL),
      m_icon_font(NULL),
      m_drag_start(),
      m_marquee(false),
      m_marquee_toggle(false),
      m_collapse_on_up(false),
      m_drag_in_progress(false),
      m_drop_index(-1),
      m_is_minified(false),
      m_allow_close(false),
      m_initialized(false),
      m_tracking_mouse(false),
      m_tracking_nc_mouse(false),
      m_restore_posted(false),
      m_blurred(false),
      m_removed(false),
      m_pending_position(false),
      m_pending_size(false),
      m_group_move(false),
      m_prev_height(0),
      m_scroll_height(0),
      m_scroll_offset(0),
      m_last_mouse_invalidate(0),
      m_last_position_save(0),
      m_last_size_save(0),
      m_drop_target(NULL),
      m_rename_target(MCFENCE_RENAME_NONE),
      m_rename_caret(0),
      m_rename_pristine(false),
      m_rename_caret_on(false),
      m_rename_scroll(0.0f),
      m_rename_origin(0.0f)
{
    m_drag_start.x = 0;
    m_drag_start.y = 0;
    m_marquee_start = m_drag_start;
    m_marquee_end = m_drag_start;
    ZeroMemory(&m_move_origin, sizeof(m_move_origin));
    m_rename_caret_point.x = 0;
    m_rename_caret_point.y = 0;
    m_logical_title_height = (m_info.title_height < 16 || m_info.title_height > 100) ? 35
                                                                                 : m_info.title_height;
}

McFenceWnd::~McFenceWnd()
{
    delete m_title_font;
    delete m_icon_font;
}

//****************************************************************************
//	Method:		ScreenDpi
//	FullName:	McFenceWnd::ScreenDpi
//	author:
//	date:		2026/11/06
//	Function:   获取当前围栏窗口所在显示器的 DPI，优先使用 GetDpiForWindow，不可用时退回 GetDeviceCaps。
//
//	Returns:	int    窗口当前的 DPI 值。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
int McFenceWnd::ScreenDpi() const
{
    const McUser32Api& api = McUser32Ext();
    if (api.get_dpi_for_window && m_hwnd)
    {
        const UINT dpi = api.get_dpi_for_window(m_hwnd);
        if (dpi >= 48)
        {
            return static_cast<int>(dpi);
        }
    }
    HDC dc = GetDC(m_hwnd);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc)
    {
        ReleaseDC(m_hwnd, dc);
    }
    return dpi > 0 ? dpi : 96;
}

int McFenceWnd::scaled(int value) const
{
    return static_cast<int>(value * m_scale + 0.5f);
}

float McFenceWnd::scaled_f(float value) const
{
    return value * m_scale;
}

int McFenceWnd::ItemHeight() const
{
    return m_icon_box + m_item_padding + m_text_height;
}

//****************************************************************************
//	Method:		ColumnCount
//	FullName:	McFenceWnd::ColumnCount
//	author:
//	date:		2026/11/06
//	Function:   根据客户区宽度与单个项目的宽度、间距计算图标网格可显示的列数。
//
//	Returns:	int    当前布局下的列数（至少为 1）。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
int McFenceWnd::ColumnCount() const
{
    RECT client;
    GetClientRect(m_hwnd, &client);
    const int width = client.right - client.left;
    if (width <= m_item_padding + m_item_width)
    {
        return 1;
    }
    const int step = max(1, m_item_width + m_item_padding);
    return 1 + (width - m_item_padding - m_item_width) / step;
}

//****************************************************************************
//	Method:		InitialDpi
//	FullName:	McFenceWnd::InitialDpi
//	author:
//	date:		2026/11/06
//	Function:   在窗口创建之前，根据存储的逻辑坐标所在显示器推算应使用的 DPI，
//              用于创建窗口前的首次缩放。
//
//	Returns:	int    窗口创建前应使用的初始 DPI 值。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
int McFenceWnd::InitialDpi() const
{
    // Before the window exists: the DPI of the monitor the stored (logical)
    // position falls on, otherwise the system DPI.
    POINT point;
    point.x = m_info.pos_x;
    point.y = m_info.pos_y;
    const McShcoreApi& shcore = McShcoreExt();
    if (shcore.get_dpi_for_monitor)
    {
        HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
        UINT dpi_x = 0;
        UINT dpi_y = 0;
        if (monitor && SUCCEEDED(shcore.get_dpi_for_monitor(monitor, 0 /* MDT_EFFECTIVE_DPI */,
                                                            &dpi_x, &dpi_y)) &&
            dpi_x >= 48)
        {
            return static_cast<int>(dpi_x);
        }
    }
    HDC dc = GetDC(NULL);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc)
    {
        ReleaseDC(NULL, dc);
    }
    return dpi > 0 ? dpi : 96;
}

void McFenceWnd::ComputeScale()
{
    ApplyDpi(ScreenDpi());
}

//****************************************************************************
//	Method:		ApplyDpi
//	FullName:	McFenceWnd::ApplyDpi
//	author:
//	date:		2026/11/06
//	Function:   按给定 DPI 重新计算缩放比例，并据此刷新图标大小、间距、文字高度、标题栏高度等以 96DPI 
//              为基准的度量值。
//
//	Returns:	void
//	Parameter:	IN  int dpi    要应用的目标 DPI
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::ApplyDpi(int dpi)
{
    m_dpi = dpi;
    m_scale = static_cast<float>(m_dpi) / 96.0f;
    if (m_scale < 0.5f)
    {
        m_scale = 1.0f;
    }
    // 图标与桌面上的一样大（openFrence 的做法）；格子宽度随图标变，两侧各留 15 px 给文件名。
    m_item_width = scaled(max(60, s_icon_size + 30));
    m_item_padding = scaled(10);
    m_text_height = scaled(22);
    m_icon_box = scaled(s_icon_size);
    m_title_offset = scaled(4);
    m_title_height = scaled(m_logical_title_height);
}

//****************************************************************************
//	Method:		ReloadFonts
//	FullName:	McFenceWnd::ReloadFonts
//	author:
//	date:		2026/11/06
//	Function:   释放旧的标题字体与图标字体，并根据当前 DPI 重新创建。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::ReloadFonts()
{
    delete m_title_font;
    delete m_icon_font;
    m_title_font = McCreateFont(8.5f, m_dpi, false);
    m_icon_font = McCreateFont(7.0f, m_dpi, false);
}

//****************************************************************************
//	Method:		CreateFenceWindow
//	FullName:	McFenceWnd::CreateFenceWindow
//	author:
//	date:		2026/11/06
//	Function:   注册窗口类并创建实际的分层弹出窗口，应用初始 DPI 与字体、启用模糊效果、注册拖放目标，
//              然后显示窗口并置于 Z 序底部。
//
//	Returns:	bool    窗口创建成功返回 true，失败返回 false。
//	Parameter:	无
//
//              创建失败时记录日志并返回，由调用方（Create）负责释放对象
//
//	changelog:  New
//****************************************************************************
bool McFenceWnd::CreateFenceWindow()
{
    if (!ensure_fence_class())
    {
        McLog(L"无法注册围栏窗口类");
        return false;
    }

    const DWORD ex_style = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    const DWORD style = WS_POPUP;
    // The stored geometry is logical (96-dpi) units: the scale has to be known
    // before the window is created, otherwise every save shrinks it by 1/scale.
    ApplyDpi(InitialDpi());
    const int start_x = scaled(m_info.pos_x);
    const int start_y = scaled(m_info.pos_y);
    const int start_w = scaled(m_info.width);
    const int start_h = scaled(m_info.height);
    m_hwnd = CreateWindowExW(ex_style, k_fence_class, m_info.name.c_str(), style, start_x, start_y,
                            start_w, start_h, NULL, NULL, GetModuleHandleW(NULL), this);
    if (!m_hwnd)
    {
        McLogLastError(L"CreateWindowExW(fence)");
        return false;
    }

    // The window may have landed on a monitor with another DPI: rescale in place.
    const int window_dpi = ScreenDpi();
    if (window_dpi != m_dpi)
    {
        ApplyDpi(window_dpi);
        SetWindowPos(m_hwnd, NULL, scaled(m_info.pos_x), scaled(m_info.pos_y), scaled(m_info.width),
                     scaled(m_info.height), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ReloadFonts();

    const bool blurred = McEnableBlur(m_hwnd);
    m_blurred = blurred;
    UpdateBlurRegion();
    // Log the presentation mode so a field report tells us whether blur is on.
    McLogFormat(L"围栏 '%s' 创建: os=%lu.%lu.%lu mode=%s dpi=%d", m_info.name.c_str(),
                static_cast<unsigned long>(McOsVersion().major),
                static_cast<unsigned long>(McOsVersion().minor),
                static_cast<unsigned long>(McOsVersion().build),
                blurred ? L"layered+blur" : L"layered", m_dpi);

    m_drop_target = new McDropTarget(this);
    if (FAILED(RegisterDragDrop(m_hwnd, static_cast<IDropTarget*>(m_drop_target))))
    {
        McLog(L"RegisterDragDrop 失败");
    }

    if (IsMapped())
    {
        ReloadFolder();
        StartFolderWatch();
    }
    EnsureOnScreen();
    ApplyDesktopOwnerTo();
    ShowWindow(m_hwnd, s_hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
    SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    if (m_info.can_minify)
    {
        minify();
    }
    m_initialized = true;
    return true;
}

//****************************************************************************
//	Method:		Create
//	FullName:	McFenceWnd::Create
//	author:
//	date:		2026/11/06
//	Function:   静态工厂方法：分配一个 McFenceWnd 实例并创建其窗口，成功后加入全局注册表并立即触发一次绘制。
//
//	Returns:	McFenceWnd*    创建成功返回新窗口实例指针，失败返回 NULL（内部对象已被释放）。
//	Parameter:	IN  const McFenceInfo& info    新围栏的初始信息
//
//	changelog:  New
//****************************************************************************
McFenceWnd* McFenceWnd::Create(const McFenceInfo& info)
{
    McFenceWnd* view = new McFenceWnd(info);
    if (!view->CreateFenceWindow())
    {
        delete view;
        return NULL;
    }
    s_registry.push_back(view);
    view->Invalidate();
    return view;
}

//****************************************************************************
//	Method:		McInvalidateAll
//	FullName:	McFenceWnd::McInvalidateAll
//	author:
//	date:		2026/11/06
//	Function:   对所有已注册的围栏窗口执行立即重绘，用于后台图标加载完成等需要全局刷新的场景。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::McInvalidateAll()
{
    for (size_t i = 0; i < s_registry.size(); ++i)
    {
        s_registry[i]->Invalidate();
    }
}

size_t McFenceWnd::McLiveCount()
{
    return s_registry.size();
}

//****************************************************************************
//	Method:		McSetIconSize
//	FullName:	McFenceWnd::McSetIconSize
//	author:
//	date:		2026/11/07
//	Function:   记下桌面图标的新边长；和当前不同时清空图标缓存（旧尺寸的位图不会再用），
//              每个围栏重新计算度量值并重绘。
//
//	Returns:	void
//	Parameter:	IN  int logical_size    桌面图标边长（逻辑像素，16..256）
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::McSetIconSize(int logical_size)
{
    if (logical_size < 16 || logical_size > 256 || logical_size == s_icon_size)
    {
        return;
    }
    McLogFormat(L"桌面图标尺寸 %d -> %d", s_icon_size, logical_size);
    s_icon_size = logical_size;
    McIconCache::Instance().Clear();
    for (size_t i = 0; i < s_registry.size(); ++i)
    {
        McFenceWnd* fence = s_registry[i];
        fence->ApplyDpi(fence->m_dpi);
        fence->Invalidate();
    }
}

//****************************************************************************
//	Method:		McRewriteAllFences
//	FullName:	McFenceWnd::McRewriteAllFences
//	author:
//	date:		2026/11/06
//	Function:   将所有已注册围栏的当前信息重新写入存储，供 verify.ps1 等自测脚本验证持久化是否正常。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::McRewriteAllFences()
{
    for (size_t i = 0; i < s_registry.size(); ++i)
    {
        const bool bOk = McStoreWriteFence(s_registry[i]->m_info);
        McLogFormat(L"selftest rewrite '%s' -> %s", s_registry[i]->m_info.id.c_str(),
                    bOk ? L"ok" : L"failed");
    }
}

//****************************************************************************
//	Method:		McDumpAllSurfaces
//	FullName:	McFenceWnd::McDumpAllSurfaces
//	author:
//	date:		2026/11/06
//	Function:   短暂泵送消息循环等待后台图标加载完成，再把每个已注册围栏当前合成好的表面导出为 PNG 文件（多个围栏时文件名依次加序号后缀）。
//
//	Returns:	bool    至少有一个围栏导出成功时返回 true，否则为 false。
//	Parameter:	IN  const std::wstring& path    导出 PNG 的基础文件路径
//
//              内部会阻塞约 900ms 并手动处理消息循环以等待异步图标
//
//	changelog:  New
//****************************************************************************
bool McFenceWnd::McDumpAllSurfaces(const std::wstring& path)
{
    // Let the message loop run for a moment so background icons finish loading.
    const ULONGLONG nDeadline = McTicks() + 900;
    MSG msg;
    while (McTicks() < nDeadline)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(16);
    }

    bool bAny = false;
    for (size_t i = 0; i < s_registry.size(); ++i)
    {
        std::wstring target(path);
        if (i > 0)
        {
            const size_t dot = target.find_last_of(L'.');
            const std::wstring suffix = McFormat(L"_%u", static_cast<unsigned>(i + 1));
            target.insert(dot == std::wstring::npos ? target.size() : dot, suffix);
        }
        bAny = s_registry[i]->McDumpSurface(target) || bAny;
        McLogFormat(L"dump surface -> %s", target.c_str());
    }
    return bAny;
}

//****************************************************************************
//	Method:		McCloseAll
//	FullName:	McFenceWnd::McCloseAll
//	author:
//	date:		2026/11/06
//	Function:   强制关闭所有已注册的围栏窗口，跳过正常关闭时“移除该围栏？”的确认提示。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::McCloseAll()
{
    std::vector<McFenceWnd*> copy = s_registry;
    for (size_t i = 0; i < copy.size(); ++i)
    {
        if (copy[i] && copy[i]->m_hwnd)
        {
            copy[i]->m_allow_close = true;
            DestroyWindow(copy[i]->m_hwnd);
        }
    }
}

//****************************************************************************
//	Method:		McSetAppWindow
//	FullName:	McFenceWnd::McSetAppWindow
//	author:
//	date:		2026/11/07
//	Function:   记录应用的隐藏主窗口（重建围栏、托盘消息都由它处理）。
//
//	Returns:	void
//	Parameter:	IN  HWND hwnd    主窗口
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::McSetAppWindow(HWND hwnd)
{
    s_app_window = hwnd;
}

//****************************************************************************
//	Method:		McSetAllHidden
//	FullName:	McFenceWnd::McSetAllHidden
//	author:
//	date:		2026/11/07
//	Function:   显示或隐藏全部围栏（托盘菜单、全局热键、双击桌面空白处共用）。
//
//	Returns:	void
//	Parameter:	IN  bool hidden    true 隐藏，false 显示
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::McSetAllHidden(bool hidden)
{
    s_hidden = hidden;
    for (size_t i = 0; i < s_registry.size(); ++i)
    {
        McFenceWnd* view = s_registry[i];
        if (!view->m_hwnd)
        {
            continue;
        }
        if (hidden)
        {
            view->EndRename(true, false);
            if (view->m_drag_in_progress)
            {
                POINT screen;
                GetCursorPos(&screen);
                view->FinishInternalDrag(screen, true);
            }
            ShowWindow(view->m_hwnd, SW_HIDE);
        }
        else
        {
            view->EnsureOnScreen();
            ShowWindow(view->m_hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(view->m_hwnd, HWND_BOTTOM, 0, 0, 0, 0, 
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            view->Invalidate();
        }
    }
}

bool McFenceWnd::McAllHidden()
{
    return s_hidden;
}

//****************************************************************************
//	Method:		McIsFenceWindow
//	FullName:	McFenceWnd::McIsFenceWindow
//	author:
//	date:		2026/11/07
//	Function:   判断窗口是否是本进程的某个围栏。
//
//	Returns:	bool    是围栏窗口返回 true
//	Parameter:	IN  HWND hwnd    要判断的窗口
//
//	changelog:  New
//****************************************************************************
bool McFenceWnd::McIsFenceWindow(HWND hwnd)
{
    for (size_t i = 0; i < s_registry.size(); ++i)
    {
        if (s_registry[i]->m_hwnd == hwnd)
        {
            return true;
        }
    }
    return false;
}

//****************************************************************************
//	Method:		McApplyDesktopOwner
//	FullName:	McFenceWnd::McApplyDesktopOwner
//	author:
//	date:		2026/11/07
//	Function:   选项变化或资源管理器重启后，按当前设置重新挂接 / 摘离每个围栏的所有者。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::McApplyDesktopOwner()
{
    for (size_t i = 0; i < s_registry.size(); ++i)
    {
        s_registry[i]->ApplyDesktopOwnerTo();
        s_registry[i]->EnsureOnScreen();
        if (!s_hidden)
        {
            SetWindowPos(s_registry[i]->m_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

//****************************************************************************
//	Method:		ApplyDesktopOwnerTo
//	FullName:	McFenceWnd::ApplyDesktopOwnerTo
//	author:
//	date:		2026/11/07
//	Function:   “Win+D 时保留围栏”：把围栏的所有者设为桌面的 SHELLDLL_DefView。显示桌面只最小化 /
//              隐藏普通顶层窗口，桌面自己名下的窗口会留下。关闭选项时恢复为无所有者。
//
//	Returns:	void
//	Parameter:	无
//
//              这是 DeskBox 的方案：只改所有者（GWLP_HWNDPARENT），不跨进程 SetParent，
//              两个进程的输入队列不会被连在一起，资源管理器卡住时围栏照常响应
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::ApplyDesktopOwnerTo()
{
    if (!m_hwnd)
    {
        return;
    }
    HWND owner = McSettingsGet().keep_on_show_desktop ? McFindDesktopDefView() : NULL;
    if (reinterpret_cast<HWND>(GetWindowLongPtrW(m_hwnd, GWLP_HWNDPARENT)) == owner)
    {
        return;
    }
    SetLastError(0);
    SetWindowLongPtrW(m_hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    McLogFormat(L"围栏 '%s' 所有者 -> %p", m_info.name.c_str(), owner);
}

//****************************************************************************
//	Method:		EnsureOnScreen
//	FullName:	McFenceWnd::EnsureOnScreen
//	author:
//	date:		2026/11/07
//	Function:   显示器拔掉 / 分辨率变化后，把跑到屏幕外的围栏挪回最近显示器的工作区（不改写已保存的位置，
//              除非真的挪动了）。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::EnsureOnScreen()
{
    if (!m_hwnd)
    {
        return;
    }
    RECT rect;
    GetWindowRect(m_hwnd, &rect);
    if (m_is_minified && m_prev_height > 0)
    {
        rect.bottom = rect.top + m_prev_height;
    }
    if (McEnsureRectVisible(rect, m_title_height))
    {
        McLogFormat(L"围栏 '%s' 不在任何显示器上，挪回 (%d,%d)", m_info.name.c_str(), rect.left, rect.top);
        SetWindowPos(m_hwnd, NULL, rect.left, rect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

//****************************************************************************
//	Method:		ReloadFolder
//	FullName:	McFenceWnd::ReloadFolder
//	author:
//	date:		2026/11/07
//	Function:   映射围栏：重新枚举文件夹（按当前排序），保留仍然存在的选择，然后重绘。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::ReloadFolder()
{
    if (!IsMapped())
    {
        return;
    }
    m_info.files = McEnumerateFolder(m_info.folder, m_info.sort, m_info.sort_descending);
    PruneSelection();
    if (m_hwnd)
    {
        Invalidate();
    }
}

void McFenceWnd::StartFolderWatch()
{
    if (IsMapped() && m_hwnd && !m_watch.Start(m_hwnd, m_info.folder))
    {
        McLogFormat(L"无法监视文件夹 '%s'", m_info.folder.c_str());
    }
}

//****************************************************************************
//	Method:		Invalidate
//	FullName:	McFenceWnd::Invalidate
//	author:
//	date:		2026/11/06
//	Function:   立即触发一次重绘；由于分层窗口不会收到有效的 WM_PAINT，因此直接调用 paint()。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::Invalidate()
{
    if (m_hwnd)
    {
        // A layered window never receives a useful WM_PAINT; paint right away so the
        // new content is composited in the same frame.
        paint();
    }
}

//****************************************************************************
//	Method:		paint
//	FullName:	McFenceWnd::paint
//	author:
//	date:		2026/11/06
//	Function:   核心绘制函数：合成围栏圆角背景面板、标题栏文字、图标网格、滚动条与拖放插入指示线、外边框，
//              并将最终结果提交给分层窗口显示。
//
//	Returns:	void
//	Parameter:	无
//
//              绘制过程中会同步更新当前悬停项和选中项状态
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::paint()
{
    if (!m_hwnd)
    {
        return;
    }
    RECT client;
    GetClientRect(m_hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
    {
        return;
    }
    if (!m_surface.Resize(width, height))
    {
        return;
    }

    Gdiplus::Graphics& g = m_surface.graphics();
    g.ResetClip();
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    const float radius = static_cast<float>(scaled(9));
    Gdiplus::GraphicsPath panel;
    {
        const float w = static_cast<float>(width);
        const float h = static_cast<float>(height);
        const float d = radius * 2.0f;
        panel.AddArc(0.0f, 0.0f, d, d, 180.0f, 90.0f);
        panel.AddArc(w - d, 0.0f, d, d, 270.0f, 90.0f);
        panel.AddArc(w - d, h - d, d, d, 0.0f, 90.0f);
        panel.AddArc(0.0f, h - d, d, d, 90.0f, 90.0f);
        panel.CloseFigure();
    }
    g.SetClip(&panel);

    {
        Gdiplus::SolidBrush background(Gdiplus::Color(110, 0, 0, 0));
        g.FillRectangle(&background, 0, 0, width, height);
        Gdiplus::SolidBrush title(Gdiplus::Color(55, 0, 0, 0));
        g.FillRectangle(&title, 0, 0, width, m_title_height);
    }

    // Title text: centred with character ellipsis.
    {
        Gdiplus::RectF title_bounds(0.0f, static_cast<float>(m_title_offset),
                                    static_cast<float>(width),
                                    static_cast<float>(m_title_height - m_title_offset));
        if (m_rename_target != MCFENCE_RENAME_TITLE)
        {
            McDrawTextLine(g, m_info.name, m_title_font, title_bounds, 
                Gdiplus::Color(255, 255, 255, 255), true);
        }
    }

    RefreshRendered();

    POINT cursor;
    GetCursorPos(&cursor);
    POINT cursor_client = cursor;
    ScreenToClient(m_hwnd, &cursor_client);
    m_hovering_item.clear();

    int x = m_item_padding;
    int y = m_item_padding;
    m_scroll_height = 0;
    for (size_t i = 0; i < m_rendered.size(); ++i)
    {
        const int draw_x = x;
        const int draw_y = y + m_title_height - m_scroll_offset;
        DrawItem(g, m_rendered[i], draw_x, draw_y);

        Gdiplus::RectF hit_bounds(static_cast<float>(draw_x - scaled(2)),
                                  static_cast<float>(draw_y - scaled(2)),
                                  static_cast<float>(m_item_width + scaled(4)),
                                  static_cast<float>(m_icon_box + m_text_height + scaled(8)));
        const bool hover = cursor_client.x >= hit_bounds.X && cursor_client.y >= hit_bounds.Y &&
                           cursor_client.x < hit_bounds.X + hit_bounds.Width &&
                           cursor_client.y < hit_bounds.Y + hit_bounds.Height;
        if (hover)
        {
            m_hovering_item = m_rendered[i].item.path;
        }

        const int bottom = y + ItemHeight();
        if (bottom > m_scroll_height)
        {
            m_scroll_height = bottom;
        }
        x += m_item_width + m_item_padding;
        if (x + m_item_width > width)
        {
            x = m_item_padding;
            y += ItemHeight() + m_item_padding;
        }
    }

    m_scroll_height -= (height - m_title_height);
    if (m_scroll_height > 0)
    {
        const int bar_x = width - scaled(5);
        const int bar_y = m_title_height + m_scroll_offset;
        const int bar_height = height - m_title_height - m_scroll_height;
        if (bar_height > 0)
        {
            Gdiplus::SolidBrush bar(Gdiplus::Color(150, 0, 0, 0));
            g.FillRectangle(&bar, bar_x, bar_y, scaled(5), bar_height);
        }
        if (m_scroll_offset > m_scroll_height)
        {
            m_scroll_offset = m_scroll_height;
        }
    }
    else
    {
        m_scroll_height = 0;
        m_scroll_offset = 0;
    }

    DrawDropIndicator(g);
    DrawMarquee(g);
    DrawRenameEditor(g, width);

    // Restore the clip so the outline can be drawn on the panel edge.
    g.ResetClip();
    {
        Gdiplus::Pen outline(Gdiplus::Color(60, 255, 255, 255), 1.0f);
        Gdiplus::GraphicsPath border;
        {
            const float w = static_cast<float>(width) - 1.0f;
            const float h = static_cast<float>(height) - 1.0f;
            const float d = radius * 2.0f;
            border.AddArc(0.5f, 0.5f, d, d, 180.0f, 90.0f);
            border.AddArc(w - d, 0.5f, d, d, 270.0f, 90.0f);
            border.AddArc(w - d, h - d, d, d, 0.0f, 90.0f);
            border.AddArc(0.5f, h - d, d, d, 90.0f, 90.0f);
            border.CloseFigure();
        }
        g.DrawPath(&outline, &border);
    }

    m_surface.Present(m_hwnd);
    ValidateRect(m_hwnd, NULL);
}

//****************************************************************************
//	Method:		RefreshRendered
//	FullName:	McFenceWnd::RefreshRendered
//	author:
//	date:		2026/11/06
//	Function:   依据 m_info.files 重新构建可渲染项目列表，过滤掉路径解析失败的条目。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::RefreshRendered()
{
    m_rendered.clear();
    for (size_t i = 0; i < m_info.files.size(); ++i)
    {
        McEntry item;
        if (McEntryFromPath(m_info.files[i], item))
        {
            McRenderedItem rendered;
            rendered.model_index = static_cast<int>(i);
            rendered.item = item;
            m_rendered.push_back(rendered);
        }
    }
}

//****************************************************************************
//	Method:		DrawItem
//	FullName:	McFenceWnd::DrawItem
//	author:
//	date:		2026/11/06
//	Function:   绘制单个图标项：按悬停/选中状态绘制高亮背景与边框，再绘制图标位图和自动换行的文件名文字。
//
//	Returns:	void
//	Parameter:	IN OUTGdiplus::Graphics& g    绘制目标的 GDI+ 图形对象
//              IN  const McRenderedItem& item    待绘制的渲染项（模型索引与条目数据）
//              IN  int x    绘制起始的客户区 x 坐标
//              IN  int y    绘制起始的客户区 y 坐标
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::DrawItem(Gdiplus::Graphics& g, const McRenderedItem& item, int x, int y)
{
    const std::wstring name = item.item.display_name();

    Gdiplus::RectF text_bounds(static_cast<float>(x - scaled(2)),
                               static_cast<float>(y + m_icon_box + scaled(4)),
                               static_cast<float>(m_item_width), static_cast<float>(m_text_height));
    Gdiplus::RectF hit(static_cast<float>(x - scaled(2)), static_cast<float>(y - scaled(2)),
                       static_cast<float>(m_item_width + scaled(4)),
                       static_cast<float>(m_icon_box + m_text_height + scaled(8)));

    POINT cursor;
    GetCursorPos(&cursor);
    POINT client = cursor;
    ScreenToClient(m_hwnd, &client);
    const bool hover = client.x >= hit.X && client.y >= hit.Y && client.x < hit.X + hit.Width &&
                       client.y < hit.Y + hit.Height;

    const bool editing = m_rename_target == MCFENCE_RENAME_ITEM && McIEquals(m_rename_path, item.item.path);
    if (editing)
    {
        // openFrence：正在改名的项目不画选中底板，只留编辑框。
    }
    else if (IsSelected(item.item.path))
    {
        const COLORREF fill = hover ? GetSysColor(COLOR_GRADIENTACTIVECAPTION)
                                    : GetSysColor(COLOR_GRADIENTINACTIVECAPTION);
        const BYTE alpha = hover ? 110 : 80;
        Gdiplus::SolidBrush selection(McArgb(alpha, fill));
        g.FillRectangle(&selection, hit);
        Gdiplus::Pen border(McArgb(130, GetSysColor(COLOR_ACTIVEBORDER)), 1.0f);
        g.DrawRectangle(&border, hit.X + 1.0f, hit.Y + 1.0f, hit.Width - 2.0f, hit.Height - 2.0f);
    }
    else if (hover)
    {
        Gdiplus::SolidBrush selection(McArgb(80, GetSysColor(COLOR_ACTIVECAPTION)));
        g.FillRectangle(&selection, hit);
        Gdiplus::Pen border(McArgb(130, GetSysColor(COLOR_ACTIVEBORDER)), 1.0f);
        g.DrawRectangle(&border, hit.X + 1.0f, hit.Y + 1.0f, hit.Width - 2.0f, hit.Height - 2.0f);
    }

    Gdiplus::Bitmap* bitmap = McIconCache::Instance().GetIcon(item.item, m_icon_box);
    if (bitmap)
    {
        const int image_x = x + (m_item_width - m_icon_box) / 2;
        g.DrawImage(bitmap, image_x, y, m_icon_box, m_icon_box);
    }

    if (!(m_rename_target == MCFENCE_RENAME_ITEM && McIEquals(m_rename_path, item.item.path)))
    {
        McDrawTextWrapped(g, name, m_icon_font, text_bounds, Gdiplus::Color(255, 255, 255, 255));
    }
}

//****************************************************************************
//	Method:		DrawDropIndicator
//	FullName:	McFenceWnd::DrawDropIndicator
//	author:
//	date:		2026/11/06
//	Function:   若存在有效的拖放插入位置且围栏未锁定，在对应网格位置绘制一条竖线作为插入指示。
//
//	Returns:	void
//	Parameter:	IN OUTGdiplus::Graphics& g    绘制目标的 GDI+ 图形对象
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::DrawDropIndicator(Gdiplus::Graphics& g)
{
    if (m_drop_index < 0 || m_info.locked)
    {
        return;
    }
    const int columns = ColumnCount();
    const int visual = ModelToVisualIndex(m_drop_index);
    const int column = visual % columns;
    const int row = visual / columns;
    const int x = m_item_padding + column * (m_item_width + m_item_padding) - scaled(3);
    const int y = m_title_height + m_item_padding + row * (ItemHeight() + m_item_padding) -
                  m_scroll_offset;
    const float thickness = static_cast<float>(max(2, scaled(2)));
    Gdiplus::Pen pen(McArgb(230, GetSysColor(COLOR_HIGHLIGHT)), thickness);
    g.DrawLine(&pen, static_cast<float>(x), static_cast<float>(y), static_cast<float>(x),
               static_cast<float>(y + ItemHeight()));
}

//****************************************************************************
//	Method:		ItemBounds
//	FullName:	McFenceWnd::ItemBounds
//	author:
//	date:		2026/11/07
//	Function:   计算第 visual 个可视项目的命中矩形（客户区坐标，已减去滚动偏移）。
//
//	Returns:	RECT    项目的命中矩形
//	Parameter:	IN  int visual    可视顺序索引
//
//	changelog:  New
//****************************************************************************
RECT McFenceWnd::ItemBounds(int visual) const
{
    const int columns = ColumnCount();
    const int column = visual % columns;
    const int row = visual / columns;
    const int x = m_item_padding + column * (m_item_width + m_item_padding);
    const int y = m_title_height + m_item_padding + row * (ItemHeight() + m_item_padding) - m_scroll_offset;
    RECT rect;
    rect.left = x - scaled(2);
    rect.top = y - scaled(2);
    rect.right = rect.left + m_item_width + scaled(4);
    rect.bottom = rect.top + m_icon_box + m_text_height + scaled(8);
    return rect;
}

//****************************************************************************
//	Method:		IsSelected
//	FullName:	McFenceWnd::IsSelected
//	author:
//	date:		2026/11/07
//	Function:   判断路径是否在当前选择中。
//
//	Returns:	bool    已选中返回 true
//	Parameter:	IN  const std::wstring& path    项目路径
//
//	changelog:  New
//****************************************************************************
bool McFenceWnd::IsSelected(const std::wstring& path) const
{
    for (size_t i = 0; i < m_selection.size(); ++i)
    {
        if (McIEquals(m_selection[i], path))
        {
            return true;
        }
    }
    return false;
}

void McFenceWnd::SelectOnly(const std::wstring& path)
{
    m_selection.clear();
    if (!path.empty())
    {
        m_selection.push_back(path);
    }
}

void McFenceWnd::ToggleSelected(const std::wstring& path)
{
    if (path.empty())
    {
        return;
    }
    if (!RemovePath(m_selection, path))
    {
        m_selection.push_back(path);
    }
}

void McFenceWnd::SelectAll()
{
    RefreshRendered();
    m_selection.clear();
    for (size_t i = 0; i < m_rendered.size(); ++i)
    {
        m_selection.push_back(m_rendered[i].item.path);
    }
}

//****************************************************************************
//	Method:		PruneSelection
//	FullName:	McFenceWnd::PruneSelection
//	author:
//	date:		2026/11/07
//	Function:   去掉选择中已经不在围栏里的路径（项目被删除、映射文件夹刷新后）。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::PruneSelection()
{
    std::vector<std::wstring> kept;
    for (size_t i = 0; i < m_selection.size(); ++i)
    {
        for (size_t j = 0; j < m_info.files.size(); ++j)
        {
            if (McIEquals(m_selection[i], m_info.files[j]))
            {
                kept.push_back(m_selection[i]);
                break;
            }
        }
    }
    m_selection.swap(kept);
}

//****************************************************************************
//	Method:		SelectedInOrder
//	FullName:	McFenceWnd::SelectedInOrder
//	author:
//	date:		2026/11/07
//	Function:   按围栏中的显示顺序返回已选中的路径（拖动、删除时保持原有相对顺序）。
//
//	Returns:	std::vector<std::wstring>    已选中的路径
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
std::vector<std::wstring> McFenceWnd::SelectedInOrder() const
{
    std::vector<std::wstring> result;
    for (size_t i = 0; i < m_info.files.size(); ++i)
    {
        if (IsSelected(m_info.files[i]))
        {
            result.push_back(m_info.files[i]);
        }
    }
    return result;
}

//****************************************************************************
//	Method:		UpdateMarquee
//	FullName:	McFenceWnd::UpdateMarquee
//	author:
//	date:		2026/11/07
//	Function:   框选过程中按当前鼠标位置更新选择：与框相交的项目被选中（Ctrl 框选时与原选择做异或）。
//
//	Returns:	void
//	Parameter:	IN  POINT client    当前鼠标的客户区坐标
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::UpdateMarquee(POINT client)
{
    m_marquee_end.x = client.x;
    m_marquee_end.y = client.y + m_scroll_offset;
    RECT box;
    box.left = min(m_marquee_start.x, m_marquee_end.x);
    box.right = max(m_marquee_start.x, m_marquee_end.x);
    box.top = min(m_marquee_start.y, m_marquee_end.y) - m_scroll_offset;
    box.bottom = max(m_marquee_start.y, m_marquee_end.y) - m_scroll_offset;

    m_selection = m_marquee_base;
    RefreshRendered();
    for (size_t i = 0; i < m_rendered.size(); ++i)
    {
        const RECT item = ItemBounds(static_cast<int>(i));
        RECT overlap;
        if (IntersectRect(&overlap, &item, &box))
        {
            const std::wstring& path = m_rendered[i].item.path;
            if (m_marquee_toggle)
            {
                ToggleSelected(path);
            }
            else if (!IsSelected(path))
            {
                m_selection.push_back(path);
            }
        }
    }
    Invalidate();
}

//****************************************************************************
//	Method:		DrawMarquee
//	FullName:	McFenceWnd::DrawMarquee
//	author:
//	date:		2026/11/07
//	Function:   绘制框选矩形（系统高亮色半透明填充 + 描边）。
//
//	Returns:	void
//	Parameter:	IN OUTGdiplus::Graphics& g    绘制目标
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::DrawMarquee(Gdiplus::Graphics& g)
{
    if (!m_marquee)
    {
        return;
    }
    const int left = min(m_marquee_start.x, m_marquee_end.x);
    const int top = min(m_marquee_start.y, m_marquee_end.y) - m_scroll_offset;
    const int width = abs(m_marquee_end.x - m_marquee_start.x);
    const int height = abs(m_marquee_end.y - m_marquee_start.y);
    if (width < 2 && height < 2)
    {
        return;
    }
    Gdiplus::SolidBrush fill(McArgb(60, GetSysColor(COLOR_HIGHLIGHT)));
    g.FillRectangle(&fill, left, top, width, height);
    Gdiplus::Pen border(McArgb(200, GetSysColor(COLOR_HIGHLIGHT)), 1.0f);
    g.DrawRectangle(&border, left, top, width, height);
}

//****************************************************************************
//	Method:		HitTestItem
//	FullName:	McFenceWnd::HitTestItem
//	author:
//	date:		2026/11/07
//	Function:   刷新渲染列表后，根据客户区坐标命中测试对应的图标项，返回其文件路径。
//
//	Returns:	std::wstring    命中项目的文件路径；未命中任何项目时为空字符串。
//	Parameter:	IN  POINT client    客户区坐标点
//
//	changelog:  2026/11/07 改用 ItemBounds
//****************************************************************************
std::wstring McFenceWnd::HitTestItem(POINT client)
{
    RefreshRendered();
    if (client.y < m_title_height)
    {
        return std::wstring();
    }
    for (size_t i = 0; i < m_rendered.size(); ++i)
    {
        const RECT bounds = ItemBounds(static_cast<int>(i));
        if (PtInRect(&bounds, client))
        {
            return m_rendered[i].item.path;
        }
    }
    return std::wstring();
}

//****************************************************************************
//	Method:		DropInsertIndex
//	FullName:	McFenceWnd::DropInsertIndex
//	author:
//	date:		2026/11/06
//	Function:   根据拖放时的客户区坐标计算行列位置，推算出应插入到模型列表中的目标索引。
//
//	Returns:	int    对应的模型插入索引。
//	Parameter:	IN  POINT client    拖放位置的客户区坐标
//
//	changelog:  New
//****************************************************************************
int McFenceWnd::DropInsertIndex(POINT client) const
{
    RECT rect;
    GetClientRect(m_hwnd, &rect);
    const int width = rect.right - rect.left;
    const int columns = (width <= m_item_padding + m_item_width)
                            ? 1
                            : 1 + (width - m_item_padding - m_item_width) / max(1, m_item_width + m_item_padding);
    const int cell_width = max(1, m_item_width + m_item_padding);
    const int row_height = max(1, ItemHeight() + m_item_padding);
    const int content_x = max(0, static_cast<int>(client.x) - m_item_padding);
    const int content_y = max(0, static_cast<int>(client.y) - m_title_height + m_scroll_offset - m_item_padding);
    const int column = min(columns - 1, content_x / cell_width);
    const int row = content_y / row_height;
    int visual = row * columns + column;
    if (content_x % cell_width > m_item_width / 2)
    {
        visual++;
    }
    visual = max(0, min(visual, static_cast<int>(m_rendered.size())));
    return VisualToModelIndex(visual);
}

//****************************************************************************
//	Method:		VisualToModelIndex
//	FullName:	McFenceWnd::VisualToModelIndex
//	author:
//	date:		2026/11/06
//	Function:   将可视（渲染顺序）索引转换为模型（m_info.files）索引。
//
//	Returns:	int    对应的模型索引；超出渲染列表范围时返回文件总数（代表末尾位置）。
//	Parameter:	IN  int visual    可视顺序索引
//
//	changelog:  New
//****************************************************************************
int McFenceWnd::VisualToModelIndex(int visual) const
{
    if (visual < 0)
    {
        visual = 0;
    }
    if (visual < static_cast<int>(m_rendered.size()))
    {
        return m_rendered[visual].model_index;
    }
    return static_cast<int>(m_info.files.size());
}

//****************************************************************************
//	Method:		ModelToVisualIndex
//	FullName:	McFenceWnd::ModelToVisualIndex
//	author:
//	date:		2026/11/06
//	Function:   将模型索引转换为当前渲染顺序下对应的可视索引。
//
//	Returns:	int    对应的可视索引。
//	Parameter:	IN  int model    模型（文件列表）索引
//
//	changelog:  New
//****************************************************************************
int McFenceWnd::ModelToVisualIndex(int model) const
{
    int visual = 0;
    while (visual < static_cast<int>(m_rendered.size()) && m_rendered[visual].model_index < model)
    {
        ++visual;
    }
    return visual;
}

//****************************************************************************
//	Method:		SetDropIndex
//	FullName:	McFenceWnd::SetDropIndex
//	author:
//	date:		2026/11/06
//	Function:   设置当前拖放插入指示的位置索引，发生变化时触发重绘。
//
//	Returns:	void
//	Parameter:	IN  int index    新的拖放插入模型索引，-1 表示不显示指示线
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::SetDropIndex(int index)
{
    if (m_drop_index != index)
    {
        m_drop_index = index;
        Invalidate();
    }
}

//****************************************************************************
//	Method:		SetFenceHeight
//	FullName:	McFenceWnd::SetFenceHeight
//	author:
//	date:		2026/11/06
//	Function:   在不改变窗口位置与宽度的前提下，调整围栏窗口的高度。
//
//	Returns:	void
//	Parameter:	IN  int height    新的窗口高度（像素）
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::SetFenceHeight(int height)
{
    RECT rect;
    GetWindowRect(m_hwnd, &rect);
    SetWindowPos(m_hwnd, NULL, 0, 0, rect.right - rect.left, height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

//****************************************************************************
//	Method:		minify
//	FullName:	McFenceWnd::minify
//	author:
//	date:		2026/11/06
//	Function:   将围栏折叠为仅显示标题栏的高度，并记录折叠前的高度以便恢复。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::minify()
{
    RECT client;
    GetClientRect(m_hwnd, &client);
    if (m_info.can_minify && !m_is_minified)
    {
        m_is_minified = true;
        m_prev_height = client.bottom - client.top;
        SetFenceHeight(m_title_height);
        Invalidate();
    }
}

//****************************************************************************
//	Method:		unminify
//	FullName:	McFenceWnd::unminify
//	author:
//	date:		2026/11/06
//	Function:   将已折叠的围栏恢复为折叠前记录的高度。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::unminify()
{
    if (!m_is_minified)
    {
        return;
    }
    m_is_minified = false;
    if (m_prev_height > 0)
    {
        SetFenceHeight(m_prev_height);
    }
    Invalidate();
}

//****************************************************************************
//	Method:		OnMouseLeft
//	FullName:	McFenceWnd::OnMouseLeft
//	author:
//	date:		2026/11/06
//	Function:   处理鼠标真正离开窗口的情形：若未处于拖拽中且允许自动折叠则折叠围栏，并清除选中项与悬停项后重绘。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::OnMouseLeft()
{
    POINT cursor;
    GetCursorPos(&cursor);
    RECT window;
    GetWindowRect(m_hwnd, &window);
    if (!PtInRect(&window, cursor) && !m_drag_in_progress)
    {
        if (m_info.can_minify && !m_marquee)
        {
            minify();
        }
    }
    m_hovering_item.clear();
    Invalidate();
}

//****************************************************************************
//	Method:		UpdateBlurRegion
//	FullName:	McFenceWnd::UpdateBlurRegion
//	author:
//	date:		2026/11/06
//	Function:   在启用了亚克力/模糊效果时，为窗口设置与绘制面板一致的圆角区域，使窗口四角保持透明。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::UpdateBlurRegion()
{
    // The accent blur fills the whole window rectangle; clip it to the same
    // rounded panel paint() draws so the corners stay transparent.
    if (!m_blurred || !m_hwnd)
    {
        return;
    }
    RECT rect;
    GetWindowRect(m_hwnd, &rect);
    const int diameter = scaled(9) * 2;
    HRGN region = CreateRoundRectRgn(0, 0, rect.right - rect.left + 1, rect.bottom - rect.top + 1,
                                     diameter, diameter);
    if (region && !SetWindowRgn(m_hwnd, region, TRUE))
    {
        DeleteObject(region);  // the system owns it only on success
    }
}

//****************************************************************************
//	Method:		ApplyTitleHeight
//	FullName:	McFenceWnd::ApplyTitleHeight
//	author:
//	date:		2026/11/06
//	Function:   重新计算 DPI 缩放和字体，若围栏处于折叠状态则同步调整折叠后高度，然后触发重绘。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::ApplyTitleHeight()
{
    ComputeScale();
    ReloadFonts();
    if (m_is_minified)
    {
        SetFenceHeight(m_title_height);
    }
    Invalidate();
}

void McFenceWnd::SaveNow()
{
    McStoreWriteFence(m_info);
}

//****************************************************************************
//	Method:		RequestSave
//	FullName:	McFenceWnd::RequestSave
//	author:
//	date:		2026/11/06
//	Function:   请求保存位置或大小：若与上次保存间隔已超过节流时间则立即保存，否则重置一个定时器延迟到节流窗口结束后再保存（节流执行）。
//
//	Returns:	void
//	Parameter:	IN  int timer_id    节流保存使用的定时器 ID（区分保存位置或保存大小）
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::RequestSave(int timer_id)
{
    const ULONGLONG now = McTicks();
    const ULONGLONG& last = (timer_id == timer_save_position) ? m_last_position_save : m_last_size_save;
    const ULONGLONG elapsed = now - last;
    if (elapsed >= MCFENCE_SAVE_THROTTLE_MS)
    {
        KillTimer(m_hwnd, timer_id);
        if (timer_id == timer_save_position)
        {
            SavePosition();
        }
        else
        {
            SaveSize();
        }
        return;
    }
    // Deliver the throttled save on the window timer; restarting the timer keeps
    // only the last request, like ThrottledExecution.
    (timer_id == timer_save_position ? m_pending_position : m_pending_size) = true;
    KillTimer(m_hwnd, timer_id);
    SetTimer(m_hwnd, timer_id, static_cast<UINT>(MCFENCE_SAVE_THROTTLE_MS - elapsed), NULL);
}

//****************************************************************************
//	Method:		SavePosition
//	FullName:	McFenceWnd::SavePosition
//	author:
//	date:		2026/11/06
//	Function:   读取窗口当前屏幕位置，换算为逻辑（96DPI）坐标写入 m_info 并立即持久化保存。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::SavePosition()
{
    RECT rect;
    GetWindowRect(m_hwnd, &rect);
    m_info.pos_x = static_cast<int>(static_cast<float>(rect.left) / m_scale + 0.5f);
    m_info.pos_y = static_cast<int>(static_cast<float>(rect.top) / m_scale + 0.5f);
    SaveNow();
    m_pending_position = false;
    m_last_position_save = McTicks();
}

//****************************************************************************
//	Method:		SaveSize
//	FullName:	McFenceWnd::SaveSize
//	author:
//	date:		2026/11/06
//	Function:   读取窗口当前客户区尺寸（折叠状态下取折叠前高度），换算为逻辑尺寸写入 m_info 并立即持久化保存。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::SaveSize()
{
    RECT client;
    GetClientRect(m_hwnd, &client);
    const int height = m_is_minified && m_prev_height > 0 ? m_prev_height : (client.bottom - client.top);
    m_info.width = static_cast<int>(static_cast<float>(client.right - client.left) / m_scale + 0.5f);
    m_info.height = static_cast<int>(static_cast<float>(height) / m_scale + 0.5f);
    SaveNow();
    m_pending_size = false;
    m_last_size_save = McTicks();
}

//****************************************************************************
//	Method:		RemoveRecords
//	FullName:	McFenceWnd::RemoveRecords
//	author:
//	date:		2026/11/07
//	Function:   从普通围栏移除项目记录；Items 仓库里属于本程序的副本一并删到回收站（可恢复），
//              不会碰仓库以外的任何文件。
//
//	Returns:	void
//	Parameter:	IN  const std::vector<std::wstring>& items    要移除的项目路径
//
//	changelog:  2026/11/07 由 DeleteItemRecord 改为批量，并清理仓库副本
//****************************************************************************
void McFenceWnd::RemoveRecords(const std::vector<std::wstring>& items)
{
    if (items.empty() || IsMapped())
    {
        return;
    }
    std::vector<std::wstring> managed;
    bool changed = false;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (RemovePath(m_info.files, items[i]))
        {
            changed = true;
            RemovePath(m_selection, items[i]);
            if (McStoreIsManagedItem(items[i]) && McPathExists(items[i]))
            {
                managed.push_back(items[i]);
            }
        }
    }
    if (!changed)
    {
        return;
    }
    SaveNow();
    Invalidate();
    if (!managed.empty())
    {
        McStoreRecycleManaged(managed);
    }
}

//****************************************************************************
//	Method:		RecycleItems
//	FullName:	McFenceWnd::RecycleItems
//	author:
//	date:		2026/11/07
//	Function:   映射文件夹围栏：把选中的真实文件删除到回收站（系统负责确认），随后刷新。
//
//	Returns:	void
//	Parameter:	IN  const std::vector<std::wstring>& items    要删除的路径
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::RecycleItems(const std::vector<std::wstring>& items)
{
    if (items.empty())
    {
        return;
    }
    if (McShellRecycle(m_hwnd, items))
    {
        ReloadFolder();
    }
}

//****************************************************************************
//	Method:		RemovePath
//	FullName:	McFenceWnd::RemovePath
//	author:
//	date:		2026/11/06
//	Function:   在给定路径列表中以忽略大小写的方式查找并移除所有与目标路径匹配的项。
//
//	Returns:	bool    确实移除了至少一项时返回 true，否则返回 false。
//	Parameter:	IN OUTstd::vector<std::wstring>& items    待处理的路径列表，匹配项会被从中移除
//              IN  const std::wstring& path    要移除的目标路径
//
//	changelog:  New
//****************************************************************************
bool McFenceWnd::RemovePath(std::vector<std::wstring>& items, const std::wstring& path)
{
    if (items.empty() || path.empty())
    {
        return false;
    }
    bool removed = false;
    for (size_t i = items.size(); i > 0; --i)
    {
        if (McIEquals(items[i - 1], path))
        {
            items.erase(items.begin() + (i - 1));
            removed = true;
        }
    }
    return removed;
}

//****************************************************************************
//	Method:		OpenFenceItem
//	FullName:	McFenceWnd::OpenFenceItem
//	author:
//	date:		2026/11/06
//	Function:   异步打开指定路径对应的文件或文件夹。
//
//	Returns:	void
//	Parameter:	IN  const std::wstring& path    要打开的项目路径
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::OpenFenceItem(const std::wstring& path)
{
    if (path.empty())
    {
        return;
    }
    McOpenPathAsync(path);
}

//****************************************************************************
//	Method:		ApplySort
//	FullName:	McFenceWnd::ApplySort
//	author:
//	date:		2026/11/07
//	Function:   切换排序方式：映射围栏按新方式重新枚举；普通围栏把列表重排一次并记住方式
//              （之后手动拖动会回到“手动排列”）。
//
//	Returns:	void
//	Parameter:	IN  int mode    McSortMode
//              IN  bool descending    降序
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::ApplySort(int mode, bool descending)
{
    if (IsMapped() && mode == MCFENCE_SORT_NONE)
    {
        mode = MCFENCE_SORT_NAME;  // 文件夹没有“手动顺序”
    }
    m_info.sort = mode;
    m_info.sort_descending = descending;
    if (IsMapped())
    {
        ReloadFolder();
    }
    else
    {
        McSortPaths(m_info.files, mode, descending);
    }
    SaveNow();
    Invalidate();
}

//****************************************************************************
//	Method:		CreateFromNewMenu
//	FullName:	McFenceWnd::CreateFromNewMenu
//	author:
//	date:		2026/11/07
//	Function:   执行 Shell「新建」子菜单的命令：映射围栏直接建在文件夹里；普通围栏建在一个新的
//              Items 子目录里并加入记录。建好后立即弹出重命名。
//
//	Returns:	void
//	Parameter:	IN  UINT command    TrackPopupMenu 返回的命令 id
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::CreateFromNewMenu(UINT command)
{
    const std::wstring created = m_new_menu.Invoke(m_hwnd, command);
    if (created.empty())
    {
        return;
    }
    if (IsMapped())
    {
        ReloadFolder();
    }
    else
    {
        m_info.files.push_back(created);
        m_new_menu_container.clear();  // 已被使用，不再当作临时目录清理
        McSortPaths(m_info.files, m_info.sort, m_info.sort_descending);
        SaveNow();
    }
    SelectOnly(created);
    Invalidate();
    BeginItemRename(created);
}

//****************************************************************************
//	Method:		TakeFocus
//	FullName:	McFenceWnd::TakeFocus
//	author:
//	date:		2026/11/07
//	Function:   让围栏拿到键盘焦点（原地重命名要接收按键）。正常情况下用户刚点过围栏，SetForegroundWindow
//              直接成功；否则临时把输入队列挂到前台线程上再设置（openFrence 用的同一个办法）。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::TakeFocus()
{
    if (GetForegroundWindow() != m_hwnd)
    {
        SetForegroundWindow(m_hwnd);
    }
    if (GetForegroundWindow() != m_hwnd)
    {
        HWND foreground = GetForegroundWindow();
        const DWORD other = foreground ? GetWindowThreadProcessId(foreground, NULL) : 0;
        const DWORD self = GetCurrentThreadId();
        if (other && other != self && AttachThreadInput(self, other, TRUE))
        {
            SetForegroundWindow(m_hwnd);
            SetFocus(m_hwnd);
            AttachThreadInput(self, other, FALSE);
        }
    }
    SetFocus(m_hwnd);
}

//****************************************************************************
//	Method:		ScrollItemIntoView
//	FullName:	McFenceWnd::ScrollItemIntoView
//	author:
//	date:		2026/11/07
//	Function:   调整滚动位置，让指定项目完整出现在内容区里（新建的项目可能在可见区域之外）。
//
//	Returns:	void
//	Parameter:	IN  const std::wstring& path    项目路径
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::ScrollItemIntoView(const std::wstring& path)
{
    RefreshRendered();
    for (size_t i = 0; i < m_rendered.size(); ++i)
    {
        if (!McIEquals(m_rendered[i].item.path, path))
        {
            continue;
        }
        RECT client;
        GetClientRect(m_hwnd, &client);
        const RECT bounds = ItemBounds(static_cast<int>(i));
        if (bounds.top < m_title_height)
        {
            m_scroll_offset -= m_title_height - bounds.top;
        }
        else if (bounds.bottom > client.bottom)
        {
            m_scroll_offset += bounds.bottom - client.bottom;
        }
        if (m_scroll_offset < 0)
        {
            m_scroll_offset = 0;
        }
        return;
    }
}

//****************************************************************************
//	Method:		BeginItemRename
//	FullName:	McFenceWnd::BeginItemRename
//	author:
//	date:		2026/11/07
//	Function:   原地重命名一个项目（与 openFrence 一致：不弹编辑框，直接在图标标签上编辑）。
//              文件的主文件名处于“全选”状态、扩展名固定不动；文件夹没有扩展名。
//
//	Returns:	void
//	Parameter:	IN  const std::wstring& path    要重命名的项目
//
//	changelog:  2026/11/07 由弹出对话框改为在围栏表面上原地编辑
//****************************************************************************
void McFenceWnd::BeginItemRename(const std::wstring& path)
{
    if (path.empty() || !McPathExists(path))
    {
        return;
    }
    EndRename(true);
    if (m_is_minified)
    {
        unminify();
    }
    const std::wstring name = McFileName(path);
    m_rename_ext.clear();
    if (!McDirectoryExists(path))
    {
        const std::wstring extension = McFileExtension(name);
        // ".gitignore" 这类只有扩展名的名字：整个名字都可编辑，和资源管理器一致。
        if (!extension.empty() && extension.size() < name.size())
        {
            m_rename_ext = extension;
        }
    }
    m_rename_text = name.substr(0, name.size() - m_rename_ext.size());
    m_rename_path = path;
    m_rename_target = MCFENCE_RENAME_ITEM;
    SelectOnly(path);
    ScrollItemIntoView(path);
    StartRenameSession();
}

//****************************************************************************
//	Method:		BeginTitleRename
//	FullName:	McFenceWnd::BeginTitleRename
//	author:
//	date:		2026/11/07
//	Function:   原地编辑栅栏名称（双击标题栏 / 菜单「重命名栅栏」），与项目重命名共用同一套编辑逻辑。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  2026/11/07 由覆盖在标题栏上的 EDIT 窗口改为在围栏表面上原地编辑
//****************************************************************************
void McFenceWnd::BeginTitleRename()
{
    EndRename(true);
    if (m_is_minified)
    {
        unminify();
    }
    m_rename_text = m_info.name;
    m_rename_ext.clear();
    m_rename_path.clear();
    m_rename_target = MCFENCE_RENAME_TITLE;
    StartRenameSession();
}

//****************************************************************************
//	Method:		StartRenameSession
//	FullName:	McFenceWnd::StartRenameSession
//	author:
//	date:		2026/11/07
//	Function:   原地编辑的公共起始步骤：全选状态、光标放末尾、启动光标闪烁、拿键盘焦点、重绘。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::StartRenameSession()
{
    m_rename_caret = m_rename_text.size();
    m_rename_pristine = true;
    m_rename_caret_on = true;
    m_rename_scroll = 0.0f;
    SetTimer(m_hwnd, timer_rename_caret, GetCaretBlinkTime(), NULL);
    TakeFocus();
    Invalidate();
}

//****************************************************************************
//	Method:		EndRename
//	FullName:	McFenceWnd::EndRename
//	author:
//	date:		2026/11/07
//	Function:   结束原地编辑。commit 时：标题直接改名并保存；项目在同目录内 MoveFileW 改名（主文件名为空或
//              没变化时什么都不做），失败时提示系统错误信息。
//
//	Returns:	void
//	Parameter:	IN  bool commit    true 保存，false 放弃
//              IN  bool interactive    false 时失败也不弹提示（窗口正在销毁）
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::EndRename(bool commit, bool interactive)
{
    if (!IsRenaming())
    {
        return;
    }
    const int target = m_rename_target;
    const std::wstring text = m_rename_text;
    const std::wstring path = m_rename_path;
    const std::wstring ext = m_rename_ext;
    m_rename_target = MCFENCE_RENAME_NONE;
    m_rename_path.clear();
    if (m_hwnd)
    {
        KillTimer(m_hwnd, timer_rename_caret);
    }

    if (commit && target == MCFENCE_RENAME_TITLE)
    {
        const std::wstring name = trim(text);
        if (!name.empty() && name != m_info.name)
        {
            m_info.name = name;
            SetWindowTextW(m_hwnd, m_info.name.c_str());
            SaveNow();
        }
    }
    else if (commit && target == MCFENCE_RENAME_ITEM && McPathExists(path))
    {
        // 资源管理器会去掉名字末尾的空格和点。
        std::wstring base = text;
        while (!base.empty() && (base[base.size() - 1] == L' ' || base[base.size() - 1] == L'.'))
        {
            base.erase(base.size() - 1);
        }
        while (!base.empty() && base[0] == L' ')
        {
            base.erase(0, 1);
        }
        const std::wstring name = base + ext;
        if (!base.empty() && name != McFileName(path))
        {
            const std::wstring renamed = McJoinPath(McParentDirectory(path), name);
            if (MoveFileW(path.c_str(), renamed.c_str()))
            {
                if (IsMapped())
                {
                    ReloadFolder();
                }
                else
                {
                    for (size_t i = 0; i < m_info.files.size(); ++i)
                    {
                        if (McIEquals(m_info.files[i], path))
                        {
                            m_info.files[i] = renamed;
                        }
                    }
                    McSortPaths(m_info.files, m_info.sort, m_info.sort_descending);
                    SaveNow();
                }
                SelectOnly(renamed);
            }
            else
            {
                const DWORD error = GetLastError();
                McLogWin32(L"rename item", error);
                if (interactive && m_hwnd)
                {
                    wchar_t* message = NULL;
                    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   NULL, error, 0, reinterpret_cast<LPWSTR>(&message), 0, NULL);
                    Invalidate();
                    MessageBoxW(m_hwnd, message ? message : McLoc(L"Rename failed.", L"重命名失败。"), McLoc(L"Rename", L"重命名"),
                                MB_OK | MB_ICONWARNING);
                    if (message)
                    {
                        LocalFree(message);
                    }
                }
            }
        }
    }
    if (m_hwnd)
    {
        Invalidate();
    }
}

//****************************************************************************
//	Method:		RenameInsert
//	FullName:	McFenceWnd::RenameInsert
//	author:
//	date:		2026/11/07
//	Function:   在光标处插入文字（打字、输入法上屏、粘贴）。处于全选状态时先清空主文件名；
//              项目名里不允许出现 \ / : * ? " < > |，遇到就响一声并丢弃该字符。
//
//	Returns:	void
//	Parameter:	IN  const std::wstring& text    要插入的文字
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::RenameInsert(const std::wstring& text)
{
    std::wstring accepted;
    bool rejected = false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const wchar_t ch = text[i];
        if (ch == L'\r' || ch == L'\n')
        {
            break;  // 只取第一行
        }
        if (ch < 32 || (m_rename_target == MCFENCE_RENAME_ITEM && wcschr(L"\\/:*?\"<>|", ch) != NULL))
        {
            rejected = true;
            continue;
        }
        accepted += ch;
    }
    if (rejected)
    {
        MessageBeep(MB_OK);
    }
    if (accepted.empty())
    {
        return;
    }
    if (m_rename_pristine)
    {
        m_rename_text.clear();
        m_rename_caret = 0;
        m_rename_pristine = false;
    }
    const size_t limit = 240;
    if (m_rename_text.size() + accepted.size() > limit)
    {
        accepted.resize(limit > m_rename_text.size() ? limit - m_rename_text.size() : 0);
    }
    m_rename_text.insert(m_rename_caret, accepted);
    m_rename_caret += accepted.size();
    m_rename_caret_on = true;
    Invalidate();
}

//****************************************************************************
//	Method:		RenameChar
//	FullName:	McFenceWnd::RenameChar
//	author:
//	date:		2026/11/07
//	Function:   编辑中的 WM_CHAR：可见字符插入；Ctrl+A 回到全选；Ctrl+V 粘贴剪贴板第一行。
//              退格等编辑键在 RenameKeyDown 里处理。
//
//	Returns:	void
//	Parameter:	IN  wchar_t ch    字符
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::RenameChar(wchar_t ch)
{
    if (ch == 1)  // Ctrl+A
    {
        m_rename_pristine = true;
        m_rename_caret = m_rename_text.size();
        Invalidate();
        return;
    }
    if (ch == 22)  // Ctrl+V
    {
        if (IsClipboardFormatAvailable(CF_UNICODETEXT) && OpenClipboard(m_hwnd))
        {
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            const wchar_t* text = data ? static_cast<const wchar_t*>(GlobalLock(data)) : NULL;
            const std::wstring pasted = text ? std::wstring(text) : std::wstring();
            if (text)
            {
                GlobalUnlock(data);
            }
            CloseClipboard();
            RenameInsert(pasted);
        }
        return;
    }
    if (ch >= 32)
    {
        RenameInsert(std::wstring(1, ch));
    }
}

//****************************************************************************
//	Method:		RenameKeyDown
//	FullName:	McFenceWnd::RenameKeyDown
//	author:
//	date:		2026/11/07
//	Function:   编辑中的按键（资源管理器重命名框的语义，与 openFrence 一致）：Enter 保存、Esc 放弃；
//              全选状态下退格 / Delete 清空，←/→ 只是把光标收到选区的头 / 尾；Home / End 跳到两端。
//
//	Returns:	void
//	Parameter:	IN  WPARAM key    虚拟键码
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::RenameKeyDown(WPARAM key)
{
    std::wstring& text = m_rename_text;
    size_t& caret = m_rename_caret;
    switch (key)
    {
        case VK_RETURN:
            EndRename(true);
            return;
        case VK_ESCAPE:
            EndRename(false);
            return;
        case VK_BACK:
            if (m_rename_pristine)
            {
                text.clear();
                caret = 0;
                m_rename_pristine = false;
            }
            else if (caret > 0)
            {
                text.erase(caret - 1, 1);
                --caret;
            }
            break;
        case VK_DELETE:
            if (m_rename_pristine)
            {
                text.clear();
                caret = 0;
                m_rename_pristine = false;
            }
            else if (caret < text.size())
            {
                text.erase(caret, 1);
            }
            break;
        case VK_LEFT:
            if (m_rename_pristine)
            {
                m_rename_pristine = false;
                caret = 0;
            }
            else if (caret > 0)
            {
                --caret;
            }
            break;
        case VK_RIGHT:
            if (m_rename_pristine)
            {
                m_rename_pristine = false;
                caret = text.size();
            }
            else if (caret < text.size())
            {
                ++caret;
            }
            break;
        case VK_HOME:
            m_rename_pristine = false;
            caret = 0;
            break;
        case VK_END:
            m_rename_pristine = false;
            caret = text.size();
            break;
        default:
            return;  // 其他键（字符由 WM_CHAR 处理）不改变状态
    }
    m_rename_caret_on = true;
    Invalidate();
}

namespace
{

// 编辑文字的排版：紧排、不换行、计入末尾空格，这样“前缀宽度”就是光标的准确位置。
float MeasureEditText(Gdiplus::Graphics& g, Gdiplus::Font* font, const std::wstring& text)
{
    if (!font || text.empty())
    {
        return 0.0f;
    }
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces |
                          Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::RectF bounds;
    g.MeasureString(text.c_str(), static_cast<INT>(text.size()), font, Gdiplus::PointF(0.0f, 0.0f), &format,
                    &bounds);
    return bounds.Width;
}

void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius)
{
    const float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

}  // namespace

//****************************************************************************
//	Method:		DrawRenameEditor
//	FullName:	McFenceWnd::DrawRenameEditor
//	author:
//	date:		2026/11/07
//	Function:   把原地编辑画在围栏表面上（openFrence 的样式）：深色圆角底 + 强调色描边，宽度随文字变化；
//              全选状态时主文件名带选区底色；光标按闪烁相位绘制。文字比框宽时水平平移，保证光标可见。
//              在所有项目之后绘制，这样编辑框可以压住相邻图标。
//
//	Returns:	void
//	Parameter:	IN OUTGdiplus::Graphics& g    绘制目标
//              IN  int client_width    客户区宽度
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::DrawRenameEditor(Gdiplus::Graphics& g, int client_width)
{
    if (!IsRenaming())
    {
        return;
    }
    Gdiplus::RectF anchor;
    Gdiplus::Font* font = NULL;
    bool center_vertical = false;
    if (m_rename_target == MCFENCE_RENAME_TITLE)
    {
        anchor = Gdiplus::RectF(0.0f, static_cast<float>(m_title_offset), static_cast<float>(client_width),
                                static_cast<float>(m_title_height - m_title_offset));
        font = m_title_font;
        center_vertical = true;
    }
    else
    {
        int visual = -1;
        for (size_t i = 0; i < m_rendered.size(); ++i)
        {
            if (McIEquals(m_rendered[i].item.path, m_rename_path))
            {
                visual = static_cast<int>(i);
                break;
            }
        }
        if (visual < 0)
        {
            return;
        }
        const RECT bounds = ItemBounds(visual);
        anchor = Gdiplus::RectF(static_cast<float>(bounds.left + scaled(2)),
                                static_cast<float>(bounds.top + scaled(2) + m_icon_box + scaled(3)),
                                static_cast<float>(m_item_width), static_cast<float>(m_text_height));
        font = m_icon_font;
    }
    if (!font)
    {
        return;
    }

    const std::wstring full = m_rename_text + m_rename_ext;
    const float pad = scaled_f(4.0f);
    const float margin = scaled_f(4.0f);
    const float line = font->GetHeight(&g);
    const float text_width = MeasureEditText(g, font, full);
    const float box_height = line + scaled_f(4.0f);
    float box_width = max(text_width + pad * 2.0f + scaled_f(2.0f), static_cast<float>(scaled(40)));
    box_width = min(box_width, static_cast<float>(client_width) - margin * 2.0f);
    float box_x = anchor.X + (anchor.Width - box_width) / 2.0f;
    box_x = max(margin, min(box_x, static_cast<float>(client_width) - margin - box_width));
    const float box_y = center_vertical ? anchor.Y + (anchor.Height - box_height) / 2.0f : anchor.Y;
    const Gdiplus::RectF box(box_x, box_y, box_width, box_height);

    // 文字放得下就居中；放不下就平移，让光标留在框里。
    const float visible = box_width - pad * 2.0f;
    const float caret_offset = MeasureEditText(g, font, m_rename_text.substr(0, m_rename_caret));
    float origin;
    if (text_width <= visible)
    {
        m_rename_scroll = 0.0f;
        origin = box_x + (box_width - text_width) / 2.0f;
    }
    else
    {
        if (caret_offset - m_rename_scroll > visible)
        {
            m_rename_scroll = caret_offset - visible;
        }
        if (caret_offset < m_rename_scroll)
        {
            m_rename_scroll = caret_offset;
        }
        m_rename_scroll = max(0.0f, min(m_rename_scroll, text_width - visible));
        origin = box_x + pad - m_rename_scroll;
    }
    m_rename_box = box;
    m_rename_origin = origin;

    const Gdiplus::GraphicsState state = g.Save();
    {
        Gdiplus::GraphicsPath shape;
        AddRoundRect(shape, box, scaled_f(3.0f));
        Gdiplus::SolidBrush back(Gdiplus::Color(245, 28, 31, 38));
        g.FillPath(&back, &shape);
        Gdiplus::Pen ring(McArgb(235, GetSysColor(COLOR_HIGHLIGHT)), max(1.0f, scaled_f(1.0f)));
        g.DrawPath(&ring, &shape);
    }
    g.SetClip(Gdiplus::RectF(box.X + 1.0f, box.Y + 1.0f, box.Width - 2.0f, box.Height - 2.0f),
              Gdiplus::CombineModeIntersect);
    const float text_y = box_y + (box_height - line) / 2.0f;
    if (m_rename_pristine && !m_rename_text.empty())
    {
        const float selected = MeasureEditText(g, font, m_rename_text);
        Gdiplus::SolidBrush highlight(McArgb(200, GetSysColor(COLOR_HIGHLIGHT)));
        g.FillRectangle(&highlight, origin, text_y, selected, line);
    }
    {
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces |
                              Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::SolidBrush ink(Gdiplus::Color(255, 255, 255, 255));
        g.DrawString(full.c_str(), static_cast<INT>(full.size()), font, Gdiplus::PointF(origin, text_y),
                     &format, &ink);
    }
    const float caret_x = origin + caret_offset;
    m_rename_caret_point.x = static_cast<LONG>(caret_x);
    m_rename_caret_point.y = static_cast<LONG>(text_y);
    if (m_rename_caret_on)
    {
        Gdiplus::Pen caret(Gdiplus::Color(230, 255, 255, 255), max(1.0f, scaled_f(1.0f)));
        g.DrawLine(&caret, caret_x, text_y, caret_x, text_y + line);
    }
    g.Restore(state);
}

//****************************************************************************
//	Method:		RenameCaretFromPoint
//	FullName:	McFenceWnd::RenameCaretFromPoint
//	author:
//	date:		2026/11/07
//	Function:   点击编辑框内部时，把光标放到离点击位置最近的字符边界（只在可编辑部分内）。
//
//	Returns:	void
//	Parameter:	IN  POINT client    点击的客户区坐标
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::RenameCaretFromPoint(POINT client)
{
    if (!m_surface.IsValid())
    {
        return;
    }
    Gdiplus::Graphics& g = m_surface.graphics();
    Gdiplus::Font* font = m_rename_target == MCFENCE_RENAME_TITLE ? m_title_font : m_icon_font;
    const float x = static_cast<float>(client.x) - m_rename_origin;
    size_t best = 0;
    float best_distance = x < 0.0f ? -x : x;
    for (size_t i = 1; i <= m_rename_text.size(); ++i)
    {
        const float width = MeasureEditText(g, font, m_rename_text.substr(0, i));
        const float distance = width > x ? width - x : x - width;
        if (distance < best_distance)
        {
            best_distance = distance;
            best = i;
        }
    }
    m_rename_caret = best;
    m_rename_pristine = false;
    m_rename_caret_on = true;
    Invalidate();
}

//****************************************************************************
//	Method:		PlaceImeWindow
//	FullName:	McFenceWnd::PlaceImeWindow
//	author:
//	date:		2026/11/07
//	Function:   把输入法的组字窗口放到编辑光标处（否则中文候选框会出现在屏幕左上角）。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::PlaceImeWindow()
{
    if (!IsRenaming())
    {
        return;
    }
    HIMC context = ImmGetContext(m_hwnd);
    if (!context)
    {
        return;
    }
    COMPOSITIONFORM form;
    ZeroMemory(&form, sizeof(form));
    form.dwStyle = CFS_POINT;
    form.ptCurrentPos = m_rename_caret_point;
    ImmSetCompositionWindow(context, &form);
    ImmReleaseContext(m_hwnd, context);
}

//****************************************************************************
//	Method:		ShowFenceMenu
//	FullName:	McFenceWnd::ShowFenceMenu
//	author:
//	date:		2026/11/07
//	Function:   在指定屏幕坐标弹出右键菜单。Shift+右键项目：该文件的完整 Shell 菜单；右键项目：
//              打开 / 打开所在位置 / 重命名 / 移除或删除；右键空白：资源管理器「新建」子菜单、排序、
//              全选、锁定、折叠、重命名、新建 / 移除栅栏、选项、关于。
//
//	Returns:	void
//	Parameter:	IN  POINT screen    菜单弹出的屏幕坐标
//
//	changelog:  2026/11/07 多选、Shell「新建」、排序、映射文件夹、选项
//****************************************************************************
void McFenceWnd::ShowFenceMenu(POINT screen)
{
    POINT client = screen;
    ScreenToClient(m_hwnd, &client);
    const std::wstring hit = client.y >= m_title_height ? HitTestItem(client) : std::wstring();

    if (!hit.empty() && (GetKeyState(VK_SHIFT) & 0x8000) != 0)
    {
        // Shift+right click: the real shell menu for that file.
        SelectOnly(hit);
        Invalidate();
        m_shell_menu.Show(hit, screen);
        if (IsMapped())
        {
            ReloadFolder();
        }
        return;
    }

    // 右键已选中的项目时作用于整个选择，否则只选中它。
    if (!hit.empty() && !IsSelected(hit))
    {
        SelectOnly(hit);
    }
    else if (hit.empty())
    {
        m_selection.clear();
    }
    Invalidate();
    const std::vector<std::wstring> items = hit.empty() ? std::vector<std::wstring>() : SelectedInOrder();

    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        return;
    }
    if (!items.empty())
    {
        const UINT single = items.size() == 1 ? MF_STRING : (MF_STRING | MF_GRAYED);
        AppendMenuW(menu, MF_STRING, IDM_MCFENCE_OPEN_ITEM, McLoc(L"&Open", L"打开(&O)"));
        SetMenuDefaultItem(menu, IDM_MCFENCE_OPEN_ITEM, FALSE);
        AppendMenuW(menu, single, IDM_MCFENCE_OPEN_FOLDER, McLoc(L"Open file &location", L"打开所在位置(&L)"));
        AppendMenuW(menu, single, IDM_MCFENCE_SHELL_MENU, McLoc(L"&More options (Shift+right-click)...", L"更多选项(Shift+右键)(&M)..."));
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, single, IDM_MCFENCE_RENAME_ITEM, McLoc(L"Rena&me\tF2", L"重命名(&M)\tF2"));
        if (IsMapped())
        {
            AppendMenuW(menu, MF_STRING, IDM_MCFENCE_RECYCLE_ITEM, McLoc(L"&Delete\tDel", L"删除(&D)\tDel"));
        }
        else
        {
            AppendMenuW(menu, MF_STRING, IDM_MCFENCE_DELETE_ITEM, McLoc(L"Remove from fence(&D)\tDel", L"从栅栏移除(&D)\tDel"));
        }
    }
    else
    {
        // 资源管理器自己的「新建」子菜单：映射围栏建在文件夹里，普通围栏建在一个新的 Items 子目录里。
        std::wstring new_folder = m_info.folder;
        if (!IsMapped() && !m_info.locked)
        {
            m_new_menu_container = McStoreNewItemContainer();
            new_folder = m_new_menu_container;
        }
        if (!m_info.locked && !new_folder.empty() &&
            m_new_menu.Attach(menu, 0, new_folder, IDM_MCFENCE_NEWMENU_FIRST, IDM_MCFENCE_NEWMENU_LAST))
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        }

        HMENU sort = CreatePopupMenu();
        if (sort)
        {
            const int mode = m_info.sort;
            if (!IsMapped())
            {
                AppendMenuW(sort, MF_STRING, IDM_MCFENCE_SORT_MANUAL, McLoc(L"&Manual", L"手动排列(&M)"));
                AppendMenuW(sort, MF_SEPARATOR, 0, NULL);
            }
            AppendMenuW(sort, MF_STRING, IDM_MCFENCE_SORT_NAME, McLoc(L"&Name", L"名称(&N)"));
            AppendMenuW(sort, MF_STRING, IDM_MCFENCE_SORT_DATE, McLoc(L"&Date modified", L"修改日期(&D)"));
            AppendMenuW(sort, MF_STRING, IDM_MCFENCE_SORT_SIZE, McLoc(L"&Size", L"大小(&S)"));
            AppendMenuW(sort, MF_STRING, IDM_MCFENCE_SORT_TYPE, McLoc(L"&Type", L"类型(&T)"));
            AppendMenuW(sort, MF_SEPARATOR, 0, NULL);
            AppendMenuW(sort, MF_STRING, IDM_MCFENCE_SORT_ASC, McLoc(L"&Ascending", L"递增(&A)"));
            AppendMenuW(sort, MF_STRING, IDM_MCFENCE_SORT_DESC, McLoc(L"D&escending", L"递减(&E)"));
            CheckMenuRadioItem(sort, IDM_MCFENCE_SORT_MANUAL, IDM_MCFENCE_SORT_TYPE,
                               mode == MCFENCE_SORT_NONE ? IDM_MCFENCE_SORT_MANUAL
                                                         : IDM_MCFENCE_SORT_NAME + mode - 1,
                               MF_BYCOMMAND);
            CheckMenuRadioItem(sort, IDM_MCFENCE_SORT_ASC, IDM_MCFENCE_SORT_DESC,
                               m_info.sort_descending ? IDM_MCFENCE_SORT_DESC : IDM_MCFENCE_SORT_ASC,
                               MF_BYCOMMAND);
            if (mode == MCFENCE_SORT_NONE)
            {
                EnableMenuItem(sort, IDM_MCFENCE_SORT_ASC, MF_BYCOMMAND | MF_GRAYED);
                EnableMenuItem(sort, IDM_MCFENCE_SORT_DESC, MF_BYCOMMAND | MF_GRAYED);
            }
            AppendMenuW(menu, MF_POPUP | (m_info.locked && !IsMapped() ? MF_GRAYED : 0),
                        reinterpret_cast<UINT_PTR>(sort), McLoc(L"&Sort by", L"排序方式(&S)"));
        }
        AppendMenuW(menu, MF_STRING, IDM_MCFENCE_SELECT_ALL, McLoc(L"Select &all\tCtrl+A", L"全选(&A)\tCtrl+A"));
        if (IsMapped())
        {
            AppendMenuW(menu, MF_STRING, IDM_MCFENCE_OPEN_MAPPED, McLoc(L"Open mapped &folder", L"打开映射的文件夹(&F)"));
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | (m_info.locked ? MF_CHECKED : MF_UNCHECKED), IDM_MCFENCE_LOCK,
                    McLoc(L"Loc&k", L"锁定(&K)"));
        AppendMenuW(menu, MF_STRING | (m_info.can_minify ? MF_CHECKED : MF_UNCHECKED), IDM_MCFENCE_MINIFY,
                    McLoc(L"Auto-&collapse", L"自动折叠(&C)"));
        AppendMenuW(menu, MF_STRING, IDM_MCFENCE_RENAME, McLoc(L"&Rename fence", L"重命名栅栏(&R)"));
        AppendMenuW(menu, MF_STRING, IDM_MCFENCE_TITLE_SIZE, McLoc(L"Title bar &height", L"标题栏高度(&H)"));
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, IDM_MCFENCE_NEW_FENCE, McLoc(L"&New fence", L"新建栅栏(&N)"));
        AppendMenuW(menu, MF_STRING, IDM_MCFENCE_NEW_MAPPED, McLoc(L"New folder &portal fence...", L"新建文件夹映射栅栏(&P)..."));
        AppendMenuW(menu, MF_STRING, IDM_MCFENCE_REMOVE_FENCE, McLoc(L"Remove fence(&X)", L"移除栅栏(&X)"));
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, IDM_MCFENCE_SETTINGS, McLoc(L"Op&tions...", L"选项(&T)..."));
        AppendMenuW(menu, MF_STRING, IDM_ABOUT, McLoc(L"A&bout McFence...", L"关于 McFence(&B)..."));
    }

    SetForegroundWindow(m_hwnd);
    // 菜单期间 WM_INITMENUPOPUP 等消息到 m_hwnd，由 handle() 转给「新建」处理程序。
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                                          screen.x, screen.y, m_hwnd, NULL);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    if (command != 0 && m_new_menu.Owns(command))
    {
        CreateFromNewMenu(command);
    }
    else if (command != 0)
    {
        RunFenceCommand(command, items);
    }
    // 处理程序在命令执行完之前要保留菜单，所以最后再销毁。
    DestroyMenu(menu);
    m_new_menu.Release();
    if (!m_new_menu_container.empty())
    {
        RemoveDirectoryW(m_new_menu_container.c_str());  // 没用上：只删空目录
        m_new_menu_container.clear();
    }
    if (m_hwnd)
    {
        Invalidate();
    }
}

//****************************************************************************
//	Method:		RunFenceCommand
//	FullName:	McFenceWnd::RunFenceCommand
//	author:
//	date:		2026/11/07
//	Function:   执行右键菜单命令（项目命令作用于 items；围栏命令作用于本围栏）。
//
//	Returns:	void
//	Parameter:	IN  UINT command    菜单命令 ID
//              IN  const std::vector<std::wstring>& items    右键时选中的项目（可为空）
//
//              移除围栏时通过 PostMessageW(WM_CLOSE) 延迟到消息循环中关闭，避免在本函数内释放自身
//
//	changelog:  2026/11/07 多选、排序、映射文件夹、选项
//****************************************************************************
void McFenceWnd::RunFenceCommand(UINT command, const std::vector<std::wstring>& items)
{
    switch (command)
    {
        case IDM_MCFENCE_OPEN_ITEM:
            for (size_t i = 0; i < items.size(); ++i)
            {
                OpenFenceItem(items[i]);
            }
            break;
        case IDM_MCFENCE_OPEN_FOLDER:
            if (!items.empty())
            {
                McLocateInExplorerAsync(items[0]);
            }
            break;
        case IDM_MCFENCE_SHELL_MENU:
            if (!items.empty())
            {
                POINT cursor;
                GetCursorPos(&cursor);
                m_shell_menu.Show(items[0], cursor);
                if (IsMapped())
                {
                    ReloadFolder();
                }
            }
            break;
        case IDM_MCFENCE_RENAME_ITEM:
            if (items.size() == 1)
            {
                BeginItemRename(items[0]);
            }
            break;
        case IDM_MCFENCE_DELETE_ITEM:
            RemoveRecords(items);
            break;
        case IDM_MCFENCE_RECYCLE_ITEM:
            RecycleItems(items);
            break;
        case IDM_MCFENCE_SELECT_ALL:
            SelectAll();
            Invalidate();
            break;
        case IDM_MCFENCE_SORT_MANUAL:
            ApplySort(MCFENCE_SORT_NONE, false);
            break;
        case IDM_MCFENCE_SORT_NAME:
        case IDM_MCFENCE_SORT_DATE:
        case IDM_MCFENCE_SORT_SIZE:
        case IDM_MCFENCE_SORT_TYPE:
        {
            const int mode = static_cast<int>(command - IDM_MCFENCE_SORT_NAME) + MCFENCE_SORT_NAME;
            // 日期、大小默认从新到旧 / 从大到小，和资源管理器一致。
            const bool descending = (mode == MCFENCE_SORT_DATE || mode == MCFENCE_SORT_SIZE);
            ApplySort(mode, m_info.sort == mode ? m_info.sort_descending : descending);
            break;
        }
        case IDM_MCFENCE_SORT_ASC:
        case IDM_MCFENCE_SORT_DESC:
            ApplySort(m_info.sort, command == IDM_MCFENCE_SORT_DESC);
            break;
        case IDM_MCFENCE_OPEN_MAPPED:
            if (IsMapped())
            {
                McOpenPathAsync(m_info.folder);
            }
            break;
        case IDM_MCFENCE_LOCK:
            m_info.locked = !m_info.locked;
            SaveNow();
            Invalidate();
            break;
        case IDM_MCFENCE_MINIFY:
            m_info.can_minify = !m_info.can_minify;
            if (!m_info.can_minify)
            {
                unminify();
            }
            SaveNow();
            Invalidate();
            break;
        case IDM_MCFENCE_RENAME:
            BeginTitleRename();
            break;
        case IDM_MCFENCE_TITLE_SIZE:
        {
            int height = 0;
            if (McShowHeightDialog(m_hwnd, m_info.title_height, height))
            {
                if (height < 16 || height > 100)
                {
                    height = 35;
                }
                m_info.title_height = height;
                m_logical_title_height = height;
                ApplyTitleHeight();
                SaveNow();
            }
            break;
        }
        case IDM_MCFENCE_NEW_FENCE:
            McFenceCreateFence(m_hwnd, false);
            break;
        case IDM_MCFENCE_NEW_MAPPED:
            McFenceCreateFence(m_hwnd, true);
            break;
        case IDM_MCFENCE_SETTINGS:
            McFenceShowSettings(m_hwnd);
            break;
        case IDM_ABOUT:
            McFenceShowAbout(m_hwnd);
            break;
        case IDM_MCFENCE_REMOVE_FENCE:
        {
            const wchar_t* question = IsMapped()
                                          ? McLoc(L"Remove this fence?\nThe mapped folder and its files will not be deleted.", L"确定移除这个栅栏吗？\n映射的文件夹及其中的文件不会被删除。")
                                          : McLoc(L"Remove this fence?\nThe item copies inside the fence will be moved to the Recycle Bin.", L"确定移除这个栅栏吗？\n栅栏里的项目副本会一并删除到回收站。");
            const INT_PTR answer = MessageBoxW(m_hwnd, question, McLoc(L"Remove Fence", L"移除栅栏"), MB_YESNO | MB_ICONQUESTION);
            if (answer == IDYES)
            {
                if (!IsMapped())
                {
                    std::vector<std::wstring> managed;
                    for (size_t i = 0; i < m_info.files.size(); ++i)
                    {
                        if (McStoreIsManagedItem(m_info.files[i]) && McPathExists(m_info.files[i]))
                        {
                            managed.push_back(m_info.files[i]);
                        }
                    }
                    McStoreRecycleManaged(managed);
                }
                McStoreRemoveFence(m_info);
                m_removed = true;
                m_allow_close = true;
                // Destroying here would free |this| while ShowFenceMenu still runs;
                // close from the message loop instead.
                PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
            }
            break;
        }
        default:
            break;
    }
}

//****************************************************************************
//	Method:		DragCursor
//	FullName:	McFenceWnd::DragCursor
//	author:
//	date:		2026/11/06
//	Function:   根据当前位置是否可放置返回对应的拖拽反馈光标，取自 ole32.dll 的“移动”或“禁止放置”光标资源，加载失败时退回系统默认光标。
//
//	Returns:	HCURSOR    可放置时返回“移动”光标句柄，否则返回“禁止放置”光标句柄。
//	Parameter:	IN  bool can_drop    当前拖拽位置是否允许放置
//
//	changelog:  New
//****************************************************************************
// Drag feedback cursors from ole32.dll: resource 1 is "no drop", 2 is "move".
HCURSOR McFenceWnd::DragCursor(bool can_drop)
{
    static HCURSOR no_drop = NULL;
    static HCURSOR move = NULL;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        HMODULE ole = GetModuleHandleW(L"ole32.dll");
        if (ole)
        {
            no_drop = LoadCursorW(ole, MAKEINTRESOURCEW(1));
            move = LoadCursorW(ole, MAKEINTRESOURCEW(2));
        }
        if (!no_drop)
        {
            no_drop = LoadCursorW(NULL, MAKEINTRESOURCEW(32648));  // IDC_NO
        }
        if (!move)
        {
            move = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        }
    }
    return can_drop ? move : no_drop;
}

//****************************************************************************
//	Method:		StartInternalDrag
//	FullName:	McFenceWnd::StartInternalDrag
//	author:
//	date:		2026/11/07
//	Function:   开始拖动：拖的是当前选择（按下的项目未选中时只拖它）。映射围栏不能调整顺序，
//              但同样可以拖到别的围栏或拖出到资源管理器。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  2026/11/07 多选拖动
//****************************************************************************
void McFenceWnd::StartInternalDrag()
{
    if (m_drag_in_progress || m_drag_candidate.empty())
    {
        return;
    }
    if (!IsSelected(m_drag_candidate))
    {
        SelectOnly(m_drag_candidate);
    }
    m_drag_set = SelectedInOrder();
    if (m_drag_set.empty())
    {
        m_drag_set.push_back(m_drag_candidate);
    }
    m_collapse_on_up = false;
    m_drag_in_progress = true;
    SetCapture(m_hwnd);
}

//****************************************************************************
//	Method:		UpdateInternalDrag
//	FullName:	McFenceWnd::UpdateInternalDrag
//	author:
//	date:		2026/11/07
//	Function:   拖动中：鼠标在某个可接收的围栏上时显示插入线；离开所有围栏时转为系统 OLE 拖拽，
//              之后由资源管理器 / 桌面 / 其他程序决定放到哪里。
//
//	Returns:	void
//	Parameter:	IN  POINT screen    当前鼠标屏幕坐标
//
//	changelog:  2026/11/07 拖出到资源管理器
//****************************************************************************
void McFenceWnd::UpdateInternalDrag(POINT screen)
{
    if (!m_drag_in_progress)
    {
        return;
    }
    McFenceWnd* over = FenceAtPoint(screen);
    if (!over)
    {
        StartDragOut();
        return;
    }
    McFenceWnd* target = over;
    // 锁定的围栏不接收；映射围栏内部不能调整顺序，但可以接收其他围栏的文件。
    if (target->m_info.locked || (target == this && IsMapped()))
    {
        target = NULL;
    }
    // The capture window gets no WM_SETCURSOR, so set the OLE drag cursors here.
    SetCursor(DragCursor(target != NULL));
    for (size_t i = 0; i < s_registry.size(); ++i)
    {
        if (s_registry[i] != target)
        {
            s_registry[i]->SetDropIndex(-1);
        }
    }
    if (!target)
    {
        return;
    }
    POINT client = screen;
    ScreenToClient(target->m_hwnd, &client);
    target->SetDropIndex(target->IsMapped() ? 0 : target->DropInsertIndex(client));
}

//****************************************************************************
//	Method:		StartDragOut
//	FullName:	McFenceWnd::StartDragOut
//	author:
//	date:		2026/11/07
//	Function:   结束鼠标捕获式的内部拖动，改用 DoDragDrop 把选中项目作为文件拖出去。
//              默认移动（按住 Ctrl 复制，和拖入一致）；移走的项目从围栏中去掉。
//
//	Returns:	void
//	Parameter:	无
//
//              DoDragDrop 阻塞期间有窗口消息，本围栏可能被关闭，所以先把要用的东西复制到栈上
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::StartDragOut()
{
    const std::vector<std::wstring> paths = m_drag_set;
    m_drag_in_progress = false;
    if (GetCapture() == m_hwnd)
    {
        ReleaseCapture();
    }
    for (size_t i = 0; i < s_registry.size(); ++i)
    {
        s_registry[i]->SetDropIndex(-1);
    }
    m_drag_candidate.clear();
    m_drag_set.clear();

    std::vector<std::wstring> existing;
    for (size_t i = 0; i < paths.size(); ++i)
    {
        if (McPathExists(paths[i]))
        {
            existing.push_back(paths[i]);
        }
    }
    if (existing.empty())
    {
        return;
    }

    s_outgoing_source = this;
    s_outgoing_paths = existing;
    s_outgoing_handled = false;
    HWND hwnd = m_hwnd;
    const bool mapped = IsMapped();
    const DWORD effect = McDoDragOut(existing, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    s_outgoing_source = NULL;
    s_outgoing_paths.clear();
    if (s_outgoing_handled || !IsWindow(hwnd))
    {
        return;  // 落回了某个围栏，已按内部移动处理（或本围栏已关闭）
    }

    // 资源管理器在同一分区上做“优化移动”时，返回值可能是 NONE：以文件是否还在为准。
    bool any_gone = false;
    for (size_t i = 0; i < existing.size(); ++i)
    {
        if (!McPathExists(existing[i]))
        {
            any_gone = true;
            break;
        }
    }
    McLogFormat(L"拖出 %u 项：effect=0x%X moved=%d", static_cast<unsigned>(existing.size()), effect,
                any_gone ? 1 : 0);
    if (mapped)
    {
        ReloadFolder();
    }
    else if (any_gone)
    {
        std::vector<std::wstring> gone;
        for (size_t i = 0; i < existing.size(); ++i)
        {
            if (!McPathExists(existing[i]))
            {
                gone.push_back(existing[i]);
            }
        }
        RemoveRecords(gone);
        // 副本被移走后 Items\<guid32>\ 容器变空：顺手删掉（只删空目录）。
        for (size_t i = 0; i < gone.size(); ++i)
        {
            if (McStoreIsManagedItem(gone[i]))
            {
                RemoveDirectoryW(McParentDirectory(gone[i]).c_str());
            }
        }
    }
    Invalidate();
}

//****************************************************************************
//	Method:		FinishInternalDrag
//	FullName:	McFenceWnd::FinishInternalDrag
//	author:
//	date:		2026/11/07
//	Function:   结束内部拖动：释放鼠标捕获；未取消且落在可接收的围栏上时移动项目。
//
//	Returns:	void
//	Parameter:	IN  POINT screen    拖拽结束时的鼠标屏幕坐标
//              IN  bool cancel    是否为取消拖拽（如按 Esc 或失去鼠标捕获）
//
//	changelog:  2026/11/07 多选
//****************************************************************************
void McFenceWnd::FinishInternalDrag(POINT screen, bool cancel)
{
    if (!m_drag_in_progress)
    {
        return;
    }
    m_drag_in_progress = false;
    if (GetCapture() == m_hwnd)
    {
        ReleaseCapture();
    }
    SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32512)));
    McFenceWnd* target = cancel ? NULL : FenceAtPoint(screen);
    if (target && (target->m_info.locked || (target == this && IsMapped())))
    {
        target = NULL;
    }
    const std::vector<std::wstring> dragged = m_drag_set;
    for (size_t i = 0; i < s_registry.size(); ++i)
    {
        s_registry[i]->SetDropIndex(-1);
    }
    m_drag_candidate.clear();
    m_drag_set.clear();
    if (target && !dragged.empty())
    {
        POINT client = screen;
        ScreenToClient(target->m_hwnd, &client);
        const int index = target->IsMapped() ? 0 : target->DropInsertIndex(client);
        MoveFenceItems(this, target, dragged, index);
    }
}

//****************************************************************************
//	Method:		FenceAtPoint
//	FullName:	McFenceWnd::FenceAtPoint
//	author:
//	date:		2026/11/06
//	Function:   沿窗口链查找屏幕坐标点下方属于围栏窗口类、且仍在全局注册表中的 McFenceWnd 实例。
//
//	Returns:	McFenceWnd*    命中的围栏窗口实例指针；未命中时为 NULL。
//	Parameter:	IN  POINT screen    屏幕坐标点
//
//	changelog:  New
//****************************************************************************
McFenceWnd* McFenceWnd::FenceAtPoint(POINT screen) const
{
    POINT target = screen;
    HWND hwnd = WindowFromPoint(target);
    while (hwnd)
    {
        if (GetWindowLongPtrW(hwnd, GWLP_USERDATA))
        {
            wchar_t name[64];
            GetClassNameW(hwnd, name, 64);
            if (wcscmp(name, k_fence_class) == 0)
            {
                McFenceWnd* view = reinterpret_cast<McFenceWnd*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                for (size_t i = 0; i < s_registry.size(); ++i)
                {
                    if (s_registry[i] == view)
                    {
                        return view;
                    }
                }
            }
        }
        // 围栏挂在 SHELLDLL_DefView 名下时 GetParent 会返回所有者，这里只沿子窗口链向上走。
        if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) == 0)
        {
            break;
        }
        hwnd = GetParent(hwnd);
    }
    return NULL;
}

//****************************************************************************
//	Method:		MoveFenceItems
//	FullName:	McFenceWnd::MoveFenceItems
//	author:
//	date:		2026/11/07
//	Function:   在围栏之间移动一组项目（或在同一普通围栏内重新排列）。
//              普通 → 普通：只移动记录；涉及映射围栏时移动真实文件（SHFileOperation，可撤销）：
//                * 普通 → 映射：Items 副本移进文件夹，源围栏去掉记录；
//                * 映射 → 普通：真实文件移进 Items 仓库（和拖入外部文件一致）；
//                * 映射 → 映射：真实文件移到目标文件夹。
//
//	Returns:	void
//	Parameter:	IN OUTMcFenceWnd* source    源围栏
//              IN OUTMcFenceWnd* target    目标围栏
//              IN  const std::vector<std::wstring>& paths    要移动的项目（按显示顺序）
//              IN  int insert_index    目标列表中的插入位置（映射目标忽略）
//
//	changelog:  2026/11/07 多选、映射文件夹
//****************************************************************************
void McFenceWnd::MoveFenceItems(McFenceWnd* source, McFenceWnd* target,
                                const std::vector<std::wstring>& paths, int insert_index)
{
    if (!source || !target || paths.empty())
    {
        return;
    }

    if (target->IsMapped())
    {
        if (source->IsMapped() && McIEquals(source->m_info.folder, target->m_info.folder))
        {
            return;
        }
        McShellCopyMove(target->m_hwnd, paths, target->m_info.folder, true);
        {
            // 用户可能在冲突对话框里取消了一部分：以文件是否还在原处为准。
            if (source->IsMapped())
            {
                source->ReloadFolder();
            }
            else
            {
                std::vector<std::wstring> gone;
                for (size_t i = 0; i < paths.size(); ++i)
                {
                    if (!McPathExists(paths[i]))
                    {
                        gone.push_back(paths[i]);
                    }
                }
                for (size_t i = 0; i < gone.size(); ++i)
                {
                    RemovePath(source->m_info.files, gone[i]);
                    RemovePath(source->m_selection, gone[i]);
                }
                if (!gone.empty())
                {
                    source->SaveNow();
                }
                // 移走后 Items\<guid32>\ 容器变空：顺手删掉。
                for (size_t i = 0; i < gone.size(); ++i)
                {
                    if (McStoreIsManagedItem(gone[i]))
                    {
                        RemoveDirectoryW(McParentDirectory(gone[i]).c_str());
                    }
                }
            }
            target->ReloadFolder();
            source->Invalidate();
            target->Invalidate();
        }
        return;
    }

    if (source->IsMapped())
    {
        target->AcceptFileDrop(paths, insert_index, true);
        source->ReloadFolder();
        source->Invalidate();
        return;
    }

    // 普通围栏之间：只搬记录。先从源列表里摘出来（同围栏时修正插入位置）。
    std::vector<std::wstring> moving;
    for (size_t p = 0; p < paths.size(); ++p)
    {
        for (size_t i = 0; i < source->m_info.files.size(); ++i)
        {
            if (McIEquals(source->m_info.files[i], paths[p]))
            {
                if (source == target && insert_index > static_cast<int>(i))
                {
                    --insert_index;
                }
                moving.push_back(source->m_info.files[i]);
                source->m_info.files.erase(source->m_info.files.begin() + i);
                break;
            }
        }
    }
    if (moving.empty())
    {
        return;
    }
    if (source != target)
    {
        for (size_t p = 0; p < moving.size(); ++p)
        {
            for (size_t i = 0; i < target->m_info.files.size(); ++i)
            {
                if (McIEquals(target->m_info.files[i], moving[p]))
                {
                    target->m_info.files.erase(target->m_info.files.begin() + i);
                    if (insert_index > static_cast<int>(i))
                    {
                        --insert_index;
                    }
                    break;
                }
            }
        }
    }
    insert_index = max(0, min(insert_index, static_cast<int>(target->m_info.files.size())));
    target->m_info.files.insert(target->m_info.files.begin() + insert_index, moving.begin(), moving.end());
    // 手动拖动排序后不再是自动排序。
    target->m_info.sort = MCFENCE_SORT_NONE;
    target->m_selection = moving;
    if (source != target)
    {
        source->PruneSelection();
        source->SaveNow();
        source->Invalidate();
    }
    target->SaveNow();
    target->Invalidate();
}

//****************************************************************************
//	Method:		AcceptFileDrop
//	FullName:	McFenceWnd::AcceptFileDrop
//	author:
//	date:		2026/11/07
//	Function:   接收外部拖入的文件。普通围栏：移动（或复制）到 Items 仓库并插入记录；映射围栏：用
//              SHFileOperation 移动（或复制）进映射的文件夹。
//
//	Returns:	void
//	Parameter:	IN  const std::vector<std::wstring>& paths    外部拖放进来的文件路径列表
//              IN  int insert_index    插入新条目的起始模型索引（映射围栏忽略）
//              IN  bool move    移动而不是复制（拖放默认移动，按住 Ctrl 复制）
//
//	changelog:  2026/11/07 映射文件夹；拖入默认移动
//****************************************************************************
void McFenceWnd::AcceptFileDrop(const std::vector<std::wstring>& paths, int insert_index, bool move)
{
    if (IsMapped())
    {
        std::vector<std::wstring> outside;
        for (size_t i = 0; i < paths.size(); ++i)
        {
            if (!McIEquals(McParentDirectory(paths[i]), m_info.folder))
            {
                outside.push_back(paths[i]);
            }
        }
        if (!outside.empty())
        {
            McShellCopyMove(m_hwnd, outside, m_info.folder, move);
            ReloadFolder();
        }
        Invalidate();
        return;
    }

    bool changed = false;
    std::vector<std::wstring> added;
    for (size_t i = 0; i < paths.size(); ++i)
    {
        const std::wstring& path = paths[i];
        bool already = false;
        for (size_t j = 0; j < m_info.files.size(); ++j)
        {
            if (McIEquals(m_info.files[j], path))
            {
                already = true;
                break;
            }
        }
        if (already || !McItemExists(path))
        {
            continue;
        }
        const std::wstring imported = McStoreImportItem(path, move);
        if (imported.empty())
        {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < m_info.files.size(); ++j)
        {
            if (McIEquals(m_info.files[j], imported))
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }
        const int at = max(0, min(insert_index, static_cast<int>(m_info.files.size())));
        m_info.files.insert(m_info.files.begin() + at, imported);
        added.push_back(imported);
        ++insert_index;
        changed = true;
    }
    if (changed)
    {
        McSortPaths(m_info.files, m_info.sort, m_info.sort_descending);
        m_selection = added;
        SaveNow();
    }
    Invalidate();
}

//****************************************************************************
//	Method:		BeginMoveSize
//	FullName:	McFenceWnd::BeginMoveSize
//	author:
//	date:		2026/11/07
//	Function:   开始拖动 / 缩放窗口时记录起点；按住 Ctrl 拖标题时记录所有围栏的位置，用于整体移动。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::BeginMoveSize()
{
    GetWindowRect(m_hwnd, &m_move_origin);
    m_group_origins.clear();
    m_group_move = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (!m_group_move)
    {
        return;
    }
    for (size_t i = 0; i < s_registry.size(); ++i)
    {
        McFenceWnd* other = s_registry[i];
        if (other != this && other->m_hwnd && IsWindowVisible(other->m_hwnd) && !other->m_info.locked)
        {
            RECT rect;
            GetWindowRect(other->m_hwnd, &rect);
            m_group_origins.push_back(std::make_pair(other->m_hwnd, rect));
        }
    }
}

//****************************************************************************
//	Method:		SnapTargets
//	FullName:	McFenceWnd::SnapTargets
//	author:
//	date:		2026/11/07
//	Function:   收集吸附目标：窗口所在显示器的工作区，以及其他可见围栏的矩形（整体移动时不含同组围栏）。
//
//	Returns:	void
//	Parameter:	IN  const RECT& rect    当前窗口矩形
//              OUT RECT& work_area    所在显示器的工作区
//              OUT std::vector<RECT>& others    其他围栏的矩形
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::SnapTargets(const RECT& rect, RECT& work_area, std::vector<RECT>& others) const
{
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info))
    {
        work_area = info.rcWork;
    }
    else
    {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    }
    others.clear();
    if (m_group_move)
    {
        return;  // 整体移动时只吸附屏幕边缘
    }
    for (size_t i = 0; i < s_registry.size(); ++i)
    {
        const McFenceWnd* other = s_registry[i];
        if (other != this && other->m_hwnd && IsWindowVisible(other->m_hwnd))
        {
            RECT r;
            GetWindowRect(other->m_hwnd, &r);
            others.push_back(r);
        }
    }
}

namespace
{

// 一条轴上的吸附：在 |candidates| 里找离 |value| 最近且不超过 |threshold| 的线，返回需要的偏移。
int SnapAxis(int value, const std::vector<int>& candidates, int threshold, int& best)
{
    int delta = 0;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const int d = candidates[i] - value;
        if (abs(d) <= threshold && abs(d) < best)
        {
            best = abs(d);
            delta = d;
        }
    }
    return delta;
}

// 两个区间是否相互靠近（另一个围栏和自己在这条轴上有重叠或相距不远时，才考虑吸附它的边）。
bool Near(int a0, int a1, int b0, int b1, int slack)
{
    return a0 <= b1 + slack && b0 <= a1 + slack;
}

}  // namespace

//****************************************************************************
//	Method:		SnapMoving
//	FullName:	McFenceWnd::SnapMoving
//	author:
//	date:		2026/11/07
//	Function:   WM_MOVING：把窗口吸附到工作区边缘和相邻围栏的边（贴边或对齐，留一个间距）；
//              Ctrl 整体移动时，其他围栏按同样的位移跟着走。
//
//	Returns:	void
//	Parameter:	IN OUTRECT* rect    系统建议的新窗口矩形，原地修改
//
//              吸附算法参考 DeskBox 的 WidgetSnapCalculator，坐标计算改写为 Win32
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::SnapMoving(RECT* rect)
{
    if (McSettingsGet().snap && (GetKeyState(VK_MENU) & 0x8000) == 0)
    {
        RECT area;
        std::vector<RECT> others;
        SnapTargets(*rect, area, others);
        const int threshold = scaled(10);
        const int gap = scaled(8);
        std::vector<int> xs;
        std::vector<int> ys;
        xs.push_back(area.left);
        ys.push_back(area.top);
        const int width = rect->right - rect->left;
        const int height = rect->bottom - rect->top;
        std::vector<int> xs_right;
        std::vector<int> ys_bottom;
        xs_right.push_back(area.right);
        ys_bottom.push_back(area.bottom);
        for (size_t i = 0; i < others.size(); ++i)
        {
            const RECT& o = others[i];
            if (Near(rect->top, rect->bottom, o.top, o.bottom, threshold))
            {
                xs.push_back(o.right + gap);    // 我的左边贴它的右边
                xs.push_back(o.left);           // 左边对齐
                xs_right.push_back(o.left - gap);
                xs_right.push_back(o.right);
            }
            if (Near(rect->left, rect->right, o.left, o.right, threshold))
            {
                ys.push_back(o.bottom + gap);
                ys.push_back(o.top);
                ys_bottom.push_back(o.top - gap);
                ys_bottom.push_back(o.bottom);
            }
        }
        int best_x = threshold + 1;
        int dx = SnapAxis(rect->left, xs, threshold, best_x);
        const int dx_right = SnapAxis(rect->right, xs_right, threshold, best_x);
        if (dx_right != 0)
        {
            dx = dx_right;
        }
        int best_y = threshold + 1;
        int dy = SnapAxis(rect->top, ys, threshold, best_y);
        const int dy_bottom = SnapAxis(rect->bottom, ys_bottom, threshold, best_y);
        if (dy_bottom != 0)
        {
            dy = dy_bottom;
        }
        rect->left += dx;
        rect->top += dy;
        rect->right = rect->left + width;
        rect->bottom = rect->top + height;
    }

    if (m_group_move && !m_group_origins.empty())
    {
        const int dx = rect->left - m_move_origin.left;
        const int dy = rect->top - m_move_origin.top;
        HDWP batch = BeginDeferWindowPos(static_cast<int>(m_group_origins.size()));
        for (size_t i = 0; i < m_group_origins.size(); ++i)
        {
            const RECT& r = m_group_origins[i].second;
            if (batch && IsWindow(m_group_origins[i].first))
            {
                batch = DeferWindowPos(batch, m_group_origins[i].first, NULL, r.left + dx, r.top + dy, 0, 0,
                                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        if (batch)
        {
            EndDeferWindowPos(batch);
        }
    }
}

//****************************************************************************
//	Method:		SnapSizing
//	FullName:	McFenceWnd::SnapSizing
//	author:
//	date:		2026/11/07
//	Function:   WM_SIZING：被拖动的那几条边吸附到工作区边缘和相邻围栏的边。
//
//	Returns:	void
//	Parameter:	IN OUTRECT* rect    系统建议的新窗口矩形，原地修改
//              IN  WPARAM edge    WMSZ_* 正在拖动的边
//
//	changelog:  New
//****************************************************************************
void McFenceWnd::SnapSizing(RECT* rect, WPARAM edge)
{
    if (!McSettingsGet().snap || (GetKeyState(VK_MENU) & 0x8000) != 0)
    {
        return;
    }
    RECT area;
    std::vector<RECT> others;
    SnapTargets(*rect, area, others);
    const int threshold = scaled(10);
    const int gap = scaled(8);
    const int min_size = scaled(80);
    std::vector<int> xs;
    std::vector<int> ys;
    xs.push_back(area.left);
    xs.push_back(area.right);
    ys.push_back(area.top);
    ys.push_back(area.bottom);
    for (size_t i = 0; i < others.size(); ++i)
    {
        const RECT& o = others[i];
        if (Near(rect->top, rect->bottom, o.top, o.bottom, threshold))
        {
            xs.push_back(o.left);
            xs.push_back(o.right);
            xs.push_back(o.left - gap);
            xs.push_back(o.right + gap);
        }
        if (Near(rect->left, rect->right, o.left, o.right, threshold))
        {
            ys.push_back(o.top);
            ys.push_back(o.bottom);
            ys.push_back(o.top - gap);
            ys.push_back(o.bottom + gap);
        }
    }
    int best = threshold + 1;
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
    {
        best = threshold + 1;
        const int left = rect->left + SnapAxis(rect->left, xs, threshold, best);
        rect->left = min(left, static_cast<int>(rect->right) - min_size);
    }
    if (edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT)
    {
        best = threshold + 1;
        const int right = rect->right + SnapAxis(rect->right, xs, threshold, best);
        rect->right = max(right, static_cast<int>(rect->left) + min_size);
    }
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
    {
        best = threshold + 1;
        const int top = rect->top + SnapAxis(rect->top, ys, threshold, best);
        rect->top = min(top, static_cast<int>(rect->bottom) - min_size);
    }
    if (edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT)
    {
        best = threshold + 1;
        const int bottom = rect->bottom + SnapAxis(rect->bottom, ys, threshold, best);
        rect->bottom = max(bottom, static_cast<int>(rect->top) + min_size);
    }
}

//****************************************************************************
//	Method:		McDumpSurface
//	FullName:	McFenceWnd::McDumpSurface
//	author:
//	date:		2026/11/06
//	Function:   将当前分层窗口的合成表面绘制到一张带中性背景色的位图上并保存为 PNG 文件，供验证脚本检查渲染结果。
//
//	Returns:	bool    保存成功返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& path    输出 PNG 文件的路径
//
//	changelog:  New
//****************************************************************************
bool McFenceWnd::McDumpSurface(const std::wstring& path)
{
    Gdiplus::Bitmap* source = m_surface.Image();
    if (!source)
    {
        return false;
    }
    const int width = source->GetWidth();
    const int height = source->GetHeight();
    Gdiplus::Bitmap target(width, height, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&target);
        // Neutral desktop colour: premultiplied pixels over this is what the user sees.
        Gdiplus::SolidBrush background(Gdiplus::Color(255, 58, 64, 74));
        g.FillRectangle(&background, 0, 0, width, height);
        g.DrawImage(source, 0, 0, width, height);
    }
    CLSID encoder;
    if (!PngEncoder(&encoder))
    {
        return false;
    }
    return target.Save(path.c_str(), &encoder, NULL) == Gdiplus::Ok;
}

//****************************************************************************
//	Method:		PngEncoder
//	FullName:	McFenceWnd::PngEncoder
//	author:
//	date:		2026/11/06
//	Function:   枚举系统已安装的 GDI+ 图像编码器，查找 MIME 类型为 image/png 的编码器 CLSID。
//
//	Returns:	bool    找到 PNG 编码器返回 true，否则返回 false。
//	Parameter:	OUT CLSID* out    用于接收找到的 PNG 编码器 CLSID
//
//	changelog:  New
//****************************************************************************
bool McFenceWnd::PngEncoder(CLSID* out)
{
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0)
    {
        return false;
    }
    std::vector<BYTE> buffer(bytes);
    Gdiplus::ImageCodecInfo* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(&buffer[0]);
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok)
    {
        return false;
    }
    for (UINT i = 0; i < count; ++i)
    {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0)
        {
            *out = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

//****************************************************************************
//	Method:		McFenceWndProc
//	FullName:	McFenceWnd::McFenceWndProc
//	author:
//	date:		2026/11/06
//	Function:   静态窗口过程：首次收到 WM_NCCREATE 时从创建参数中取出 McFenceWnd 指针并存入窗口用户数据，之后将消息转发给对应实例的 handle() 方法处理。
//
//	Returns:	LRESULT    消息处理结果，转发自实例方法或 DefWindowProcW 的返回值。
//	Parameter:	IN  HWND hwnd    目标窗口句柄
//              IN  UINT message    窗口消息 ID
//              IN  WPARAM wparam    消息附加参数
//              IN  LPARAM lparam    消息附加参数
//
//	changelog:  New
//****************************************************************************
LRESULT CALLBACK McFenceWnd::McFenceWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    McFenceWnd* self = reinterpret_cast<McFenceWnd*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = reinterpret_cast<McFenceWnd*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self)
        {
            self->m_hwnd = hwnd;
        }
    }
    if (self)
    {
        return self->handle(hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

//****************************************************************************
//	Method:		handle
//	FullName:	McFenceWnd::handle
//	author:
//	date:		2026/11/06
//	Function:   围栏窗口的主消息处理函数：处理无边框分层窗口所需的非客户区计算与命中测试（WM_NCCALCSIZE/WM_NCHITTEST）、绘制（WM_PAINT/WM_PRINTCLIENT/WM_ERASEBKGND）、DPI 变化（WM_DPICHANGED）、移动缩放的节流保存（WM_MOVE/WM_SIZE/WM_TIMER）、鼠标进入悬停与自动折叠（WM_MOUSELEAVE/WM_NCMOUSELEAVE/WM_NCMOUSEMOVE）、内部拖拽的开始/更新/结束/取消（WM_MOUSEMOVE/WM_LBUTTONDOWN/WM_LBUTTONUP/WM_CAPTURECHANGED/WM_KEYDOWN）、双击打开条目（WM_LBUTTONDBLCLK）、右键菜单（WM_CONTEXTMENU）、滚轮滚动（WM_MOUSEWHEEL）、关闭与销毁清理（WM_CLOSE/WM_DESTROY/WM_NCDESTROY），以及任务栏重建和异步图标加载完成的自定义通知。
//
//	Returns:	LRESULT    消息处理结果；未处理的消息转交给 DefWindowProcW 处理。
//	Parameter:	IN  HWND hwnd    窗口句柄
//              IN  UINT message    窗口消息 ID
//              IN  WPARAM wparam    消息附加参数
//              IN  LPARAM lparam    消息附加参数
//
//              WM_NCDESTROY 收到后，若对象已完成初始化会在此处 delete this，这是该窗口实例生命周期的终点
//
//	changelog:  New
//****************************************************************************
LRESULT McFenceWnd::handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_NCCALCSIZE:
            return 0;  // no non-client area at all

        case WM_NCACTIVATE:
            // No caption to repaint; TRUE lets the activation change proceed
            // (FALSE on deactivation would veto it).
            return TRUE;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            paint();
            return 0;
        }

        case WM_PRINTCLIENT:
        {
            // Lets PrintWindow capture the composited surface (a layered window has
            // no redirection bitmap to read back).
            paint();
            if (m_surface.MemoryDc())
            {
                RECT client;
                GetClientRect(hwnd, &client);
                BitBlt(reinterpret_cast<HDC>(wparam), 0, 0, client.right - client.left,
                       client.bottom - client.top, m_surface.MemoryDc(), 0, 0, SRCCOPY);
            }
            return 0;
        }

        case WM_NCHITTEST:
        {
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            POINT screen;
            screen.x = x;
            screen.y = y;
            POINT client = screen;
            ScreenToClient(hwnd, &client);
            RECT rect;
            GetClientRect(hwnd, &rect);
            // Hovering anywhere over a rolled-up fence unrolls it, locked or not
            // (FenceWindow_MouseEnter has no lock check either).
            if (m_is_minified && !m_restore_posted)
            {
                m_restore_posted = true;
                PostMessageW(hwnd, k_restore_message, 0, 0);
            }
            const bool right_down = (GetKeyState(VK_RBUTTON) & 0x8000) != 0;
            if (!right_down && !m_info.locked)
            {
                if (client.y < m_title_height)
                {
                    return HTCAPTION;
                }
                const int edge = scaled(8);
                const int width = rect.right - rect.left;
                const int height = rect.bottom - rect.top;
                if (client.x < edge && client.y < edge) return HTTOPLEFT;
                if (client.x > width - edge && client.y < edge) return HTTOPRIGHT;
                if (client.x < edge && client.y > height - edge) return HTBOTTOMLEFT;
                if (client.x > width - edge && client.y > height - edge) return HTBOTTOMRIGHT;
                if (client.y > height - edge) return HTBOTTOM;
                if (client.x < edge) return HTLEFT;
                if (client.x > width - edge) return HTRIGHT;
            }
            return HTCLIENT;
        }

        case k_restore_message:
            m_restore_posted = false;
            unminify();
            return 0;

        case WM_MOUSELEAVE:
            m_tracking_mouse = false;
            OnMouseLeft();
            return 0;

        case WM_NCMOUSELEAVE:
            m_tracking_nc_mouse = false;
            OnMouseLeft();
            return 0;

        case WM_NCMOUSEMOVE:
            if (!m_tracking_nc_mouse)
            {
                TRACKMOUSEEVENT track;
                ZeroMemory(&track, sizeof(track));
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE | TME_NONCLIENT;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                m_tracking_nc_mouse = true;
            }
            break;

        case WM_SETFOCUS:
            SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;

        case WM_NCLBUTTONDBLCLK:
            // 标题栏返回 HTCAPTION，所以双击到的是非客户区消息；DefWindowProc 会把它当成“最大化”。
            if (wparam == HTCAPTION)
            {
                BeginTitleRename();
                return 0;
            }
            break;

        case WM_CHAR:
            if (IsRenaming())
            {
                RenameChar(static_cast<wchar_t>(wparam));
                return 0;
            }
            break;

        case WM_IME_STARTCOMPOSITION:
            if (IsRenaming())
            {
                PlaceImeWindow();  // 候选框跟着光标走；上屏的字以 WM_CHAR 到达
            }
            break;

        case WM_KILLFOCUS:
            // 和资源管理器一样：焦点离开（点到别的程序、桌面）就保存。
            if (IsRenaming())
            {
                EndRename(true);
            }
            return 0;

        case WM_SYSCOMMAND:
        {
            const UINT command = static_cast<UINT>(wparam) & 0xFFF0;
            if (command == SC_MAXIMIZE || command == SC_MINIMIZE)
            {
                return 0;
            }
            break;
        }

        case WM_MOVE:
            if (m_initialized)
            {
                RequestSave(timer_save_position);
            }
            return 0;

        case WM_SIZE:
            if (m_initialized)
            {
                RequestSave(timer_save_size);
            }
            UpdateBlurRegion();
            Invalidate();
            return 0;

        case WM_TIMER:
            if (wparam == timer_save_position)
            {
                KillTimer(hwnd, timer_save_position);
                SavePosition();
            }
            else if (wparam == timer_save_size)
            {
                KillTimer(hwnd, timer_save_size);
                SaveSize();
            }
            else if (wparam == timer_folder_reload)
            {
                KillTimer(hwnd, timer_folder_reload);
                // 改名途中刷新会让正在编辑的项目跳位置；等编辑结束再说。
                if (IsRenaming())
                {
                    SetTimer(hwnd, timer_folder_reload, 500, NULL);
                }
                else
                {
                    ReloadFolder();
                }
            }
            else if (wparam == timer_rename_caret)
            {
                m_rename_caret_on = !m_rename_caret_on;
                Invalidate();
            }
            return 0;

        case WM_DPICHANGED:
        {
            ComputeScale();
            ReloadFonts();
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            RECT bounds;
            GetWindowRect(hwnd, &bounds);
            if (suggested)
            {
                SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            else
            {
                SetWindowPos(hwnd, NULL, 0, 0, scaled(m_info.width), scaled(m_info.height),
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            Invalidate();
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            if (!m_tracking_mouse)
            {
                TRACKMOUSEEVENT track;
                ZeroMemory(&track, sizeof(track));
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                m_tracking_mouse = true;
            }
            POINT client;
            client.x = GET_X_LPARAM(lparam);
            client.y = GET_Y_LPARAM(lparam);

            if (m_drag_in_progress)
            {
                POINT screen = client;
                ClientToScreen(hwnd, &screen);
                UpdateInternalDrag(screen);
                return 0;
            }

            if (m_marquee)
            {
                // 框到边缘时顺便滚动内容。
                RECT rect;
                GetClientRect(hwnd, &rect);
                const int step = scaled(8);
                if (client.y > rect.bottom - step && m_scroll_offset < m_scroll_height)
                {
                    m_scroll_offset = min(m_scroll_height, m_scroll_offset + step);
                }
                else if (client.y < m_title_height + step && m_scroll_offset > 0)
                {
                    m_scroll_offset = max(0, m_scroll_offset - step);
                }
                UpdateMarquee(client);
                return 0;
            }

            if ((wparam & MK_LBUTTON) != 0 && !m_drag_candidate.empty())
            {
                const int drag_x = GetSystemMetrics(SM_CXDRAG);
                const int drag_y = GetSystemMetrics(SM_CYDRAG);
                if (abs(client.x - m_drag_start.x) > drag_x / 2 ||
                    abs(client.y - m_drag_start.y) > drag_y / 2)
                {
                    StartInternalDrag();
                    if (m_drag_in_progress)
                    {
                        POINT screen = client;
                        ClientToScreen(hwnd, &screen);
                        UpdateInternalDrag(screen);
                        return 0;
                    }
                }
            }

            const ULONGLONG now = McTicks();
            if (now - m_last_mouse_invalidate >= 32)
            {
                m_last_mouse_invalidate = now;
                Invalidate();
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            POINT client;
            client.x = GET_X_LPARAM(lparam);
            client.y = GET_Y_LPARAM(lparam);
            if (IsRenaming())
            {
                if (m_rename_box.Contains(static_cast<float>(client.x), static_cast<float>(client.y)))
                {
                    RenameCaretFromPoint(client);
                    return 0;
                }
                // openFrence / 资源管理器：点到编辑框以外先保存，再按普通点击处理。
                EndRename(true);
            }
            SetFocus(hwnd);  // 键盘（Del、F2、Ctrl+A、Esc）要能到这里
            const bool ctrl = (wparam & MK_CONTROL) != 0;
            const std::wstring hit = HitTestItem(client);
            m_drag_candidate.clear();
            m_collapse_on_up = false;
            if (!hit.empty())
            {
                if (ctrl)
                {
                    ToggleSelected(hit);
                }
                else if (!IsSelected(hit))
                {
                    SelectOnly(hit);
                }
                else
                {
                    // 在已选中的多项之一上按下：可能要拖整个选择，松开时才收缩成这一项。
                    m_collapse_on_up = m_selection.size() > 1;
                }
                // 锁定的围栏不能拖动项目（也不能拖出）；未锁定时映射围栏同样可以拖出。
                if (!m_info.locked && IsSelected(hit))
                {
                    m_drag_candidate = hit;
                    m_drag_start = client;
                }
            }
            else if (client.y >= m_title_height)
            {
                // 空白处：开始框选（Ctrl 时在原选择上切换）。
                m_marquee = true;
                m_marquee_toggle = ctrl;
                m_marquee_base = ctrl ? m_selection : std::vector<std::wstring>();
                if (!ctrl)
                {
                    m_selection.clear();
                }
                m_marquee_start.x = client.x;
                m_marquee_start.y = client.y + m_scroll_offset;
                m_marquee_end = m_marquee_start;
                SetCapture(hwnd);
            }
            Invalidate();
            return 0;
        }

        case WM_LBUTTONUP:
        {
            POINT client;
            client.x = GET_X_LPARAM(lparam);
            client.y = GET_Y_LPARAM(lparam);
            if (m_drag_in_progress)
            {
                POINT screen = client;
                ClientToScreen(hwnd, &screen);
                FinishInternalDrag(screen, false);
            }
            else if (m_marquee)
            {
                m_marquee = false;
                m_marquee_base.clear();
                if (GetCapture() == hwnd)
                {
                    ReleaseCapture();
                }
            }
            else if (m_collapse_on_up && !m_drag_candidate.empty())
            {
                SelectOnly(m_drag_candidate);
            }
            m_collapse_on_up = false;
            m_drag_candidate.clear();
            Invalidate();
            return 0;
        }

        case WM_CAPTURECHANGED:
            // Capture taken away mid-drag (Alt+Tab, a popup...): cancel cleanly.
            if (reinterpret_cast<HWND>(lparam) != hwnd)
            {
                if (m_drag_in_progress)
                {
                    POINT screen;
                    GetCursorPos(&screen);
                    FinishInternalDrag(screen, true);
                }
                if (m_marquee)
                {
                    m_marquee = false;
                    m_marquee_base.clear();
                }
                Invalidate();
            }
            return 0;

        case WM_KEYDOWN:
        {
            if (IsRenaming())
            {
                RenameKeyDown(wparam);
                return 0;
            }
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (wparam == VK_ESCAPE)
            {
                if (m_drag_in_progress)
                {
                    POINT screen;
                    GetCursorPos(&screen);
                    FinishInternalDrag(screen, true);
                }
                else if (m_marquee)
                {
                    m_marquee = false;
                    m_selection = m_marquee_base;
                    m_marquee_base.clear();
                    if (GetCapture() == hwnd)
                    {
                        ReleaseCapture();
                    }
                }
                else
                {
                    m_selection.clear();
                }
                Invalidate();
                return 0;
            }
            if (m_drag_in_progress || m_marquee)
            {
                return 0;
            }
            if (ctrl && wparam == 'A')
            {
                SelectAll();
                Invalidate();
                return 0;
            }
            const std::vector<std::wstring> selected = SelectedInOrder();
            if (wparam == VK_DELETE && !selected.empty())
            {
                if (IsMapped())
                {
                    RecycleItems(selected);
                }
                else if (!m_info.locked)
                {
                    RemoveRecords(selected);
                }
                return 0;
            }
            if (wparam == VK_F2 && selected.size() == 1)
            {
                BeginItemRename(selected[0]);
                return 0;
            }
            if (wparam == VK_RETURN)
            {
                for (size_t i = 0; i < selected.size(); ++i)
                {
                    OpenFenceItem(selected[i]);
                }
                return 0;
            }
            if (wparam == VK_F5 && IsMapped())
            {
                ReloadFolder();
                return 0;
            }
            break;
        }

        case WM_LBUTTONDBLCLK:
        {
            POINT client;
            client.x = GET_X_LPARAM(lparam);
            client.y = GET_Y_LPARAM(lparam);
            if (IsRenaming() &&
                m_rename_box.Contains(static_cast<float>(client.x), static_cast<float>(client.y)))
            {
                m_rename_pristine = true;  // 双击编辑框：重新全选
                m_rename_caret = m_rename_text.size();
                Invalidate();
                return 0;
            }
            if (client.y < m_title_height)
            {
                BeginTitleRename();  // 锁定的围栏标题栏是 HTCLIENT，双击走这里
                return 0;
            }
            const std::wstring item = HitTestItem(client);
            if (!item.empty())
            {
                OpenFenceItem(item);
            }
            return 0;
        }

        case WM_CONTEXTMENU:
        {
            EndRename(true);
            POINT screen;
            if (static_cast<INT>(lparam) == -1)
            {
                GetCursorPos(&screen);
            }
            else
            {
                screen.x = GET_X_LPARAM(lparam);
                screen.y = GET_Y_LPARAM(lparam);
            }
            ShowFenceMenu(screen);
            return 0;
        }

        case WM_INITMENUPOPUP:
        case WM_DRAWITEM:
        case WM_MEASUREITEM:
        case WM_MENUCHAR:
        {
            // 「新建」子菜单由 Shell 处理程序延迟填充、自绘图标。
            LRESULT result = 0;
            if (m_new_menu.HandleMenuMessage(message, wparam, lparam, &result))
            {
                return result;
            }
            break;
        }

        case WM_ENTERSIZEMOVE:
            EndRename(true);
            BeginMoveSize();
            return 0;

        case WM_EXITSIZEMOVE:
            if (m_group_move)
            {
                // 整体移动结束：其他围栏也要把新位置写盘。
                for (size_t i = 0; i < s_registry.size(); ++i)
                {
                    if (s_registry[i] != this)
                    {
                        s_registry[i]->SavePosition();
                    }
                }
            }
            m_group_move = false;
            m_group_origins.clear();
            return 0;

        case WM_MOVING:
            SnapMoving(reinterpret_cast<RECT*>(lparam));
            return TRUE;

        case WM_SIZING:
            SnapSizing(reinterpret_cast<RECT*>(lparam), wparam);
            return TRUE;

        case WM_DISPLAYCHANGE:
            EnsureOnScreen();
            break;

        case MCFENCE_WM_FOLDER_CHANGED:
            // 防抖：复制一批文件会连续触发很多次通知。
            SetTimer(hwnd, timer_folder_reload, MCFENCE_FOLDER_DEBOUNCE_MS, NULL);
            return 0;

        case WM_MOUSEWHEEL:
        {
            if (m_rename_target == MCFENCE_RENAME_ITEM)
            {
                EndRename(true);
            }
            if (m_scroll_height > 0)
            {
                const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
                const int step = scaled(14);
                int offset = m_scroll_offset - (delta > 0 ? step : (delta < 0 ? -step : 0));
                if (offset < 0)
                {
                    offset = 0;
                }
                if (offset > m_scroll_height)
                {
                    offset = m_scroll_height;
                }
                if (offset != m_scroll_offset)
                {
                    m_scroll_offset = offset;
                    Invalidate();
                }
            }
            return 0;
        }

        case WM_CLOSE:
            if (!m_allow_close)
            {
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            EndRename(true, false);
            KillTimer(hwnd, timer_rename_caret);
            KillTimer(hwnd, timer_save_position);
            KillTimer(hwnd, timer_save_size);
            KillTimer(hwnd, timer_folder_reload);
            m_watch.Stop();
            // Flush a throttled save still waiting on its timer, so the last four
            // seconds of moving / resizing survive an exit. A removed fence must
            // not be written back (that would recreate its directory).
            if (m_initialized && !m_removed)
            {
                if (m_pending_position)
                {
                    SavePosition();
                }
                if (m_pending_size)
                {
                    SaveSize();
                }
            }
            if (m_drop_target)
            {
                RevokeDragDrop(hwnd);
                m_drop_target->owner = NULL;  // OLE may still hold a reference
                m_drop_target->Release();
                m_drop_target = NULL;
            }
            for (size_t i = 0; i < s_registry.size(); ++i)
            {
                if (s_registry[i] == this)
                {
                    s_registry.erase(s_registry.begin() + i);
                    break;
                }
            }
            if (!m_allow_close && m_initialized && !m_removed && s_app_window)
            {
                McLogFormat(L"围栏 '%s' 被意外销毁（资源管理器重启？），重建", m_info.name.c_str());
                PostMessageW(s_app_window, MCFENCE_WM_RECREATE, 0,
                             reinterpret_cast<LPARAM>(new McFenceInfo(m_info)));
                return 0;
            }
            // 有托盘图标时最后一个围栏被移除也不退出：还可以从托盘新建。
            if (s_registry.empty() && !s_app_window)
            {
                PostQuitMessage(0);
            }
            return 0;

        case WM_NCDESTROY:
            // Last message the window receives: the view owns nothing else now.
            // Before CreateFenceWindow finished, Create() still owns the view and
            // deletes it on failure, so only free it once it is fully initialised.
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            m_hwnd = NULL;
            if (m_initialized)
            {
                delete this;
            }
            return 0;

        default:
            break;
    }

    // TaskbarCreated is a registered message, so it has to be compared by id.
    static const UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    if (message == taskbar_created)
    {
        // 新的资源管理器有新的 SHELLDLL_DefView：重新挂接所有者。
        ApplyDesktopOwnerTo();
        EnsureOnScreen();
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Invalidate();
        return 0;
    }
    if (message == k_icon_ready_message)
    {
        Invalidate();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

