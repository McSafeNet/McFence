#include "McLog.h"
#include "McUtil.h"

#include <stdarg.h>
#include <stdio.h>


namespace
{

//****************************************************************************
//	Method:		LogLock
//	FullName:	LogLock
//	author:
//	date:		2026/11/06
//	Function:   返回一个延迟初始化的静态临界区，用于序列化日志文件的写入。
//
//	Returns:	CRITICAL_SECTION&    对静态临界区对象的引用，供调用方加锁/解锁使用。
//	Parameter:	无
//
//              首次调用时才初始化临界区，之后所有调用返回同一实例
//
//	changelog:  New
//****************************************************************************
CRITICAL_SECTION& LogLock()
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

std::wstring LogPath()
{
    std::wstring dir = McDataRoot();
    McEnsureDirectory(dir);
    return McJoinPath(dir, MCFENCE_LOG_FILE);
}

}  // namespace

//****************************************************************************
//	Method:		McLog
//	FullName:	McLog
//	author:
//	date:		2026/11/06
//	Function:   将一条带时间戳和线程 ID 的日志行以 UTF-8（必要时带 BOM）追加写入 app_diag.log。
//
//	Returns:	void
//	Parameter:	IN  const std::wstring& message    要写入日志的消息文本
//
//              通过 LogLock 临界区保证多线程下写入安全
//              文件首次创建时会先写入 UTF-8 BOM
//
//	changelog:  New
//****************************************************************************
void McLog(const std::wstring& message)
{
    std::wstring line = McNowStamp() + L" [tid" +
                        McFormat(L"%lu", static_cast<unsigned long>(GetCurrentThreadId())) + L"] " +
                        message + L"\r\n";
    std::string utf8 = McToUtf8(line);
    // UTF-8 BOM on first write, like File.AppendAllText(..., Encoding.UTF8).
    bool needs_bom = !McFileExists(LogPath());

    EnterCriticalSection(&LogLock());
    HANDLE file = CreateFileW(LogPath().c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        if (needs_bom)
        {
            std::string bom;
            McAppendUtf8Bom(bom);
            WriteFile(file, bom.data(), static_cast<DWORD>(bom.size()), &written, NULL);
        }
        if (!utf8.empty())
        {
            WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, NULL);
        }
        CloseHandle(file);
    }
    LeaveCriticalSection(&LogLock());
}

//****************************************************************************
//	Method:		McLogFormat
//	FullName:	McLogFormat
//	author:
//	date:		2026/11/06
//	Function:   按 printf 风格格式化可变参数字符串，再调用 McLog 写入日志。
//
//	Returns:	void
//	Parameter:	IN  const wchar_t* fmt    格式字符串
//              IN  ... ...    与格式字符串对应的可变参数
//
//              内部缓冲区固定 2048 字符，超长内容会被截断
//
//	changelog:  New
//****************************************************************************
void McLogFormat(const wchar_t* fmt, ...)
{
    wchar_t buffer[2048];
    va_list args;
    va_start(args, fmt);
    int written = _vsnwprintf(buffer, sizeof(buffer) / sizeof(buffer[0]) - 1, fmt, args);
    va_end(args);
    if (written < 0)
    {
        buffer[sizeof(buffer) / sizeof(buffer[0]) - 1] = L'\0';
    }
    McLog(std::wstring(buffer));
}

void McLogHr(const wchar_t* what, HRESULT hr)
{
    McLogFormat(L"%s failed: HRESULT=0x%08lX", what, static_cast<unsigned long>(hr));
}

//****************************************************************************
//	Method:		McLogWin32
//	FullName:	McLogWin32
//	author:
//	date:		2026/11/06
//	Function:   将 Win32 错误码转换成系统消息文本，连同操作描述一起写入日志。
//
//	Returns:	void
//	Parameter:	IN  const wchar_t* what    发生失败的操作描述
//              IN  DWORD error    Win32 错误码
//
//              使用 FormatMessageW 分配消息缓冲区，并在使用后通过 LocalFree 释放
//
//	changelog:  New
//****************************************************************************
void McLogWin32(const wchar_t* what, DWORD error)
{
    wchar_t* text = NULL;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error, 0, reinterpret_cast<wchar_t*>(&text), 0, NULL);
    std::wstring detail;
    if (length != 0 && text != NULL)
    {
        detail.assign(text, length);
        while (!detail.empty() && (detail[detail.size() - 1] == L'\r' || detail[detail.size() - 1] == L'\n'))
        {
            detail.erase(detail.size() - 1);
        }
    }
    if (text)
    {
        LocalFree(text);
    }
    McLogFormat(L"%s failed: %lu (%s)", what, static_cast<unsigned long>(error), detail.c_str());
}

void McLogLastError(const wchar_t* what)
{
    McLogWin32(what, GetLastError());
}

