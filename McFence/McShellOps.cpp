#include "McShellOps.h"
#include "McLog.h"
#include "McSettings.h"
#include "McUtil.h"

#include <ole2.h>


namespace
{

// {D969A300-E7FF-11d0-A93B-00A0C90F2719}：HKCR\Directory\Background\shellex\
// ContextMenuHandlers\New 注册的处理程序，Windows 2000 起 CLSID 未变。
const CLSID k_clsid_new_menu = {0xD969A300, 0xE7FF, 0x11D0, {0xA9, 0x3B, 0x00, 0xA0, 0xC9, 0x0F, 0x27, 0x19}};

//****************************************************************************
//	Method:		DoubleNullList
//	FullName:	DoubleNullList
//	author:
//	date:		2026/11/07
//	Function:   把路径列表拼成 SHFileOperation / DROPFILES 需要的“\0 分隔、\0\0 结尾”缓冲。
//
//	Returns:	std::vector<wchar_t>    拼好的缓冲
//	Parameter:	IN  const std::vector<std::wstring>& paths    路径列表
//
//	changelog:  New
//****************************************************************************
std::vector<wchar_t> DoubleNullList(const std::vector<std::wstring>& paths)
{
    std::vector<wchar_t> buffer;
    for (size_t i = 0; i < paths.size(); ++i)
    {
        buffer.insert(buffer.end(), paths[i].begin(), paths[i].end());
        buffer.push_back(L'\0');
    }
    buffer.push_back(L'\0');
    if (buffer.size() < 2)
    {
        buffer.push_back(L'\0');
    }
    return buffer;
}

HGLOBAL DuplicateGlobal(HGLOBAL source)
{
    const SIZE_T size = GlobalSize(source);
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy)
    {
        return NULL;
    }
    void* from = GlobalLock(source);
    void* to = GlobalLock(copy);
    if (from && to)
    {
        memcpy(to, from, size);
    }
    if (to) GlobalUnlock(copy);
    if (from) GlobalUnlock(source);
    return copy;
}

HGLOBAL MakeDropFiles(const std::vector<std::wstring>& paths)
{
    const std::vector<wchar_t> list = DoubleNullList(paths);
    const SIZE_T bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!global)
    {
        return NULL;
    }
    DROPFILES* drop = static_cast<DROPFILES*>(GlobalLock(global));
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    memcpy(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES), &list[0], list.size() * sizeof(wchar_t));
    GlobalUnlock(global);
    return global;
}

HGLOBAL MakeDword(DWORD value)
{
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (global)
    {
        *static_cast<DWORD*>(GlobalLock(global)) = value;
        GlobalUnlock(global);
    }
    return global;
}

// 只提供 CF_HDROP 的数据对象；目标通过 SetData 塞进来的格式（Preferred DropEffect、
// Performed DropEffect、DropDescription……）原样存着，再被 GetData 读回去。
class McFileDataObject : public IDataObject
{
public:
    McFileDataObject(const std::vector<std::wstring>& paths) : m_references(1), m_paths(paths) {}

    ~McFileDataObject()
    {
        for (size_t i = 0; i < m_stored.size(); ++i)
        {
            ReleaseStgMedium(&m_stored[i].medium);
        }
    }

    void Store(CLIPFORMAT format, HGLOBAL global)
    {
        if (!global)
        {
            return;
        }
        FORMATETC etc = {format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium;
        ZeroMemory(&medium, sizeof(medium));
        medium.tymed = TYMED_HGLOBAL;
        medium.hGlobal = global;
        SetData(&etc, &medium, TRUE);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** out)
    {
        if (!out)
        {
            return E_POINTER;
        }
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDataObject))
        {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_references));
    }
    STDMETHODIMP_(ULONG) Release()
    {
        const LONG left = InterlockedDecrement(&m_references);
        if (left == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(left);
    }

    // IDataObject
    STDMETHODIMP GetData(FORMATETC* format, STGMEDIUM* medium)
    {
        if (!format || !medium)
        {
            return E_INVALIDARG;
        }
        ZeroMemory(medium, sizeof(*medium));
        if (format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL))
        {
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = MakeDropFiles(m_paths);
            return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
        }
        const int index = Find(format);
        if (index < 0)
        {
            return DV_E_FORMATETC;
        }
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = DuplicateGlobal(m_stored[index].medium.hGlobal);
        return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*)
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP QueryGetData(FORMATETC* format)
    {
        if (!format)
        {
            return E_INVALIDARG;
        }
        if (format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL))
        {
            return S_OK;
        }
        return Find(format) >= 0 ? S_OK : DV_E_FORMATETC;
    }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out)
    {
        if (out)
        {
            out->ptd = NULL;
        }
        return E_NOTIMPL;
    }
    STDMETHODIMP SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release)
    {
        if (!format || !medium || medium->tymed != TYMED_HGLOBAL || format->cfFormat == CF_HDROP)
        {
            return E_NOTIMPL;
        }
        McStored entry;
        entry.format = *format;
        entry.format.ptd = NULL;
        entry.format.tymed = TYMED_HGLOBAL;
        ZeroMemory(&entry.medium, sizeof(entry.medium));
        entry.medium.tymed = TYMED_HGLOBAL;
        entry.medium.hGlobal = release ? medium->hGlobal : DuplicateGlobal(medium->hGlobal);
        if (!entry.medium.hGlobal)
        {
            return E_OUTOFMEMORY;
        }
        if (release && medium->pUnkForRelease)
        {
            // 我们拷走的是句柄本身；释放者不再需要。
            medium->pUnkForRelease->Release();
        }
        const int index = Find(&entry.format);
        if (index >= 0)
        {
            ReleaseStgMedium(&m_stored[index].medium);
            m_stored[index] = entry;
        }
        else
        {
            m_stored.push_back(entry);
        }
        return S_OK;
    }
    STDMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** out)
    {
        if (!out)
        {
            return E_POINTER;
        }
        if (direction != DATADIR_GET)
        {
            *out = NULL;
            return E_NOTIMPL;
        }
        std::vector<FORMATETC> formats;
        FORMATETC hdrop = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        formats.push_back(hdrop);
        for (size_t i = 0; i < m_stored.size(); ++i)
        {
            formats.push_back(m_stored[i].format);
        }
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(formats.size()), &formats[0], out);
    }
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*)
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    STDMETHODIMP DUnadvise(DWORD)
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**)
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    struct McStored
    {
        FORMATETC format;
        STGMEDIUM medium;
    };

    int Find(const FORMATETC* format) const
    {
        for (size_t i = 0; i < m_stored.size(); ++i)
        {
            if (m_stored[i].format.cfFormat == format->cfFormat &&
                m_stored[i].format.dwAspect == format->dwAspect &&
                m_stored[i].format.lindex == format->lindex && (format->tymed & TYMED_HGLOBAL))
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    LONG m_references;
    std::vector<std::wstring> m_paths;
    std::vector<McStored> m_stored;
};

class McDropSource : public IDropSource
{
public:
    McDropSource() : m_references(1) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** out)
    {
        if (!out)
        {
            return E_POINTER;
        }
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDropSource))
        {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_references));
    }
    STDMETHODIMP_(ULONG) Release()
    {
        const LONG left = InterlockedDecrement(&m_references);
        if (left == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(left);
    }
    STDMETHODIMP QueryContinueDrag(BOOL escape, DWORD keys)
    {
        if (escape)
        {
            return DRAGDROP_S_CANCEL;
        }
        if ((keys & MK_LBUTTON) == 0)
        {
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD)
    {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    LONG m_references;
};

void SnapshotNames(const std::wstring& folder, std::vector<std::wstring>& out)
{
    out.clear();
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(McJoinPath(folder, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return;
    }
    do
    {
        out.push_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

//****************************************************************************
//	Method:		EnglishTypeName
//	FullName:	EnglishTypeName
//	author:
//	date:		2026/11/07
//	Function:   取扩展名对应文件类型的英文名称：HKCR\<扩展名> 的默认值是 ProgID，ProgID 的默认值
//              通常就是英文类型名（"Text Document"、"Microsoft Word Document"）。
//
//	Returns:	std::wstring    英文名称；取不到或不是纯 ASCII（有些软件直接写本地化名称）时返回空串
//	Parameter:	IN  const std::wstring& extension    带点的扩展名，例如 ".txt"
//
//	changelog:  New
//****************************************************************************
std::wstring EnglishTypeName(const std::wstring& extension)
{
    wchar_t prog_id[256] = L"";
    LONG bytes = sizeof(prog_id);
    if (extension.empty() || RegQueryValueW(HKEY_CLASSES_ROOT, extension.c_str(), prog_id, &bytes) != ERROR_SUCCESS ||
        prog_id[0] == L'\0')
    {
        return std::wstring();
    }
    wchar_t name[256] = L"";
    bytes = sizeof(name);
    if (RegQueryValueW(HKEY_CLASSES_ROOT, prog_id, name, &bytes) != ERROR_SUCCESS || name[0] == L'\0')
    {
        return std::wstring();
    }
    for (const wchar_t* p = name; *p; ++p)
    {
        if (*p > 0x7E)
        {
            return std::wstring();
        }
    }
    return name;
}

}  // namespace

//****************************************************************************
//	Method:		McDoDragOut
//	FullName:	McDoDragOut
//	author:
//	date:		2026/11/07
//	Function:   用自建的 CF_HDROP 数据对象发起系统 OLE 拖拽，让文件可以拖到资源管理器、桌面或其他程序。
//
//	Returns:	DWORD    DoDragDrop 报告的效果（取消时为 DROPEFFECT_NONE）
//	Parameter:	IN  const std::vector<std::wstring>& paths    要拖出的文件 / 文件夹
//              IN  DWORD allowed    允许的效果组合
//
//              Preferred DropEffect 设为移动：资源管理器跨分区时也默认移动，按住 Ctrl 才复制。
//              同一分区上的“优化移动”由目标自己完成，返回值可能是 NONE，调用方应检查文件是否还在
//
//	changelog:  2026/11/07 拖出默认移动
//****************************************************************************
DWORD McDoDragOut(const std::vector<std::wstring>& paths, DWORD allowed)
{
    if (paths.empty())
    {
        return DROPEFFECT_NONE;
    }
    McFileDataObject* data = new McFileDataObject(paths);
    if (allowed & DROPEFFECT_MOVE)
    {
        static const CLIPFORMAT preferred =
            static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
        data->Store(preferred, MakeDword(DROPEFFECT_MOVE));
    }
    McDropSource* source = new McDropSource();
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = DoDragDrop(data, source, allowed, &effect);
    if (FAILED(hr))
    {
        McLogHr(L"DoDragDrop", hr);
    }
    source->Release();
    data->Release();
    return hr == DRAGDROP_S_DROP ? effect : DROPEFFECT_NONE;
}

//****************************************************************************
//	Method:		McShellCopyMove
//	FullName:	McShellCopyMove
//	author:
//	date:		2026/11/07
//	Function:   用 SHFileOperationW 把文件复制或移动到目标文件夹；重名、覆盖由系统对话框处理，可撤销。
//
//	Returns:	bool    操作完成且未被用户取消返回 true
//	Parameter:	IN  HWND owner    进度 / 冲突对话框的所有者
//              IN  const std::vector<std::wstring>& paths    源路径
//              IN  const std::wstring& folder    目标文件夹
//              IN  bool move    true 移动，false 复制
//
//	changelog:  New
//****************************************************************************
bool McShellCopyMove(HWND owner, const std::vector<std::wstring>& paths, const std::wstring& folder,
                     bool move)
{
    if (paths.empty() || folder.empty())
    {
        return false;
    }
    std::vector<wchar_t> from = DoubleNullList(paths);
    std::vector<std::wstring> to_list(1, folder);
    std::vector<wchar_t> to = DoubleNullList(to_list);
    SHFILEOPSTRUCTW op;
    ZeroMemory(&op, sizeof(op));
    op.hwnd = owner;
    op.wFunc = move ? FO_MOVE : FO_COPY;
    op.pFrom = &from[0];
    op.pTo = &to[0];
    // 拖到同一个文件夹时自动起“副本”名，和资源管理器一致。
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION;
    const int result = SHFileOperationW(&op);
    if (result != 0)
    {
        McLogFormat(L"SHFileOperation(%s) 返回 0x%X", move ? L"move" : L"copy", result);
    }
    return result == 0 && !op.fAnyOperationsAborted;
}

//****************************************************************************
//	Method:		McShellRecycle
//	FullName:	McShellRecycle
//	author:
//	date:		2026/11/07
//	Function:   把文件删除到回收站（系统负责确认提示）。
//
//	Returns:	bool    操作完成且未被用户取消返回 true
//	Parameter:	IN  HWND owner    对话框所有者
//              IN  const std::vector<std::wstring>& paths    要删除的路径
//
//	changelog:  New
//****************************************************************************
bool McShellRecycle(HWND owner, const std::vector<std::wstring>& paths)
{
    if (paths.empty())
    {
        return false;
    }
    std::vector<wchar_t> from = DoubleNullList(paths);
    SHFILEOPSTRUCTW op;
    ZeroMemory(&op, sizeof(op));
    op.hwnd = owner;
    op.wFunc = FO_DELETE;
    op.pFrom = &from[0];
    op.fFlags = FOF_ALLOWUNDO;
    const int result = SHFileOperationW(&op);
    return result == 0 && !op.fAnyOperationsAborted;
}

McNewMenu::McNewMenu()
    : m_menu(NULL), m_menu2(NULL), m_menu3(NULL), m_first(0), m_last(0), m_owner_menu(NULL)
{
}

McNewMenu::~McNewMenu()
{
    Release();
}

void McNewMenu::Release()
{
    if (m_menu3)
    {
        m_menu3->Release();
        m_menu3 = NULL;
    }
    if (m_menu2)
    {
        m_menu2->Release();
        m_menu2 = NULL;
    }
    if (m_menu)
    {
        m_menu->Release();
        m_menu = NULL;
    }
    m_first = 0;
    m_last = 0;
    m_owner_menu = NULL;
}

//****************************************************************************
//	Method:		Attach
//	FullName:	McNewMenu::Attach
//	author:
//	date:		2026/11/07
//	Function:   创建系统 NewMenu 处理程序，用目标文件夹初始化，并把「新建」子菜单插入到给定菜单中。
//
//	Returns:	bool    插入成功返回 true
//	Parameter:	IN  HMENU menu    要插入的菜单
//              IN  UINT position    插入位置
//              IN  const std::wstring& folder    新项目所在的文件夹
//              IN  UINT first_id    处理程序可用的第一个命令 id
//              IN  UINT last_id    处理程序可用的最后一个命令 id
//
//	changelog:  New
//****************************************************************************
bool McNewMenu::Attach(HMENU menu, UINT position, const std::wstring& folder, UINT first_id,
                       UINT last_id)
{
    Release();
    if (!menu || folder.empty())
    {
        return false;
    }
    HRESULT hr = CoCreateInstance(k_clsid_new_menu, NULL, CLSCTX_INPROC_SERVER, IID_IContextMenu,
                                  reinterpret_cast<void**>(&m_menu));
    if (FAILED(hr) || !m_menu)
    {
        McLogHr(L"CoCreateInstance(NewMenu)", hr);
        m_menu = NULL;
        return false;
    }
    LPITEMIDLIST pidl = NULL;
    hr = SHParseDisplayName(folder.c_str(), NULL, &pidl, 0, NULL);
    IShellExtInit* init = NULL;
    if (SUCCEEDED(hr))
    {
        hr = m_menu->QueryInterface(IID_IShellExtInit, reinterpret_cast<void**>(&init));
    }
    if (SUCCEEDED(hr))
    {
        hr = init->Initialize(pidl, NULL, NULL);
    }
    if (init)
    {
        init->Release();
    }
    if (pidl)
    {
        CoTaskMemFree(pidl);
    }
    if (SUCCEEDED(hr))
    {
        hr = m_menu->QueryContextMenu(menu, position, first_id, last_id, CMF_NORMAL);
    }
    if (FAILED(hr))
    {
        McLogHr(L"NewMenu", hr);
        Release();
        return false;
    }
    m_menu->QueryInterface(IID_IContextMenu2, reinterpret_cast<void**>(&m_menu2));
    m_menu->QueryInterface(IID_IContextMenu3, reinterpret_cast<void**>(&m_menu3));
    m_folder = folder;
    m_first = first_id;
    m_last = last_id;
    m_owner_menu = menu;
    if (McSettingsGet().language == MCFENCE_LANG_EN)
    {
        // 「新建」本身插在 |position|，它的命令 id 是 first_id。
        MENUITEMINFOW item;
        ZeroMemory(&item, sizeof(item));
        item.cbSize = sizeof(item);
        item.fMask = MIIM_STRING;
        item.dwTypeData = const_cast<LPWSTR>(L"Ne&w");
        SetMenuItemInfoW(menu, position, TRUE, &item);
    }
    return true;
}

//****************************************************************************
//	Method:		LocalizeItems
//	FullName:	McNewMenu::LocalizeItems
//	author:
//	date:		2026/11/07
//	Function:   英文界面下改写「新建」子菜单里处理程序填进来的文字：按命令的动词识别，
//              NewFolder / NewLink 用固定英文，".xxx" 用注册表里的英文类型名；识别不了的保持原样。
//
//	Returns:	void
//	Parameter:	IN  HMENU menu    刚由处理程序填充完的子菜单
//
//              只改 MIIM_STRING，图标（hbmpItem）和 dwItemData 是处理程序自己的，保持不动
//
//	changelog:  New
//****************************************************************************
void McNewMenu::LocalizeItems(HMENU menu)
{
    if (!m_menu || McSettingsGet().language != MCFENCE_LANG_EN)
    {
        return;
    }
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i)
    {
        const UINT id = GetMenuItemID(menu, i);
        if (id == static_cast<UINT>(-1) || !Owns(id))
        {
            continue;  // 分隔线或子菜单
        }
        MENUITEMINFOW type;
        ZeroMemory(&type, sizeof(type));
        type.cbSize = sizeof(type);
        type.fMask = MIIM_FTYPE;
        if (!GetMenuItemInfoW(menu, i, TRUE, &type) || (type.fType & MFT_OWNERDRAW))
        {
            continue;  // 自绘项的文字在处理程序手里，改 MIIM_STRING 会把它变成普通项
        }
        wchar_t verb[MAX_PATH] = L"";
        if (FAILED(m_menu->GetCommandString(id - m_first, GCS_VERBW, NULL, reinterpret_cast<LPSTR>(verb),
                                            MAX_PATH)))
        {
            continue;
        }
        std::wstring text;
        if (lstrcmpiW(verb, L"NewFolder") == 0)
        {
            text = L"&Folder";
        }
        else if (lstrcmpiW(verb, L"NewLink") == 0)
        {
            text = L"&Shortcut";
        }
        else if (verb[0] == L'.')
        {
            text = EnglishTypeName(verb);
        }
        if (text.empty())
        {
            continue;
        }
        MENUITEMINFOW item;
        ZeroMemory(&item, sizeof(item));
        item.cbSize = sizeof(item);
        item.fMask = MIIM_STRING;
        item.dwTypeData = const_cast<LPWSTR>(text.c_str());
        SetMenuItemInfoW(menu, i, TRUE, &item);
    }
}

bool McNewMenu::Owns(UINT command) const
{
    return m_menu != NULL && command >= m_first && command <= m_last;
}

//****************************************************************************
//	Method:		HandleMenuMessage
//	FullName:	McNewMenu::HandleMenuMessage
//	author:
//	date:		2026/11/07
//	Function:   在菜单显示期间把菜单相关消息转发给 NewMenu 处理程序（它延迟填充子菜单并自绘图标）。
//
//	Returns:	bool    消息已被处理返回 true（result 为应返回的值）
//	Parameter:	IN  UINT message    消息编号
//              IN  WPARAM wparam    消息参数
//              IN  LPARAM lparam    消息参数
//              OUT LRESULT* result    处理结果
//
//	changelog:  New
//****************************************************************************
bool McNewMenu::HandleMenuMessage(UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result)
{
    if (!m_menu2 && !m_menu3)
    {
        return false;
    }
    switch (message)
    {
        case WM_INITMENUPOPUP:
            // 只转发「新建」自己的子菜单（及其下级）。弹出顶层菜单时也会收到 WM_INITMENUPOPUP，
            // 如果转给处理程序，它会把新建项直接塞进顶层菜单，而且这些项与它内部的命令表脱钩，
            // InvokeCommand 会返回 E_FAIL —— 菜单项被“拍平”且点了不建文件，就是这个原因。
            if (reinterpret_cast<HMENU>(wparam) == m_owner_menu)
            {
                return false;
            }
            break;
        case WM_DRAWITEM:
        case WM_MEASUREITEM:
        case WM_MENUCHAR:
            break;
        default:
            return false;
    }
    LRESULT value = 0;
    if (m_menu3)
    {
        if (FAILED(m_menu3->HandleMenuMsg2(message, wparam, lparam, &value)))
        {
            return false;
        }
    }
    else if (message == WM_MENUCHAR || FAILED(m_menu2->HandleMenuMsg(message, wparam, lparam)))
    {
        return false;
    }
    if (message == WM_INITMENUPOPUP)
    {
        LocalizeItems(reinterpret_cast<HMENU>(wparam));
    }
    if (result)
    {
        *result = (message == WM_DRAWITEM || message == WM_MEASUREITEM) ? TRUE : value;
    }
    return true;
}

//****************************************************************************
//	Method:		Invoke
//	FullName:	McNewMenu::Invoke
//	author:
//	date:		2026/11/07
//	Function:   执行「新建」子菜单的命令，并通过前后两次目录快照找出新建出来的项目。
//
//	Returns:	std::wstring    新项目完整路径；没有新项目时返回空串
//	Parameter:	IN  HWND owner    命令的所有者窗口
//              IN  UINT command    TrackPopupMenu 返回的命令 id
//
//              处理程序不会告诉我们最终名称（重名时是“新建 文本文档 (2).txt”），所以只能对比目录
//
//	changelog:  New
//****************************************************************************
std::wstring McNewMenu::Invoke(HWND owner, UINT command)
{
    if (!Owns(command))
    {
        return std::wstring();
    }
    std::vector<std::wstring> before;
    std::vector<std::wstring> after;
    SnapshotNames(m_folder, before);

    CMINVOKECOMMANDINFOEX info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_NOASYNC;
    info.hwnd = owner;
    info.lpVerb = MAKEINTRESOURCEA(command - m_first);
    info.lpVerbW = MAKEINTRESOURCEW(command - m_first);
    info.nShow = SW_SHOWNORMAL;
    const HRESULT hr = m_menu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
    if (FAILED(hr))
    {
        McLogHr(L"NewMenu InvokeCommand", hr);
    }

    SnapshotNames(m_folder, after);
    for (size_t i = 0; i < after.size(); ++i)
    {
        bool existed = false;
        for (size_t j = 0; j < before.size(); ++j)
        {
            if (McIEquals(before[j], after[i]))
            {
                existed = true;
                break;
            }
        }
        if (!existed)
        {
            return McJoinPath(m_folder, after[i]);
        }
    }
    return std::wstring();
}

//****************************************************************************
//	Method:		McPickFolder
//	FullName:	McPickFolder
//	author:
//	date:		2026/11/07
//	Function:   弹出系统“浏览文件夹”对话框，只允许选文件系统目录。
//
//	Returns:	bool    用户选定了文件夹返回 true
//	Parameter:	IN  HWND owner    对话框所有者
//              IN  const wchar_t* title    对话框说明文字
//              OUT std::wstring& folder    选中的文件夹路径
//
//              IFileOpenDialog(FOS_PICKFOLDERS) 要 Vista；SHBrowseForFolder 在 XP 到 11 上都可用
//
//	changelog:  New
//****************************************************************************
bool McPickFolder(HWND owner, const wchar_t* title, std::wstring& folder)
{
    BROWSEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.hwndOwner = owner;
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&info);
    if (!pidl)
    {
        return false;
    }
    wchar_t path[MAX_PATH] = L"";
    const BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok || path[0] == L'\0')
    {
        return false;
    }
    folder = path;
    return true;
}
