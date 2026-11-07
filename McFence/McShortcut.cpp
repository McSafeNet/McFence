#include "McShortcut.h"
#include "McLog.h"
#include "McUtil.h"

#include <shlobj.h>
#include <shobjidl.h>


namespace
{

//****************************************************************************
//	Method:		QuoteArgument
//	FullName:	QuoteArgument
//	author:
//	date:		2026/11/06
//	Function:   给字符串两端加上双引号，用于拼接命令行参数。
//
//	Returns:	std::wstring    加上双引号后的字符串。
//	Parameter:	IN  const std::wstring& text    需要加引号的原始文本
//
//	changelog:  New
//****************************************************************************
std::wstring QuoteArgument(const std::wstring& text)
{
    std::wstring out = L"\"";
    out += text;
    out += L"\"";
    return out;
}

}  // namespace

//****************************************************************************
//	Method:		McResolveShortcut
//	FullName:	McResolveShortcut
//	author:
//	date:		2026/11/06
//	Function:   解析 .lnk 快捷方式文件，取出其目标路径、命令行参数和工作目录。
//
//	Returns:	bool    解析成功且目标路径非空时返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& lnk_path    快捷方式文件路径
//              OUT std::wstring& target    解析得到的目标路径
//              OUT std::wstring& arguments    解析得到的命令行参数
//              OUT std::wstring& working_directory    解析得到的工作目录
//
//              内部使用 IShellLinkW/IPersistFile，调用前当前线程需已初始化 COM
//
//	changelog:  New
//****************************************************************************
bool McResolveShortcut(const std::wstring& lnk_path, std::wstring& target, std::wstring& arguments,
                      std::wstring& working_directory)
{
    target.clear();
    arguments.clear();
    working_directory.clear();

    IShellLinkW* link = NULL;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                               reinterpret_cast<void**>(&link))) ||
        !link)
    {
        return false;
    }
    bool ok = false;
    IPersistFile* file = NULL;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))) && file)
    {
        if (SUCCEEDED(file->Load(lnk_path.c_str(), STGM_READ)))
        {
            wchar_t buffer[MAX_PATH * 2];
            WIN32_FIND_DATAW data;
            ZeroMemory(&data, sizeof(data));
            if (SUCCEEDED(link->GetPath(buffer, MAX_PATH * 2, &data, 0)))
            {
                target = buffer;
            }
            ZeroMemory(buffer, sizeof(buffer));
            if (SUCCEEDED(link->GetArguments(buffer, MAX_PATH * 2)))
            {
                arguments = buffer;
            }
            ZeroMemory(buffer, sizeof(buffer));
            if (SUCCEEDED(link->GetWorkingDirectory(buffer, MAX_PATH * 2)))
            {
                working_directory = buffer;
            }
            ok = !target.empty();
        }
        file->Release();
    }
    link->Release();
    return ok;
}

//****************************************************************************
//	Method:		McOpenPath
//	FullName:	McOpenPath
//	author:
//	date:		2026/11/06
//	Function:   打开文件、文件夹或快捷方式：文件夹用 ShellExecute 打开；
//              指向 .exe 的 .lnk 直接 CreateProcess 启动目标；其余交给 shell 默认打开方式。
//
//	Returns:	bool    成功启动或打开返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& path    要打开的文件、文件夹或快捷方式路径
//
//              调用方需在当前线程完成 COM 初始化（IShellLink 与 ShellExecuteEx 都依赖 COM）
//
//	changelog:  New
//****************************************************************************
bool McOpenPath(const std::wstring& path)
{
    if (!McPathExists(path))
    {
        McLogFormat(L"无法打开不存在的项目 '%s'", path.c_str());
        return false;
    }
    if (McDirectoryExists(path))
    {
        HINSTANCE result = ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(result) > 32;
    }

    if (McIEndsWith(path, L".lnk"))
    {
        std::wstring target;
        std::wstring arguments;
        std::wstring working_directory;
        if (McResolveShortcut(path, target, arguments, working_directory) && McFileExists(target) &&
            McIEndsWith(target, L".exe"))
        {
            std::wstring command = QuoteArgument(target);
            if (!arguments.empty())
            {
                command += L" ";
                command += arguments;
            }
            std::wstring directory = McDirectoryExists(working_directory) ? working_directory
                                                                        : McParentDirectory(target);
            std::vector<wchar_t> mutable_command(command.begin(), command.end());
            mutable_command.push_back(L'\0');
            STARTUPINFOW startup;
            ZeroMemory(&startup, sizeof(startup));
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process;
            ZeroMemory(&process, sizeof(process));
            // UseShellExecute = false equivalent: no shell involvement.
            if (CreateProcessW(target.c_str(), &mutable_command[0], NULL, NULL, FALSE,
                               CREATE_UNICODE_ENVIRONMENT, NULL, directory.c_str(), &startup,
                               &process))
            {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                return true;
            }
            McLogLastError(L"CreateProcessW(shortcut target)");
            // Fall through to the shell below.
        }
    }

    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;  // the worker thread exits right after
    info.lpVerb = L"open";
    info.lpFile = path.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info))
    {
        McLogLastError(L"ShellExecuteExW(item)");
        return false;
    }
    return true;
}

namespace
{

//****************************************************************************
//	Method:		OpenPathThread
//	FullName:	OpenPathThread
//	author:
//	date:		2026/11/06
//	Function:   线程池回调：初始化 COM 后调用 McOpenPath 打开路径，结束后释放参数并反初始化 COM。
//
//	Returns:	DWORD    固定返回 0。
//	Parameter:	IN  void* parameter    指向堆分配的 std::wstring 路径，函数内部负责释放
//
//              parameter 指向的字符串由本函数 delete，调用方不应再访问
//
//	changelog:  New
//****************************************************************************
DWORD WINAPI OpenPathThread(void* parameter)
{
    std::wstring* path = static_cast<std::wstring*>(parameter);
    // IShellLink and ShellExecuteEx both need COM on this thread.
    const HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    McOpenPath(*path);
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
    delete path;
    return 0;
}

}  // namespace

//****************************************************************************
//	Method:		McOpenPathAsync
//	FullName:	McOpenPathAsync
//	author:
//	date:		2026/11/06
//	Function:   在线程池线程中异步调用 McOpenPath，避免打开慢速路径（如网络位置）阻塞围栏界面。
//
//	Returns:	void
//	Parameter:	IN  const std::wstring& path    要打开的路径
//
//              QueueUserWorkItem 投递失败时退化为同步调用 McOpenPath
//
//	changelog:  New
//****************************************************************************
void McOpenPathAsync(const std::wstring& path)
{
    std::wstring* copy = new std::wstring(path);
    // Task.Run in FenceEntry.Open(): a slow network path or shell handler must
    // not freeze the fence.
    if (!QueueUserWorkItem(&OpenPathThread, copy, WT_EXECUTELONGFUNCTION))
    {
        delete copy;
        McOpenPath(path);
    }
}

//****************************************************************************
//	Method:		McLocateInExplorer
//	FullName:	McLocateInExplorer
//	author:
//	date:		2026/11/06
//	Function:   在资源管理器中定位并选中指定项目；优先使用 SHOpenFolderAndSelectItems，
//              失败则回退为启动 explorer.exe /select。
//
//	Returns:	bool    成功定位返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& path    要在资源管理器中定位的文件或文件夹路径
//
//	changelog:  New
//****************************************************************************
bool McLocateInExplorer(const std::wstring& path)
{
    // Preferred: the shell API (Windows XP+). It reuses an open Explorer window
    // and selects the item even when the path needs quoting.
    LPITEMIDLIST pidl = ILCreateFromPathW(path.c_str());
    if (pidl)
    {
        const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
        ILFree(pidl);
        if (SUCCEEDED(hr))
        {
            return true;
        }
        McLogHr(L"SHOpenFolderAndSelectItems", hr);
    }

    // Fallback: explorer.exe /select,"path". The executable must be part of the
    // command line - CreateProcess treats the first token as the program to run.
    wchar_t windows_dir[MAX_PATH];
    const UINT length = GetWindowsDirectoryW(windows_dir, MAX_PATH);
    const std::wstring explorer = (length > 0 && length < MAX_PATH)
                                      ? McJoinPath(windows_dir, L"explorer.exe")
                                      : std::wstring(L"explorer.exe");
    std::wstring command = QuoteArgument(explorer) + L" /select," + QuoteArgument(path);
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process;
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(explorer.c_str(), &mutable_command[0], NULL, NULL, FALSE, 0, NULL, NULL,
                        &startup, &process))
    {
        McLogLastError(L"CreateProcessW(explorer /select)");
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

namespace
{

//****************************************************************************
//	Method:		LocateThread
//	FullName:	LocateThread
//	author:
//	date:		2026/11/06
//	Function:   线程池回调：初始化 COM 后调用 McLocateInExplorer，结束后释放参数并反初始化 COM。
//
//	Returns:	DWORD    固定返回 0。
//	Parameter:	IN  void* parameter    指向堆分配的 std::wstring 路径，函数内部负责释放
//
//	changelog:  New
//****************************************************************************
DWORD WINAPI LocateThread(void* parameter)
{
    std::wstring* path = static_cast<std::wstring*>(parameter);
    const HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    McLocateInExplorer(*path);
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
    delete path;
    return 0;
}

}  // namespace

//****************************************************************************
//	Method:		McLocateInExplorerAsync
//	FullName:	McLocateInExplorerAsync
//	author:
//	date:		2026/11/06
//	Function:   在线程池线程中异步调用 McLocateInExplorer，避免打开资源管理器窗口耗时导致围栏卡顿。
//
//	Returns:	void
//	Parameter:	IN  const std::wstring& path    要定位的路径
//
//              QueueUserWorkItem 投递失败时退化为同步调用 McLocateInExplorer
//
//	changelog:  New
//****************************************************************************
void McLocateInExplorerAsync(const std::wstring& path)
{
    std::wstring* copy = new std::wstring(path);
    // Opening an Explorer window can take a while; keep the fence responsive.
    if (!QueueUserWorkItem(&LocateThread, copy, WT_EXECUTELONGFUNCTION))
    {
        delete copy;
        McLocateInExplorer(path);
    }
}

