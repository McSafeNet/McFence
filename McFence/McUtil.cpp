#include "McUtil.h"

#include <stdio.h>
#include <stdarg.h>
#include <shlobj.h>


//****************************************************************************
//	Method:		McExeDirectory
//	FullName:	McExeDirectory
//	author:
//	date:		2026/11/06
//	Function:   获取当前可执行文件所在的目录路径。
//
//	Returns:	std::wstring    可执行文件所在目录；获取失败时返回空字符串。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
std::wstring McExeDirectory()
{
    wchar_t buffer[MAX_PATH * 2];
    DWORD len = GetModuleFileNameW(NULL, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    if (len == 0)
    {
        return std::wstring();
    }
    std::wstring path(buffer, len);
    return McParentDirectory(path);
}

//****************************************************************************
//	Method:		McLocalAppData
//	FullName:	McLocalAppData
//	author:
//	date:		2026/11/06
//	Function:   获取当前用户的 Local AppData 目录路径，获取失败时回退为可执行文件所在目录。
//
//	Returns:	std::wstring    Local AppData 目录路径，或失败时的回退目录。
//	Parameter:	无
//
//              使用 SHGetFolderPathW 而非 SHGetKnownFolderPath，以兼容 Windows XP。
//
//	changelog:  New
//****************************************************************************
std::wstring McLocalAppData()
{
    wchar_t buffer[MAX_PATH];
    // SHGetFolderPathW works on XP; SHGetKnownFolderPath (Vista+) is not used.
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, buffer)))
    {
        return std::wstring(buffer);
    }
    return McExeDirectory();
}

//****************************************************************************
//	Method:		McDataRoot
//	FullName:	McDataRoot
//	author:
//	date:		2026/11/06
//	Function:   获取应用数据根目录：优先读取 MCFENCE_DATA_ROOT 环境变量作为开发/测试覆盖路径，
//              否则使用 Local AppData 下的 McFence 子目录；结果按进程缓存。
//
//	Returns:	std::wstring    应用数据根目录路径。
//	Parameter:	无
//
//              结果用静态变量缓存，只计算一次。
//
//	changelog:  New
//****************************************************************************
std::wstring McDataRoot()
{
    static std::wstring cached;
    if (cached.empty())
    {
        // Dev / smoke-test override: keeps an isolated Instance away from the real
        // %LOCALAPPDATA%\SimpleGrid data.
        wchar_t override_path[MAX_PATH];
        DWORD length = GetEnvironmentVariableW(L"MCFENCE_DATA_ROOT", override_path, MAX_PATH);
        if (length > 0 && length < MAX_PATH)
        {
            cached = override_path;
        }
        else
        {
            cached = McJoinPath(McLocalAppData(), MCFENCE_DATA_FOLDER);
        }
    }
    return cached;
}

std::wstring McItemsRoot()
{
    return McJoinPath(McDataRoot(), MCFENCE_ITEMS_FOLDER);
}

//****************************************************************************
//	Method:		McEnsureDirectory
//	FullName:	McEnsureDirectory
//	author:
//	date:		2026/11/06
//	Function:   确保指定目录存在，若不存在则递归创建其缺失的父目录后再创建自身。
//
//	Returns:	bool    目录已存在或创建成功返回 true，路径为空或创建失败返回 false。
//	Parameter:	IN  const std::wstring& dir    要确保存在的目录路径。
//
//	changelog:  New
//****************************************************************************
bool McEnsureDirectory(const std::wstring& dir)
{
    if (dir.empty())
    {
        return false;
    }
    if (McDirectoryExists(dir))
    {
        return true;
    }
    // Directory.CreateDirectory semantics: create missing parents first.
    const std::wstring parent = McParentDirectory(dir);
    if (!parent.empty() && parent != dir && !McDirectoryExists(parent))
    {
        McEnsureDirectory(parent);
    }
    if (CreateDirectoryW(dir.c_str(), NULL))
    {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

//****************************************************************************
//	Method:		McRemoveDirectory
//	FullName:	McRemoveDirectory
//	author:
//	date:		2026/11/06
//	Function:   递归删除目录：先删除其中所有文件（并清除只读属性），再递归删除子目录，最后删除目录自身。
//
//	Returns:	bool    目录不存在或路径为空返回 false；否则返回最终删除目录自身是否成功。
//	Parameter:	IN  const std::wstring& dir    要删除的目录路径。
//
//              递归且不可恢复的删除操作，调用前需确认路径正确。
//
//	changelog:  New
//****************************************************************************
bool McRemoveDirectory(const std::wstring& dir)
{
    if (dir.empty() || !McDirectoryExists(dir))
    {
        return false;
    }
    // Files first, then subdirectories, then the directory itself.
    std::vector<std::wstring> files = McListFiles(dir);
    for (size_t i = 0; i < files.size(); ++i)
    {
        SetFileAttributesW(files[i].c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(files[i].c_str());
    }
    std::vector<std::wstring> dirs = McListDirectories(dir);
    for (size_t i = 0; i < dirs.size(); ++i)
    {
        McRemoveDirectory(dirs[i]);
    }
    return RemoveDirectoryW(dir.c_str()) != FALSE;
}

//****************************************************************************
//	Method:		McFileExists
//	FullName:	McFileExists
//	author:
//	date:		2026/11/06
//	Function:   判断给定路径是否存在且为普通文件（非目录）。
//
//	Returns:	bool    是文件返回 true，否则（不存在、是目录或路径为空）返回 false。
//	Parameter:	IN  const std::wstring& path    待检测的路径。
//
//	changelog:  New
//****************************************************************************
bool McFileExists(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

//****************************************************************************
//	Method:		McDirectoryExists
//	FullName:	McDirectoryExists
//	author:
//	date:		2026/11/06
//	Function:   判断给定路径是否存在且为目录。
//
//	Returns:	bool    是目录返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& path    待检测的路径。
//
//	changelog:  New
//****************************************************************************
bool McDirectoryExists(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool McPathExists(const std::wstring& path)
{
    return McFileExists(path) || McDirectoryExists(path);
}

namespace
{

//****************************************************************************
//	Method:		ListMatching
//	FullName:	ListMatching
//	author:
//	date:		2026/11/06
//	Function:   内部辅助函数：枚举目录下所有条目（跳过 "." 和 ".."），按 wanted_attributes 筛选出目录或文件，返回其完整路径列表。
//
//	Returns:	std::vector<std::wstring>    匹配到的完整路径列表，目录为空或打开失败时为空列表。
//	Parameter:	IN  const std::wstring& dir    要枚举的目录路径。
//              IN  DWORD wanted_attributes    筛选条件，传 FILE_ATTRIBUTE_DIRECTORY 返回子目录，否则返回文件。
//
//	changelog:  New
//****************************************************************************
std::vector<std::wstring> ListMatching(const std::wstring& dir, DWORD wanted_attributes)
{
    std::vector<std::wstring> result;
    if (dir.empty())
    {
        return result;
    }
    std::wstring pattern = McJoinPath(dir, L"*");
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return result;
    }
    do
    {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
        {
            continue;
        }
        bool is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (is_directory == (wanted_attributes == FILE_ATTRIBUTE_DIRECTORY))
        {
            result.push_back(McJoinPath(dir, data.cFileName));
        }
    }
    while (FindNextFileW(find, &data));
    FindClose(find);
    return result;
}

}  // namespace

std::vector<std::wstring> McListDirectories(const std::wstring& dir)
{
    return ListMatching(dir, FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<std::wstring> McListFiles(const std::wstring& dir)
{
    return ListMatching(dir, FILE_ATTRIBUTE_NORMAL);
}

//****************************************************************************
//	Method:		McJoinPath
//	FullName:	McJoinPath
//	author:
//	date:		2026/11/06
//	Function:   拼接两段路径，必要时在中间补上反斜杠；任一侧为空则直接返回另一侧。
//
//	Returns:	std::wstring    拼接后的路径。
//	Parameter:	IN  const std::wstring& a    路径前段。
//              IN  const std::wstring& b    路径后段。
//
//	changelog:  New
//****************************************************************************
std::wstring McJoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty())
    {
        return b;
    }
    if (b.empty())
    {
        return a;
    }
    wchar_t last = a[a.size() - 1];
    if (last == L'\\' || last == L'/')
    {
        return a + b;
    }
    return a + L"\\" + b;
}

//****************************************************************************
//	Method:		McParentDirectory
//	FullName:	McParentDirectory
//	author:
//	date:		2026/11/06
//	Function:   返回给定路径的父目录，特殊处理根路径与盘符（如保留 "C:\" 而非截断为 "C:"）。
//
//	Returns:	std::wstring    父目录路径；路径中没有分隔符时返回空字符串。
//	Parameter:	IN  const std::wstring& path    原始路径。
//
//	changelog:  New
//****************************************************************************
std::wstring McParentDirectory(const std::wstring& path)
{
    size_t cut = path.find_last_of(L"\\/");
    if (cut == std::wstring::npos)
    {
        return std::wstring();
    }
    if (cut == 0)
    {
        return path.substr(0, 1);
    }
    // Keep "C:\" intact instead of "C:".
    if (cut == 2 && path.size() > 2 && path[1] == L':')
    {
        return path.substr(0, 3);
    }
    return path.substr(0, cut);
}

//****************************************************************************
//	Method:		McFileName
//	FullName:	McFileName
//	author:
//	date:		2026/11/06
//	Function:   提取路径中最后一个分隔符之后的部分，即文件名（含扩展名）。
//
//	Returns:	std::wstring    文件名部分；路径中没有分隔符时返回原路径。
//	Parameter:	IN  const std::wstring& path    原始路径。
//
//	changelog:  New
//****************************************************************************
std::wstring McFileName(const std::wstring& path)
{
    size_t cut = path.find_last_of(L"\\/");
    if (cut == std::wstring::npos)
    {
        return path;
    }
    return path.substr(cut + 1);
}

//****************************************************************************
//	Method:		McFileExtension
//	FullName:	McFileExtension
//	author:
//	date:		2026/11/06
//	Function:   返回文件名中最后一个点及之后的扩展名部分。
//
//	Returns:	std::wstring    包含起始点的扩展名；没有扩展名时返回空字符串。
//	Parameter:	IN  const std::wstring& path    原始路径。
//
//	changelog:  New
//****************************************************************************
std::wstring McFileExtension(const std::wstring& path)
{
    std::wstring name = McFileName(path);
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size())
    {
        return std::wstring();
    }
    return name.substr(dot);
}


//****************************************************************************
//	Method:		McFullPath
//	FullName:	McFullPath
//	author:
//	date:		2026/11/06
//	Function:   调用 GetFullPathNameW 将相对路径解析为绝对路径。
//
//	Returns:	std::wstring    解析得到的绝对路径；解析失败或缓冲区不足时原样返回输入路径。
//	Parameter:	IN  const std::wstring& path    待解析的路径。
//
//	changelog:  New
//****************************************************************************
std::wstring McFullPath(const std::wstring& path)
{
    wchar_t buffer[MAX_PATH * 2];
    DWORD len = GetFullPathNameW(path.c_str(), static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])),
                                 buffer, NULL);
    if (len == 0 || len >= sizeof(buffer) / sizeof(buffer[0]))
    {
        return path;
    }
    return std::wstring(buffer, len);
}

//****************************************************************************
//	Method:		McIEquals
//	FullName:	McIEquals
//	author:
//	date:		2026/11/06
//	Function:   忽略大小写比较两个字符串是否相等。
//
//	Returns:	bool    长度相同且忽略大小写内容相等返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& a    第一个字符串。
//              IN  const std::wstring& b    第二个字符串。
//
//	changelog:  New
//****************************************************************************
bool McIEquals(const std::wstring& a, const std::wstring& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

//****************************************************************************
//	Method:		McIEndsWith
//	FullName:	McIEndsWith
//	author:
//	date:		2026/11/06
//	Function:   忽略大小写判断字符串是否以指定后缀结尾。
//
//	Returns:	bool    是则返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& text    被检测的字符串。
//              IN  const std::wstring& suffix    期望的后缀。
//
//	changelog:  New
//****************************************************************************
bool McIEndsWith(const std::wstring& text, const std::wstring& suffix)
{
    if (suffix.size() > text.size())
    {
        return false;
    }
    return _wcsicmp(text.c_str() + (text.size() - suffix.size()), suffix.c_str()) == 0;
}

//****************************************************************************
//	Method:		McIStartsWith
//	FullName:	McIStartsWith
//	author:
//	date:		2026/11/06
//	Function:   忽略大小写判断字符串是否以指定前缀开头。
//
//	Returns:	bool    是则返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& text    被检测的字符串。
//              IN  const std::wstring& prefix    期望的前缀。
//
//	changelog:  New
//****************************************************************************
bool McIStartsWith(const std::wstring& text, const std::wstring& prefix)
{
    if (prefix.size() > text.size())
    {
        return false;
    }
    return _wcsnicmp(text.c_str(), prefix.c_str(), prefix.size()) == 0;
}

//****************************************************************************
//	Method:		McToLower
//	FullName:	McToLower
//	author:
//	date:		2026/11/06
//	Function:   将字符串原地转换为小写后返回（参数按值传递，不影响调用方原字符串）。
//
//	Returns:	std::wstring    转换为小写后的字符串。
//	Parameter:	IN  std::wstring text    要转换的字符串副本。
//
//	changelog:  New
//****************************************************************************
std::wstring McToLower(std::wstring text)
{
    if (!text.empty())
    {
        CharLowerBuffW(&text[0], static_cast<DWORD>(text.size()));
    }
    return text;
}

//****************************************************************************
//	Method:		trim
//	FullName:	trim
//	author:
//	date:		2026/11/06
//	Function:   去除字符串首尾的空格、制表符、回车和换行等空白字符。
//
//	Returns:	std::wstring    去除首尾空白后的子串。
//	Parameter:	IN  const std::wstring& text    原始字符串。
//
//	changelog:  New
//****************************************************************************
std::wstring trim(const std::wstring& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t' || text[begin] == L'\r' ||
                           text[begin] == L'\n'))
    {
        ++begin;
    }
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t' || text[end - 1] == L'\r' ||
                           text[end - 1] == L'\n'))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

//****************************************************************************
//	Method:		McReplaceAll
//	FullName:	McReplaceAll
//	author:
//	date:		2026/11/06
//	Function:   将字符串中所有出现的子串 from 替换为 to。
//
//	Returns:	std::wstring    替换后得到的新字符串；from 为空时原样返回输入。
//	Parameter:	IN  std::wstring text    原始字符串（按值传入）。
//              IN  const std::wstring& from    要查找的子串。
//              IN  const std::wstring& to    替换成的子串。
//
//	changelog:  New
//****************************************************************************
std::wstring McReplaceAll(std::wstring text, const std::wstring& from, const std::wstring& to)
{
    if (from.empty())
    {
        return text;
    }
    std::wstring result;
    size_t pos = 0;
    for (;;)
    {
        size_t hit = text.find(from, pos);
        if (hit == std::wstring::npos)
        {
            result.append(text, pos, std::wstring::npos);
            break;
        }
        result.append(text, pos, hit - pos);
        result.append(to);
        pos = hit + from.size();
    }
    return result;
}

//****************************************************************************
//	Method:		McFormat
//	FullName:	McFormat
//	author:
//	date:		2026/11/06
//	Function:   按 printf 风格格式化参数生成宽字符串，内部使用固定大小缓冲区（1024 字符）。
//
//	Returns:	std::wstring    格式化后的字符串；格式化出错时返回缓冲区中已写入（截断）的内容。
//	Parameter:	IN  const wchar_t* fmt    printf 风格的格式字符串，后跟可变数量的参数。
//
//              结果最长受限于 1024 字符的内部缓冲区，过长内容会被截断。
//
//	changelog:  New
//****************************************************************************
std::wstring McFormat(const wchar_t* fmt, ...)
{
    wchar_t buffer[1024];
    va_list args;
    va_start(args, fmt);
    int written = _vsnwprintf(buffer, sizeof(buffer) / sizeof(buffer[0]) - 1, fmt, args);
    va_end(args);
    if (written < 0)
    {
        buffer[sizeof(buffer) / sizeof(buffer[0]) - 1] = L'\0';
        return std::wstring(buffer);
    }
    return std::wstring(buffer, static_cast<size_t>(written));
}

//****************************************************************************
//	Method:		McNewGuid
//	FullName:	McNewGuid
//	author:
//	date:		2026/11/06
//	Function:   生成一个新的 GUID，去掉花括号并转换为小写的字符串形式。
//
//	Returns:	std::wstring    小写、不带花括号的 GUID 字符串；CoCreateGuid 失败时返回全零 GUID 字符串。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
std::wstring McNewGuid()
{
    GUID guid;
    if (FAILED(CoCreateGuid(&guid)))
    {
        return L"00000000-0000-0000-0000-000000000000";
    }
    wchar_t buffer[64];
    StringFromGUID2(guid, buffer, 64);
    // StringFromGUID2 returns "{xxxxxxxx-....}"; the XML stores the bare form.
    std::wstring text(buffer);
    if (!text.empty() && text[0] == L'{')
    {
        text = text.substr(1, text.size() - 2);
    }
    return McToLower(text);
}

//****************************************************************************
//	Method:		McIsGuidString
//	FullName:	McIsGuidString
//	author:
//	date:		2026/11/06
//	Function:   校验字符串是否为标准格式（36 字符、连字符位置正确、其余为十六进制字符）的 GUID 文本。
//
//	Returns:	bool    符合 GUID 格式返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& text    待校验的字符串。
//
//	changelog:  New
//****************************************************************************
bool McIsGuidString(const std::wstring& text)
{
    if (text.size() != 36)
    {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i)
    {
        wchar_t c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (c != L'-')
            {
                return false;
            }
        }
        else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F')))
        {
            return false;
        }
    }
    return true;
}

//****************************************************************************
//	Method:		McNowStamp
//	FullName:	McNowStamp
//	author:
//	date:		2026/11/06
//	Function:   获取当前本地时间，并格式化为 "yyyy-MM-dd HH:mm:ss.fff" 形式的字符串。
//
//	Returns:	std::wstring    格式化后的当前本地时间字符串。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
std::wstring McNowStamp()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return McFormat(L"%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay, st.wHour,
                       st.wMinute, st.wSecond, st.wMilliseconds);
}

//****************************************************************************
//	Method:		McTicks
//	FullName:	McTicks
//	author:
//	date:		2026/11/06
//	Function:   基于 QueryPerformanceCounter 返回自首次调用以来经过的毫秒数，作为单调递增的计时基准。
//
//	Returns:	ULONGLONG    自首次调用起经过的毫秒数。
//	Parameter:	无
//
//              选用 QueryPerformanceCounter 而非 GetTickCount64，以兼容 Windows XP。
//
//	changelog:  New
//****************************************************************************
ULONGLONG McTicks()
{
    // GetTickCount64 is Vista+; QueryPerformanceCounter is monotonic on XP as well.
    static LARGE_INTEGER frequency = LARGE_INTEGER();
    static LARGE_INTEGER origin = LARGE_INTEGER();
    if (frequency.QuadPart == 0)
    {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&origin);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<ULONGLONG>((now.QuadPart - origin.QuadPart) * 1000 / frequency.QuadPart);
}

bool McCopyFile(const std::wstring& src, const std::wstring& dst, bool overwrite)
{
    return CopyFileW(src.c_str(), dst.c_str(), overwrite ? FALSE : TRUE) != FALSE;
}

//****************************************************************************
//	Method:		McCopyDirectory
//	FullName:	McCopyDirectory
//	author:
//	date:		2026/11/06
//	Function:   递归地将 src 目录下的所有文件和子目录复制到 dst 目录（会先确保 dst 存在）。
//
//	Returns:	bool    目标目录创建失败返回 false；否则返回所有文件/子目录是否全部复制成功。
//	Parameter:	IN  const std::wstring& src    源目录路径。
//              IN  const std::wstring& dst    目标目录路径。
//
//              复制文件时不覆盖已存在的同名文件。
//
//	changelog:  New
//****************************************************************************
bool McCopyDirectory(const std::wstring& src, const std::wstring& dst)
{
    if (!McEnsureDirectory(dst))
    {
        return false;
    }
    bool ok = true;
    std::vector<std::wstring> files = McListFiles(src);
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (!McCopyFile(files[i], McJoinPath(dst, McFileName(files[i])), false))
        {
            ok = false;
        }
    }
    std::vector<std::wstring> dirs = McListDirectories(src);
    for (size_t i = 0; i < dirs.size(); ++i)
    {
        if (!McCopyDirectory(dirs[i], McJoinPath(dst, McFileName(dirs[i]))))
        {
            ok = false;
        }
    }
    return ok;
}

//****************************************************************************
//	Method:		McWriteFileBytes
//	FullName:	McWriteFileBytes
//	author:
//	date:		2026/11/06
//	Function:   将字节缓冲区完整写入指定文件，覆盖已存在的文件。
//
//	Returns:	bool    文件创建及写入全部成功返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& path    目标文件路径。
//              IN  const std::string& bytes    要写入的字节数据。
//
//	changelog:  New
//****************************************************************************
bool McWriteFileBytes(const std::wstring& path, const std::string& bytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD written = 0;
    bool ok = true;
    if (!bytes.empty())
    {
        ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, NULL) != FALSE &&
             written == bytes.size();
    }
    CloseHandle(file);
    return ok;
}

//****************************************************************************
//	Method:		McReadFileBytes
//	FullName:	McReadFileBytes
//	author:
//	date:		2026/11/06
//	Function:   读取指定文件的全部内容到字节缓冲区，限制文件大小不超过 64MB。
//
//	Returns:	bool    打开、获取大小及读取全部成功返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& path    要读取的文件路径。
//              OUT std::string& bytes    用于接收读取到的文件内容；失败时被清空。
//
//              超过 64MB 的文件会被直接拒绝读取。
//
//	changelog:  New
//****************************************************************************
bool McReadFileBytes(const std::wstring& path, std::string& bytes)
{
    bytes.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > (64LL << 20))
    {
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    bool ok = true;
    if (!bytes.empty())
    {
        ok = ReadFile(file, &bytes[0], static_cast<DWORD>(bytes.size()), &read, NULL) != FALSE &&
             read == bytes.size();
    }
    CloseHandle(file);
    return ok;
}

//****************************************************************************
//	Method:		McToUtf8
//	FullName:	McToUtf8
//	author:
//	date:		2026/11/06
//	Function:   将宽字符字符串转换为 UTF-8 编码的字节串。
//
//	Returns:	std::string    转换得到的 UTF-8 字节串；输入为空或转换失败时返回空字符串。
//	Parameter:	IN  const std::wstring& text    要转换的宽字符字符串。
//
//	changelog:  New
//****************************************************************************
std::string McToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return std::string();
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), NULL, 0,
                                     NULL, NULL);
    if (needed <= 0)
    {
        return std::string();
    }
    std::string result(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &result[0], needed,
                        NULL, NULL);
    return result;
}

//****************************************************************************
//	Method:		McFromUtf8
//	FullName:	McFromUtf8
//	author:
//	date:		2026/11/06
//	Function:   将 UTF-8 编码的字节串转换为宽字符字符串。
//
//	Returns:	std::wstring    转换得到的宽字符字符串；输入为空或转换失败时返回空字符串。
//	Parameter:	IN  const std::string& text    要转换的 UTF-8 字节串。
//
//	changelog:  New
//****************************************************************************
std::wstring McFromUtf8(const std::string& text)
{
    if (text.empty())
    {
        return std::wstring();
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), NULL, 0);
    if (needed <= 0)
    {
        return std::wstring();
    }
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &result[0], needed);
    return result;
}

void McAppendUtf8Bom(std::string& bytes)
{
    // Kept in an array: MSVC warns (C4309) about truncating a 0xEF constant to char.
    static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    bytes.append(reinterpret_cast<const char*>(bom), sizeof(bom));
}

