#include "McShellMenu.h"
#include "McLog.h"
#include "McUtil.h"


namespace
{

const wchar_t* const k_host_class = MCFENCE_MENU_HOST_CLASS;

const UINT MCFENCE_MENU_CMD_FIRST = 1;
const UINT MCFENCE_MENU_CMD_LAST = 30000;

//****************************************************************************
//	Method:		invoke_command
//	FullName:	invoke_command
//	author:
//	date:		2026/11/06
//	Function:   构造 CMINVOKECOMMANDINFOEX 并调用 IContextMenu::InvokeCommand 
//              执行用户在 Shell 右键菜单中选中的命令，
//              附带 Ctrl/Shift 修饰键状态及调用坐标/目录。
//
//	Returns:	void
//	Parameter:	IN  IContextMenu* menu    承载该命令的上下文菜单接口
//              IN  UINT command    TrackPopupMenuEx 返回的命令 ID，
//                                  会减去 MCFENCE_MENU_CMD_FIRST 还原为 verb 偏移
//              IN  const std::wstring& folder    命令执行时使用的工作目录
//              IN  POINT point    屏幕坐标，作为 ptInvoke 传给命令
//
//	changelog:  New
//****************************************************************************
void invoke_command(IContextMenu* menu, UINT command, const std::wstring& folder, POINT point)
{
    if (!menu || command == 0)
    {
        return;
    }
    CMINVOKECOMMANDINFOEX info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.hwnd = GetActiveWindow();
    info.lpVerb = reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(command - MCFENCE_MENU_CMD_FIRST));
    info.lpVerbW = reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(command - MCFENCE_MENU_CMD_FIRST));
    info.nShow = SW_SHOWNORMAL;
    if (!folder.empty())
    {
        info.lpDirectory = "";
        info.lpDirectoryW = folder.c_str();
    }
    // CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_NOASYNC, plus the
    // modifier state the shell handler reads (Control/Shift).
    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_NOASYNC;
    if (GetKeyState(VK_CONTROL) < 0)
    {
        info.fMask |= CMIC_MASK_CONTROL_DOWN;
    }
    if (GetKeyState(VK_SHIFT) < 0)
    {
        info.fMask |= CMIC_MASK_SHIFT_DOWN;
    }
    info.ptInvoke.x = point.x;
    info.ptInvoke.y = point.y;
    menu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
}

}  // namespace

McShellMenu::McShellMenu()
    : m_host(NULL), m_context_menu(NULL), m_context_menu2(NULL), m_context_menu3(NULL)
{
    EnsureWindowClass();
    m_host = CreateWindowExW(0, k_host_class, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL,
                            GetModuleHandleW(NULL), this);
}

//****************************************************************************
//	Method:		~McShellMenu
//	FullName:	McShellMenu::~McShellMenu
//	author:
//	date:		2026/11/06
//	Function:   析构函数：释放已缓存的 Shell 上下文菜单接口并销毁宿主窗口。
//
//	Returns:	无
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
McShellMenu::~McShellMenu()
{
    Release();
    if (m_host)
    {
        DestroyWindow(m_host);
        m_host = NULL;
    }
}

//****************************************************************************
//	Method:		EnsureWindowClass
//	FullName:	McShellMenu::EnsureWindowClass
//	author:
//	date:		2026/11/06
//	Function:   延迟注册菜单宿主窗口类，用静态 ATOM 缓存结果，保证只注册一次。
//
//	Returns:	ATOM    窗口类原子
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
ATOM McShellMenu::EnsureWindowClass()
{
    static ATOM atom = 0;
    if (atom)
    {
        return atom;
    }
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &McShellMenu::HostProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = k_host_class;
    atom = RegisterClassExW(&wc);
    return atom;
}

//****************************************************************************
//	Method:		HostProc
//	FullName:	McShellMenu::HostProc
//	author:
//	date:		2026/11/06
//	Function:   菜单宿主窗口过程：把 WM_INITMENUPOPUP/WM_DRAWITEM/WM_MEASUREITEM/WM_MENUSELECT 
//              转交给 IContextMenu2::HandleMenuMsg，WM_MENUCHAR 转交给 IContextMenu3::HandleMenuMsg2，
//              使 Shell 扩展菜单项（图标、拥有者绘制项、助记符）能正常工作；其余消息走默认处理。
//
//	Returns:	LRESULT              消息处理结果
//	Parameter:	IN  HWND hwnd        宿主窗口句柄
//              IN  UINT message     消息编号
//              IN  WPARAM wparam    消息参数
//              IN  LPARAM lparam    消息参数
//
//	changelog:  New
//****************************************************************************
LRESULT CALLBACK McShellMenu::HostProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    McShellMenu* self = reinterpret_cast<McShellMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self)
    {
        switch (message)
        {
            case WM_INITMENUPOPUP:
                if (self->m_context_menu2)
                {
                    self->m_context_menu2->HandleMenuMsg(message, wparam, lparam);
                    return 0;
                }
                break;
            case WM_DRAWITEM:
            case WM_MEASUREITEM:
                if (self->m_context_menu2)
                {
                    self->m_context_menu2->HandleMenuMsg(message, wparam, lparam);
                    return TRUE;
                }
                break;
            case WM_MENUCHAR:
                if (self->m_context_menu3)
                {
                    LRESULT result = 0;
                    if (SUCCEEDED(self->m_context_menu3->HandleMenuMsg2(message, wparam, lparam,
                                                                      &result)))
                    {
                        return result;
                    }
                }
                break;
            case WM_MENUSELECT:
                if (self->m_context_menu2)
                {
                    self->m_context_menu2->HandleMenuMsg(message, wparam, lparam);
                }
                break;
            default:
                break;
        }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

//****************************************************************************
//	Method:		Release
//	FullName:	McShellMenu::Release
//	author:
//	date:		2026/11/06
//	Function:   释放已缓存的 IContextMenu/IContextMenu2/IContextMenu3 接口指针并置空。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McShellMenu::Release()
{
    if (m_context_menu3)
    {
        m_context_menu3->Release();
        m_context_menu3 = NULL;
    }
    if (m_context_menu2)
    {
        m_context_menu2->Release();
        m_context_menu2 = NULL;
    }
    if (m_context_menu)
    {
        m_context_menu->Release();
        m_context_menu = NULL;
    }
}

//****************************************************************************
//	Method:		Show
//	FullName:	McShellMenu::Show
//	author:
//	date:		2026/11/06
//	Function:   为指定路径弹出 Shell 右键菜单：解析 PIDL 并从桌面文件夹取得 IContextMenu，
//              调用 QueryContextMenu 填充弹出菜单（按下 Shift 时附加扩展命令），
//              前置宿主窗口并用 TrackPopupMenuEx 显示菜单，取得用户选择的命令后调用 invoke_command 执行，
//              最后释放接口。
//
//	Returns:	bool    菜单成功显示（无论用户是否选中命令）返回 true；
//                      路径不存在、获取桌面文件夹/解析路径/取得 IContextMenu 或创建菜单失败时返回 false
// 
//	Parameter:	IN  const std::wstring& path    目标文件或文件夹路径
//              IN  POINT screen_point          菜单弹出的屏幕坐标
//
//	changelog:  New
//****************************************************************************
bool McShellMenu::Show(const std::wstring& path, POINT screen_point)
{
    Release();
    if (!m_host || !McPathExists(path))
    {
        return false;
    }

    m_folder = McParentDirectory(path);
    IShellFolder* desktop = NULL;
    if (FAILED(SHGetDesktopFolder(&desktop)) || !desktop)
    {
        McLog(L"SHGetDesktopFolder failed");
        return false;
    }

    LPITEMIDLIST pidl = NULL;
    HRESULT hr = SHParseDisplayName(path.c_str(), NULL, &pidl, 0, NULL);
    if (FAILED(hr) || !pidl)
    {
        desktop->Release();
        McLogHr(L"SHParseDisplayName", hr);
        return false;
    }

    const void* children[1] = {pidl};
    hr = desktop->GetUIObjectOf(m_host, 1, reinterpret_cast<PCUITEMID_CHILD_ARRAY>(children),
                                IID_IContextMenu, NULL, reinterpret_cast<void**>(&m_context_menu));
    CoTaskMemFree(pidl);
    desktop->Release();
    if (FAILED(hr) || !m_context_menu)
    {
        McLogHr(L"GetUIObjectOf(IContextMenu)", hr);
        return false;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        Release();
        return false;
    }
    UINT flags = CMF_EXPLORE;
    if (GetKeyState(VK_SHIFT) < 0)
    {
        flags |= CMF_EXTENDEDVERBS;  // the "extended" verbs Explorer shows with Shift
    }
    m_context_menu->QueryContextMenu(menu, 0, MCFENCE_MENU_CMD_FIRST, MCFENCE_MENU_CMD_LAST, flags);
    m_context_menu->QueryInterface(IID_IContextMenu2, reinterpret_cast<void**>(&m_context_menu2));
    m_context_menu->QueryInterface(IID_IContextMenu3, reinterpret_cast<void**>(&m_context_menu3));

    SetWindowLongPtrW(m_host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // TrackPopupMenu documentation: the owner must be foreground, or a click
    // outside the menu does not dismiss it; WM_NULL afterwards finishes the switch.
    SetForegroundWindow(m_host);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                                    screen_point.x, screen_point.y, m_host, NULL);
    PostMessageW(m_host, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (command != 0)
    {
        invoke_command(m_context_menu, command, m_folder, screen_point);
    }
    Release();
    return true;
}

