#include "McStore.h"
#include "McLog.h"
#include "McUtil.h"

#include <wchar.h>

#include <algorithm>
#include <map>


namespace
{

//****************************************************************************
//	Method:		HasNeverShowExt
//	FullName:	HasNeverShowExt
//	author:
//	date:		2026/11/07
//	Function:   判断 HKCR\<子键> 下是否存在 NeverShowExt 值。
//
//	Returns:	bool    存在时返回 true。
//	Parameter:	IN  const std::wstring& sub_key    HKCR 下的子键名（扩展名或 ProgID）
//
//	changelog:  New
//****************************************************************************
bool HasNeverShowExt(const std::wstring& sub_key)
{
    HKEY hkey = NULL;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, sub_key.c_str(), 0, KEY_QUERY_VALUE, &hkey) != ERROR_SUCCESS)
    {
        return false;
    }
    const bool found = RegQueryValueExW(hkey, L"NeverShowExt", NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    RegCloseKey(hkey);
    return found;
}

//****************************************************************************
//	Method:		NeverShowExtension
//	FullName:	NeverShowExtension
//	author:
//	date:		2026/11/07
//	Function:   判断扩展名对应的文件类型是否注册了 NeverShowExt（例如 .lnk、.url、.pif），
//              这类文件在资源管理器中始终不显示扩展名。结果按小写扩展名缓存，避免每次绘制都查注册表。
//
//	Returns:	bool    HKCR\<扩展名> 或其 ProgID 键下存在 NeverShowExt 值时返回 true。
//	Parameter:	IN  const std::wstring& extension    带点的扩展名，例如 ".lnk"
//
//	changelog:  New
//****************************************************************************
bool NeverShowExtension(const std::wstring& extension)
{
    static std::map<std::wstring, bool> s_cache;
    const std::wstring key = McToLower(extension);
    std::map<std::wstring, bool>::const_iterator it = s_cache.find(key);
    if (it != s_cache.end())
    {
        return it->second;
    }

    bool never_show = HasNeverShowExt(key);
    if (!never_show)
    {
        wchar_t prog_id[256] = L"";
        LONG bytes = sizeof(prog_id);
        if (RegQueryValueW(HKEY_CLASSES_ROOT, key.c_str(), prog_id, &bytes) == ERROR_SUCCESS && prog_id[0] != L'\0')
        {
            never_show = HasNeverShowExt(prog_id);
        }
    }
    s_cache[key] = never_show;
    return never_show;
}

}  // namespace


//****************************************************************************
//	Method:		display_name
//	FullName:	McEntry::display_name
//	author:
//	date:		2026/11/06
//	Function:   返回带扩展名的完整文件名，作为围栏条目的显示名称（与 openFence 一致）。
//              快捷方式（.lnk / .url 等注册了 NeverShowExt 的类型）和资源管理器一样不显示扩展名。
//
//	Returns:	std::wstring    完整文件名；NeverShowExt 类型返回去掉扩展名的文件名。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
std::wstring McEntry::display_name() const
{
    std::wstring name = McFileName(path);
    if (type == MCFENCE_ENTRY_FOLDER)
    {
        return name;
    }
    std::wstring extension = McFileExtension(name);
    if (extension.empty() || extension.size() >= name.size() || !NeverShowExtension(extension))
    {
        return name;
    }
    return name.substr(0, name.size() - extension.size());
}

//****************************************************************************
//	Method:		McEntryFromPath
//	FullName:	McEntryFromPath
//	author:
//	date:		2026/11/06
//	Function:   根据路径判断其是文件还是文件夹，并据此填充 McEntry。
//
//	Returns:	bool    路径存在（是文件或文件夹）时返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& path    要检测的路径
//              OUT McEntry& out    填充路径和类型信息的输出条目
//
//	changelog:  New
//****************************************************************************
bool McEntryFromPath(const std::wstring& path, McEntry& out)
{
    if (McFileExists(path))
    {
        out.path = path;
        out.type = MCFENCE_ENTRY_FILE;
        return true;
    }
    if (McDirectoryExists(path))
    {
        out.path = path;
        out.type = MCFENCE_ENTRY_FOLDER;
        return true;
    }
    return false;
}

bool McItemExists(const std::wstring& path)
{
    return McFileExists(path) || McDirectoryExists(path);
}


const wchar_t* const MCFENCE_META_FILE = L"__fence_metadata.xml";
const wchar_t* const MCFENCE_INDEX_FILE = L"fences.index";

namespace
{

//****************************************************************************
//	Method:		ImportLock
//	FullName:	ImportLock
//	author:
//	date:		2026/11/06
//	Function:   返回一个延迟初始化的静态临界区，用于保护项目导入 Items 仓库的过程。
//
//	Returns:	CRITICAL_SECTION&    对静态临界区对象的引用。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
CRITICAL_SECTION& ImportLock()
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
//	Method:		EscapeXml
//	FullName:	EscapeXml
//	author:
//	date:		2026/11/06
//	Function:   转义文本中的 &、< 和 > 三个字符，用于写入 XML 元素内容（与 XmlSerializer 行为一致，不转义引号）。
//
//	Returns:	std::wstring    转义后的文本。
//	Parameter:	IN  const std::wstring& text    待转义的原始文本
//
//	changelog:  New
//****************************************************************************
// XmlSerializer (XmlTextWriter) escapes only &, < and > in element text; quotes
// are left alone, so escaping them would break the byte-for-byte match.
std::wstring EscapeXml(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i)
    {
        wchar_t c = text[i];
        switch (c)
        {
            case L'&':
                out += L"&amp;";
                break;
            case L'<':
                out += L"&lt;";
                break;
            case L'>':
                out += L"&gt;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

//****************************************************************************
//	Method:		AppendCodePoint
//	FullName:	AppendCodePoint
//	author:
//	date:		2026/11/06
//	Function:   将一个 Unicode 码点以 UTF-16 形式追加到字符串，超出基本多语言平面的码点编码为代理对。
//
//	Returns:	void
//	Parameter:	OUT std::wstring& out    追加编码结果的目标字符串
//              IN  unsigned long code    要追加的 Unicode 码点
//
//	changelog:  New
//****************************************************************************
// Appends a code point as UTF-16 (surrogate pair above the BMP).
void AppendCodePoint(std::wstring& out, unsigned long code)
{
    if (code >= 0x10000 && code <= 0x10FFFF)
    {
        code -= 0x10000;
        out += static_cast<wchar_t>(0xD800 + (code >> 10));
        out += static_cast<wchar_t>(0xDC00 + (code & 0x3FF));
    }
    else if (code > 0 && code < 0x10000)
    {
        out += static_cast<wchar_t>(code);
    }
}

//****************************************************************************
//	Method:		UnescapeXml
//	FullName:	UnescapeXml
//	author:
//	date:		2026/11/06
//	Function:   反转义 XML 文本中的实体引用，支持 &amp;/&lt;/&gt;/&quot;/&apos; 以及十进制、十六进制数字字符引用。
//
//	Returns:	std::wstring    反转义后的文本。
//	Parameter:	IN  const std::wstring& text    包含 XML 实体的原始文本
//
//              单遍扫描，保证类似 &amp;lt; 的双重转义能正确还原为 &lt;
//
//	changelog:  New
//****************************************************************************
// Single pass, so "&amp;lt;" correctly becomes "&lt;". Handles the five named
// entities plus decimal / hexadecimal character references.
std::wstring UnescapeXml(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != L'&')
        {
            out += text[i];
            continue;
        }
        const size_t end = text.find(L';', i + 1);
        if (end == std::wstring::npos || end - i > 12)
        {
            out += text[i];
            continue;
        }
        const std::wstring entity = text.substr(i + 1, end - i - 1);
        if (entity == L"amp")
        {
            out += L'&';
        }
        else if (entity == L"lt")
        {
            out += L'<';
        }
        else if (entity == L"gt")
        {
            out += L'>';
        }
        else if (entity == L"quot")
        {
            out += L'"';
        }
        else if (entity == L"apos")
        {
            out += L'\'';
        }
        else if (entity.size() > 1 && entity[0] == L'#')
        {
            const bool hex = entity[1] == L'x' || entity[1] == L'X';
            AppendCodePoint(out, wcstoul(entity.c_str() + (hex ? 2 : 1), NULL, hex ? 16 : 10));
        }
        else
        {
            out += text[i];  // unknown entity: keep the text as is
            continue;
        }
        i = end;
    }
    return out;
}

//****************************************************************************
//	Method:		ReplaceFileAtomic
//	FullName:	ReplaceFileAtomic
//	author:
//	date:		2026/11/06
//	Function:   用临时文件原子方式替换目标文件；ReplaceFile 不可用时退化为 MoveFileEx 覆盖移动。
//
//	Returns:	bool    替换或移动成功返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& temp    用于替换的临时文件路径
//              IN  const std::wstring& dest    目标文件路径
//
//	changelog:  New
//****************************************************************************
// Replaces dest with temp, keeping the same semantics as File.Replace/Move.
bool ReplaceFileAtomic(const std::wstring& temp, const std::wstring& dest)
{
    if (McFileExists(dest))
    {
        if (ReplaceFileW(dest.c_str(), temp.c_str(), NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL))
        {
            return true;
        }
        // ReplaceFile fails when the volume or the sharing mode does not allow it;
        // fall back to a plain move so the update still lands.
        return MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    }
    return MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

//****************************************************************************
//	Method:		ReadTextFile
//	FullName:	ReadTextFile
//	author:
//	date:		2026/11/06
//	Function:   读取文本文件并按 BOM 自动识别编码（UTF-8 BOM、UTF-16LE BOM，默认按 UTF-8 处理），
//              转换为 std::wstring。
//
//	Returns:	bool    读取成功返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& path    要读取的文件路径
//              OUT std::wstring& text    读取并解码后的文本内容
//
//	changelog:  New
//****************************************************************************
bool ReadTextFile(const std::wstring& path, std::wstring& text)
{
    std::string bytes;
    if (!McReadFileBytes(path, bytes))
    {
        return false;
    }
    size_t begin = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        begin = 3;
    }
    // XmlSerializer with a StreamWriter emits UTF-8; tolerate UTF-16 files too.
    if (bytes.size() >= begin + 2 && static_cast<unsigned char>(bytes[begin]) == 0xFF &&
        static_cast<unsigned char>(bytes[begin + 1]) == 0xFE)
    {
        const wchar_t* data = reinterpret_cast<const wchar_t*>(bytes.data() + begin + 2);
        size_t count = (bytes.size() - begin - 2) / sizeof(wchar_t);
        text.assign(data, count);
        return true;
    }
    text = McFromUtf8(bytes.substr(begin));
    return true;
}

//****************************************************************************
//	Method:		WriteTextFile
//	FullName:	WriteTextFile
//	author:
//	date:		2026/11/06
//	Function:   以带 UTF-8 BOM 的编码将文本写入文件。
//
//	Returns:	bool    写入成功返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& path    目标文件路径
//              IN  const std::wstring& text    要写入的文本内容
//
//	changelog:  New
//****************************************************************************
bool WriteTextFile(const std::wstring& path, const std::wstring& text)
{
    std::string bytes;
    McAppendUtf8Bom(bytes);
    bytes += McToUtf8(text);
    return McWriteFileBytes(path, bytes);
}

//****************************************************************************
//	Method:		ElementValue
//	FullName:	ElementValue
//	author:
//	date:		2026/11/06
//	Function:   在简单 XML 文本中查找 <name>...</name> 元素，提取并反转义其文本值。
//
//	Returns:	bool    找到该元素返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& text    待查找的 XML 文本
//              IN  const wchar_t* name    元素名称
//              OUT std::wstring& value    提取到的元素文本值
//
//	changelog:  New
//****************************************************************************
// <name>value</name> Lookup; returns false when the element is absent.
bool ElementValue(const std::wstring& text, const wchar_t* name, std::wstring& value)
{
    const std::wstring open = std::wstring(L"<") + name + L">";
    size_t start = text.find(open);
    if (start == std::wstring::npos)
    {
        return false;
    }
    // Skip attributes (<Name something="...">) although XmlSerializer never writes any.
    start = text.find(L'>', start);
    if (start == std::wstring::npos)
    {
        return false;
    }
    const std::wstring close = std::wstring(L"</") + name + L">";
    size_t end = text.find(close, start + 1);
    if (end == std::wstring::npos)
    {
        return false;
    }
    value = UnescapeXml(text.substr(start + 1, end - start - 1));
    return true;
}

//****************************************************************************
//	Method:		ElementInt
//	FullName:	ElementInt
//	author:
//	date:		2026/11/06
//	Function:   读取指定 XML 元素并转换为整数，元素缺失时返回默认值。
//
//	Returns:	int    解析得到的整数；元素缺失时返回 fallback。
//	Parameter:	IN  const std::wstring& text    待查找的 XML 文本
//              IN  const wchar_t* name    元素名称
//              IN  int fallback    元素缺失时使用的默认值
//
//	changelog:  New
//****************************************************************************
int ElementInt(const std::wstring& text, const wchar_t* name, int fallback)
{
    std::wstring value;
    if (!ElementValue(text, name, value))
    {
        return fallback;
    }
    return _wtoi(trim(value).c_str());
}

//****************************************************************************
//	Method:		ElementBool
//	FullName:	ElementBool
//	author:
//	date:		2026/11/06
//	Function:   读取指定 XML 元素并解析为布尔值（"true"/"false"，忽略大小写），
//              元素缺失或值无法识别时返回默认值。
//
//	Returns:	bool    解析得到的布尔值；元素缺失或无法识别时返回 fallback。
//	Parameter:	IN  const std::wstring& text    待查找的 XML 文本
//              IN  const wchar_t* name    元素名称
//              IN  bool fallback    元素缺失或值无法识别时使用的默认值
//
//	changelog:  New
//****************************************************************************
bool ElementBool(const std::wstring& text, const wchar_t* name, bool fallback)
{
    std::wstring value;
    if (!ElementValue(text, name, value))
    {
        return fallback;
    }
    value = McToLower(trim(value));
    if (value == L"true")
    {
        return true;
    }
    if (value == L"false")
    {
        return false;
    }
    return fallback;
}

//****************************************************************************
//	Method:		MetadataXml
//	FullName:	MetadataXml
//	author:
//	date:		2026/11/07
//	Function:   将 McFenceInfo 序列化为 McFence 自有的 XML（version 2）。
//
//	Returns:	std::wstring    生成的 XML 文本。
//	Parameter:	IN  const McFenceInfo& info    要序列化的围栏信息
//
//              映射文件夹围栏的内容来自目录本身，不写 <Files>
//
//	changelog:  2026/11/07 自有格式：增加 version / Folder / Sort / SortDescending
//****************************************************************************
std::wstring MetadataXml(const McFenceInfo& info)
{
    std::wstring out;
    out += L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    out += L"<McFence version=\"2\">\r\n";
    out += L"  <Id>" + EscapeXml(info.id) + L"</Id>\r\n";
    out += L"  <Name>" + EscapeXml(info.name) + L"</Name>\r\n";
    out += L"  <PosX>" + McFormat(L"%d", info.pos_x) + L"</PosX>\r\n";
    out += L"  <PosY>" + McFormat(L"%d", info.pos_y) + L"</PosY>\r\n";
    out += L"  <Width>" + McFormat(L"%d", info.width) + L"</Width>\r\n";
    out += L"  <Height>" + McFormat(L"%d", info.height) + L"</Height>\r\n";
    out += std::wstring(L"  <Locked>") + (info.locked ? L"true" : L"false") + L"</Locked>\r\n";
    out += std::wstring(L"  <CanMinify>") + (info.can_minify ? L"true" : L"false") +
           L"</CanMinify>\r\n";
    out += L"  <TitleHeight>" + McFormat(L"%d", info.title_height) + L"</TitleHeight>\r\n";
    out += L"  <Folder>" + EscapeXml(info.folder) + L"</Folder>\r\n";
    out += L"  <Sort>" + McFormat(L"%d", info.sort) + L"</Sort>\r\n";
    out += std::wstring(L"  <SortDescending>") + (info.sort_descending ? L"true" : L"false") +
           L"</SortDescending>\r\n";
    if (info.folder.empty() && !info.files.empty())
    {
        out += L"  <Files>\r\n";
        for (size_t i = 0; i < info.files.size(); ++i)
        {
            out += L"    <string>" + EscapeXml(info.files[i]) + L"</string>\r\n";
        }
        out += L"  </Files>\r\n";
    }
    else
    {
        out += L"  <Files />\r\n";
    }
    out += L"</McFence>\r\n";
    return out;
}

//****************************************************************************
//	Method:		ParseMetadata
//	FullName:	ParseMetadata
//	author:
//	date:		2026/11/06
//	Function:   从元数据 XML 文本解析出 McFenceInfo 的各字段，包括文件列表。
//
//	Returns:	bool    文本中存在 <FenceInfo> 根节点时返回 true 并填充 out，否则返回 false。
//	Parameter:	IN  const std::wstring& text    围栏元数据 XML 文本
//              OUT McFenceInfo& out    解析结果写入的围栏信息对象
//
//              文本中未出现的字段会保留 out 原有的默认值
//
//	changelog:  New
//****************************************************************************
bool ParseMetadata(const std::wstring& text, McFenceInfo& out)
{
    // <McFence> 是自有格式；<FenceInfo> 是早期 SimpleGrid 兼容格式，读入后下次保存即升级。
    if (text.find(L"<McFence") == std::wstring::npos && text.find(L"<FenceInfo") == std::wstring::npos)
    {
        return false;
    }
    ElementValue(text, L"Id", out.id);
    ElementValue(text, L"Name", out.name);
    out.pos_x = ElementInt(text, L"PosX", out.pos_x);
    out.pos_y = ElementInt(text, L"PosY", out.pos_y);
    out.width = ElementInt(text, L"Width", out.width);
    out.height = ElementInt(text, L"Height", out.height);
    out.locked = ElementBool(text, L"Locked", out.locked);
    out.can_minify = ElementBool(text, L"CanMinify", out.can_minify);
    out.title_height = ElementInt(text, L"TitleHeight", out.title_height);
    ElementValue(text, L"Folder", out.folder);
    out.folder = trim(out.folder);
    out.sort = ElementInt(text, L"Sort", out.sort);
    if (out.sort < MCFENCE_SORT_NONE || out.sort > MCFENCE_SORT_TYPE)
    {
        out.sort = MCFENCE_SORT_NONE;
    }
    out.sort_descending = ElementBool(text, L"SortDescending", out.sort_descending);

    out.files.clear();
    size_t files_start = text.find(L"<Files>");
    if (files_start != std::wstring::npos)
    {
        size_t files_end = text.find(L"</Files>", files_start);
        if (files_end != std::wstring::npos)
        {
            size_t pos = files_start;
            for (;;)
            {
                size_t item_start = text.find(L"<string>", pos);
                if (item_start == std::wstring::npos || item_start > files_end)
                {
                    break;
                }
                size_t value_start = item_start + 8;
                size_t item_end = text.find(L"</string>", value_start);
                if (item_end == std::wstring::npos || item_end > files_end)
                {
                    break;
                }
                out.files.push_back(UnescapeXml(text.substr(value_start, item_end - value_start)));
                pos = item_end + 9;
            }
        }
    }
    return true;
}

bool HasMetadata(const std::wstring& directory)
{
    return McFileExists(McJoinPath(directory, MCFENCE_META_FILE));
}

//****************************************************************************
//	Method:		WriteFenceIndex
//	FullName:	WriteFenceIndex
//	author:
//	date:		2026/11/06
//	Function:   对目录名去重后写入 fences.index 索引文件（先写临时文件，再原子替换）。
//
//	Returns:	void
//	Parameter:	IN  const std::vector<std::wstring>& directories    围栏目录路径列表
//
//              写入或替换失败时仅记录日志，不会抛出错误
//
//	changelog:  New
//****************************************************************************
void WriteFenceIndex(const std::vector<std::wstring>& directories)
{
    std::vector<std::wstring> names;
    for (size_t i = 0; i < directories.size(); ++i)
    {
        std::wstring name = McFileName(directories[i]);
        bool duplicate = false;
        for (size_t j = 0; j < names.size(); ++j)
        {
            if (McIEquals(names[j], name))
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
        {
            names.push_back(name);
        }
    }
    std::wstring text;
    for (size_t i = 0; i < names.size(); ++i)
    {
        text += names[i] + L"\r\n";
    }
    const std::wstring indexPath = McJoinPath(McDataRoot(), MCFENCE_INDEX_FILE);
    const std::wstring tempPath = indexPath + L".tmp";
    if (!WriteTextFile(tempPath, text))
    {
        McLogFormat(L"无法写入围栏索引 '%s'", tempPath.c_str());
        return;
    }
    if (!ReplaceFileAtomic(tempPath, indexPath))
    {
        McLogLastError(L"replace fences.index");
    }
}

//****************************************************************************
//	Method:		IsManagedItem
//	FullName:	IsManagedItem
//	author:
//	date:		2026/11/06
//	Function:   判断给定的完整路径是否位于 Items 仓库根目录之下。
//
//	Returns:	bool    路径在 Items 仓库下返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& full    要判断的完整路径
//
//	changelog:  New
//****************************************************************************
bool IsManagedItem(const std::wstring& full)
{
    std::wstring root = McFullPath(McItemsRoot());
    if (root.empty())
    {
        return false;
    }
    if (root[root.size() - 1] != L'\\')
    {
        root += L'\\';
    }
    return McIStartsWith(full, root);
}

//****************************************************************************
//	Method:		StripGuidSeparators
//	FullName:	StripGuidSeparators
//	author:
//	date:		2026/11/06
//	Function:   去掉 GUID 字符串中的连字符。
//
//	Returns:	std::wstring    去掉 '-' 字符后的字符串。
//	Parameter:	IN  const std::wstring& guid    GUID 字符串
//
//	changelog:  New
//****************************************************************************
std::wstring StripGuidSeparators(const std::wstring& guid)
{
    std::wstring out;
    for (size_t i = 0; i < guid.size(); ++i)
    {
        if (guid[i] != L'-')
        {
            out += guid[i];
        }
    }
    return out;
}

}  // namespace

std::wstring McStoreFenceDirectory(const std::wstring& id)
{
    return McJoinPath(McDataRoot(), id);
}

//****************************************************************************
//	Method:		McStoreReadFence
//	FullName:	McStoreReadFence
//	author:
//	date:		2026/11/06
//	Function:   读取并解析指定目录下的 __fence_metadata.xml，对缺失的 Id 和非法的 TitleHeight 进行修正。
//
//	Returns:	bool    成功读取并解析返回 true，否则返回 false。
//	Parameter:	IN  const std::wstring& directory    围栏目录路径
//              OUT McFenceInfo& out    解析得到的围栏信息
//
//              Id 为空时用目录名回填；TitleHeight 超出 [16,100] 范围时重置为 35
//
//	changelog:  New
//****************************************************************************
bool McStoreReadFence(const std::wstring& directory, McFenceInfo& out)
{
    const std::wstring path = McJoinPath(directory, MCFENCE_META_FILE);
    std::wstring text;
    if (!ReadTextFile(path, text))
    {
        return false;
    }
    McFenceInfo parsed;
    if (!ParseMetadata(text, parsed))
    {
        return false;
    }
    if (parsed.id.empty())
    {
        parsed.id = McFileName(directory);
    }
    if (parsed.title_height < 16 || parsed.title_height > 100)
    {
        parsed.title_height = 35;
    }
    out = parsed;
    return true;
}

//****************************************************************************
//	Method:		McStoreWriteFence
//	FullName:	McStoreWriteFence
//	author:
//	date:		2026/11/06
//	Function:   创建围栏目录（如不存在），将元数据写入临时文件后原子替换，并确保索引文件中包含该目录。
//
//	Returns:	bool    全部步骤均成功返回 true，否则返回 false。
//	Parameter:	IN  const McFenceInfo& info    要写入的围栏信息
//
//	changelog:  New
//****************************************************************************
bool McStoreWriteFence(const McFenceInfo& info)
{
    const std::wstring directory = McStoreFenceDirectory(info.id);
    if (!McEnsureDirectory(directory))
    {
        McLogFormat(L"无法创建围栏目录 '%s'", directory.c_str());
        return false;
    }
    const std::wstring path = McJoinPath(directory, MCFENCE_META_FILE);
    const std::wstring temp_path = path + L".tmp";
    if (!WriteTextFile(temp_path, MetadataXml(info)))
    {
        McLogFormat(L"无法写入围栏数据 '%s'", temp_path.c_str());
        return false;
    }
    if (!ReplaceFileAtomic(temp_path, path))
    {
        McLogLastError(L"replace __fence_metadata.xml");
        return false;
    }
    McStoreEnsureIndex(directory);
    return true;
}

//****************************************************************************
//	Method:		McStoreFenceDirectories
//	FullName:	McStoreFenceDirectories
//	author:
//	date:		2026/11/06
//	Function:   列出所有围栏目录：优先读取 fences.index；
//              索引缺失时退化为扫描数据根目录下含元数据的子目录。
//
//	Returns:	std::vector<std::wstring>    去重后的围栏目录完整路径列表。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
std::vector<std::wstring> McStoreFenceDirectories()
{
    std::vector<std::wstring> result;
    const std::wstring base = McDataRoot();
    const std::wstring indexPath = McJoinPath(base, MCFENCE_INDEX_FILE);
    if (McFileExists(indexPath))
    {
        std::wstring text;
        if (ReadTextFile(indexPath, text))
        {
            size_t pos = 0;
            while (pos <= text.size())
            {
                size_t next = text.find(L'\n', pos);
                std::wstring line = trim(text.substr(pos, next == std::wstring::npos
                                                                ? std::wstring::npos
                                                                : next - pos));
                if (!line.empty() && McIsGuidString(line))
                {
                    std::wstring directory = McJoinPath(base, line);
                    if (McDirectoryExists(directory))
                    {
                        bool duplicate = false;
                        for (size_t i = 0; i < result.size(); ++i)
                        {
                            if (McIEquals(result[i], directory))
                            {
                                duplicate = true;
                                break;
                            }
                        }
                        if (!duplicate)
                        {
                            result.push_back(directory);
                        }
                    }
                }
                if (next == std::wstring::npos)
                {
                    break;
                }
                pos = next + 1;
            }
        }
        return result;
    }
    // No index (pre-index installs): scan for directories holding metadata.
    std::vector<std::wstring> directories = McListDirectories(base);
    for (size_t i = 0; i < directories.size(); ++i)
    {
        if (HasMetadata(directories[i]))
        {
            result.push_back(directories[i]);
        }
    }
    return result;
}

//****************************************************************************
//	Method:		McStoreEnsureIndex
//	FullName:	McStoreEnsureIndex
//	author:
//	date:		2026/11/06
//	Function:   若索引中尚未包含指定目录，则将其加入并重写 fences.index。
//
//	Returns:	void
//	Parameter:	IN  const std::wstring& directory    需要确保存在于索引中的围栏目录
//
//	changelog:  New
//****************************************************************************
void McStoreEnsureIndex(const std::wstring& directory)
{
    std::vector<std::wstring> directories = McStoreFenceDirectories();
    for (size_t i = 0; i < directories.size(); ++i)
    {
        if (McIEquals(directories[i], directory))
        {
            return;
        }
    }
    directories.push_back(directory);
    WriteFenceIndex(directories);
}

//****************************************************************************
//	Method:		McStoreRemoveFence
//	FullName:	McStoreRemoveFence
//	author:
//	date:		2026/11/06
//	Function:   删除指定围栏的磁盘目录，并从索引文件中移除对应条目。
//
//	Returns:	bool    目录删除成功返回 true，否则返回 false（索引仍会被重写）。
//	Parameter:	IN  const McFenceInfo& info    要删除的围栏信息
//
//	changelog:  New
//****************************************************************************
bool McStoreRemoveFence(const McFenceInfo& info)
{
    const std::wstring directory = McStoreFenceDirectory(info.id);
    bool removed = McRemoveDirectory(directory);
    if (!removed)
    {
        McLogFormat(L"无法删除围栏目录 '%s'", directory.c_str());
    }
    std::vector<std::wstring> directories = McStoreFenceDirectories();
    std::vector<std::wstring> kept;
    for (size_t i = 0; i < directories.size(); ++i)
    {
        if (!McIEquals(directories[i], directory))
        {
            kept.push_back(directories[i]);
        }
    }
    WriteFenceIndex(kept);
    return removed;
}

//****************************************************************************
//	Method:		McStoreLoadAll
//	FullName:	McStoreLoadAll
//	author:
//	date:		2026/11/06
//	Function:   加载所有围栏目录的元数据，解析失败的目录记录到 errors 中并跳过，不中断其余加载。
//
//	Returns:	int    成功加载的围栏数量。
//	Parameter:	OUT std::vector<McFenceInfo>& out    成功解析的围栏信息列表
//              OUT std::vector<std::wstring>& errors    解析失败的目录路径列表
//
//	changelog:  New
//****************************************************************************
int McStoreLoadAll(std::vector<McFenceInfo>& out, std::vector<std::wstring>& errors)
{
    out.clear();
    errors.clear();
    std::vector<std::wstring> directories = McStoreFenceDirectories();
    for (size_t i = 0; i < directories.size(); ++i)
    {
        McFenceInfo info;
        if (McStoreReadFence(directories[i], info))
        {
            out.push_back(info);
        }
        else
        {
            errors.push_back(directories[i]);
            McLogFormat(L"无法载入围栏数据 '%s'", directories[i].c_str());
        }
    }
    return static_cast<int>(out.size());
}

//****************************************************************************
//	Method:		McStoreCreateFence
//	FullName:	McStoreCreateFence
//	author:
//	date:		2026/11/06
//	Function:   创建一个新的围栏信息（分配 GUID、默认位置与大小）并立即写入磁盘。
//
//	Returns:	McFenceInfo    新建并已写入磁盘的围栏信息。
//	Parameter:	IN  const std::wstring& name    新围栏的名称
//              IN  const std::wstring& folder    映射的文件夹，空串表示普通围栏
//
//	changelog:  New
//****************************************************************************
McFenceInfo McStoreCreateFence(const std::wstring& name, const std::wstring& folder)
{
    McFenceInfo info;
    info.id = McNewGuid();
    info.name = name;
    info.folder = folder;
    if (!folder.empty())
    {
        info.sort = MCFENCE_SORT_NAME;  // 映射文件夹默认按名称自然排序
    }
    info.pos_x = 100;
    info.pos_y = 250;
    info.width = 300;
    info.height = 300;
    McStoreWriteFence(info);
    return info;
}

//****************************************************************************
//	Method:		McStoreImportItem
//	FullName:	McStoreImportItem
//	author:
//	date:		2026/11/06
//	Function:   将外部文件或文件夹移动（或复制）到 Items 仓库下新建的 GUID 子目录中；
//              若源已在仓库内，则直接返回其原路径。
//
//	Returns:	std::wstring    存储后的路径；来源无效或移动 / 复制失败时返回空字符串。
//	Parameter:	IN  const std::wstring& source    要导入的源文件或文件夹路径
//              IN  bool move    true 移动（拖放的默认动作），false 复制
//
//              通过 ImportLock 临界区串行化并发导入，避免 GUID 目录冲突
//              失败时会清理已创建的目标目录
//
//	changelog:  2026/11/07 拖入默认移动
//****************************************************************************
std::wstring McStoreImportItem(const std::wstring& source, bool move)
{
    if (trim(source).empty())
    {
        return std::wstring();
    }
    std::wstring full = McFullPath(source);
    if (full.empty())
    {
        return std::wstring();
    }
    if (IsManagedItem(full))
    {
        return McPathExists(full) ? full : std::wstring();
    }
    if (!McPathExists(full))
    {
        return std::wstring();
    }

    EnterCriticalSection(&ImportLock());
    std::wstring container = McJoinPath(McItemsRoot(), StripGuidSeparators(McNewGuid()));
    std::wstring trimmed = full;
    while (!trimmed.empty() && (trimmed[trimmed.size() - 1] == L'\\' || trimmed[trimmed.size() - 1] == L'/'))
    {
        trimmed.erase(trimmed.size() - 1);
    }
    std::wstring destination = McJoinPath(container, McFileName(trimmed));
    bool ok = false;
    if (McEnsureDirectory(container))
    {
        if (move)
        {
            // SHFileOperation 跨卷移动文件夹也能处理（MoveFileEx 不行），且可撤销；目标容器是新建的，
            // 不会有重名冲突。
            std::vector<wchar_t> from(full.begin(), full.end());
            from.push_back(L'\0');
            from.push_back(L'\0');
            std::vector<wchar_t> to(destination.begin(), destination.end());
            to.push_back(L'\0');
            to.push_back(L'\0');
            SHFILEOPSTRUCTW op;
            ZeroMemory(&op, sizeof(op));
            op.wFunc = FO_MOVE;
            op.pFrom = &from[0];
            op.pTo = &to[0];
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR;
            ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted && McPathExists(destination) &&
                 !McPathExists(full);
        }
        else
        {
            ok = McFileExists(full) ? McCopyFile(full, destination, false)
                                   : McCopyDirectory(full, destination);
        }
    }
    if (!ok)
    {
        McLogFormat(L"无法将项目归档到 Items '%s'", full.c_str());
        McRemoveDirectory(container);
    }
    LeaveCriticalSection(&ImportLock());
    return ok ? destination : std::wstring();
}

//****************************************************************************
//	Method:		McStoreNewItemContainer
//	FullName:	McStoreNewItemContainer
//	author:
//	date:		2026/11/07
//	Function:   在 Items 仓库下新建一个空的 <guid32> 子目录，供 Shell「新建」菜单在其中创建文件。
//
//	Returns:	std::wstring    新目录路径；创建失败返回空串。
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
std::wstring McStoreNewItemContainer()
{
    const std::wstring container = McJoinPath(McItemsRoot(), StripGuidSeparators(McNewGuid()));
    return McEnsureDirectory(container) ? container : std::wstring();
}

namespace
{

struct McSortKey
{
    std::wstring path;
    std::wstring name;
    std::wstring extension;
    bool folder;
    ULONGLONG size;
    ULONGLONG time;
};

// 排序只发生在 UI 线程上，比较函数用的模式放在这里即可。
int g_sort_mode = MCFENCE_SORT_NAME;

typedef int(WINAPI* McPfnStrCmpLogicalW)(LPCWSTR, LPCWSTR);

//****************************************************************************
//	Method:		CompareLogical
//	FullName:	CompareLogical
//	author:
//	date:		2026/11/07
//	Function:   资源管理器式的自然排序（"文件2" 在 "文件10" 之前）：shlwapi!StrCmpLogicalW。
//              XP 自带的 shlwapi 已导出它，但 _WIN32_IE=0x0501 的头文件不声明，所以运行时解析；
//              解析不到时退回 lstrcmpiW。
//
//	Returns:	int    <0 / 0 / >0
//	Parameter:	IN  const std::wstring& a    左值
//              IN  const std::wstring& b    右值
//
//	changelog:  New
//****************************************************************************
int CompareLogical(const std::wstring& a, const std::wstring& b)
{
    static McPfnStrCmpLogicalW fn = NULL;
    static bool probed = false;
    if (!probed)
    {
        probed = true;
        fn = reinterpret_cast<McPfnStrCmpLogicalW>(
            McResolveExport(GetModuleHandleW(L"shlwapi.dll"), "StrCmpLogicalW"));
    }
    return fn ? fn(a.c_str(), b.c_str()) : lstrcmpiW(a.c_str(), b.c_str());
}

//****************************************************************************
//	Method:		SortLess
//	FullName:	SortLess
//	author:
//	date:		2026/11/07
//	Function:   排序比较：文件夹总在前；再按当前模式比较，相等时退回名称自然排序。
//
//	Returns:	bool    a 应排在 b 之前返回 true
//	Parameter:	IN  const McSortKey& a    左值
//              IN  const McSortKey& b    右值
//
//	changelog:  New
//****************************************************************************
bool SortLess(const McSortKey& a, const McSortKey& b)
{
    if (a.folder != b.folder)
    {
        return a.folder;
    }
    int order = 0;
    switch (g_sort_mode)
    {
        case MCFENCE_SORT_DATE:
            order = a.time < b.time ? -1 : (a.time > b.time ? 1 : 0);
            break;
        case MCFENCE_SORT_SIZE:
            order = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
            break;
        case MCFENCE_SORT_TYPE:
            order = CompareLogical(a.extension, b.extension);
            break;
        default:
            break;
    }
    if (order == 0)
    {
        order = CompareLogical(a.name, b.name);
    }
    return order < 0;
}

McSortKey MakeSortKey(const std::wstring& path, const WIN32_FILE_ATTRIBUTE_DATA* known)
{
    McSortKey key;
    key.path = path;
    key.name = McFileName(path);
    WIN32_FILE_ATTRIBUTE_DATA data;
    ZeroMemory(&data, sizeof(data));
    if (known)
    {
        data = *known;
    }
    else
    {
        GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data);
    }
    key.folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    key.extension = key.folder ? std::wstring() : McToLower(McFileExtension(key.name));
    key.size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    key.time = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) |
               data.ftLastWriteTime.dwLowDateTime;
    return key;
}

void SortKeys(std::vector<McSortKey>& keys, int mode, bool descending)
{
    g_sort_mode = mode;
    std::stable_sort(keys.begin(), keys.end(), SortLess);
    if (descending)
    {
        // 文件夹仍在前：两段各自反转。
        size_t split = 0;
        while (split < keys.size() && keys[split].folder)
        {
            ++split;
        }
        std::reverse(keys.begin(), keys.begin() + split);
        std::reverse(keys.begin() + split, keys.end());
    }
}

}  // namespace

//****************************************************************************
//	Method:		McSortPaths
//	FullName:	McSortPaths
//	author:
//	date:		2026/11/07
//	Function:   按名称（自然排序）/ 修改时间 / 大小 / 类型对路径列表排序；文件夹总在前。
//
//	Returns:	void
//	Parameter:	IN OUTstd::vector<std::wstring>& paths    待排序的路径
//              IN  int mode    McSortMode；MCFENCE_SORT_NONE 时不动
//              IN  bool descending    降序
//
//	changelog:  New
//****************************************************************************
void McSortPaths(std::vector<std::wstring>& paths, int mode, bool descending)
{
    if (mode == MCFENCE_SORT_NONE || paths.size() < 2)
    {
        return;
    }
    std::vector<McSortKey> keys;
    keys.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i)
    {
        keys.push_back(MakeSortKey(paths[i], NULL));
    }
    SortKeys(keys, mode, descending);
    for (size_t i = 0; i < keys.size(); ++i)
    {
        paths[i] = keys[i].path;
    }
}

//****************************************************************************
//	Method:		McEnumerateFolder
//	FullName:	McEnumerateFolder
//	author:
//	date:		2026/11/07
//	Function:   枚举映射文件夹的直接子项（跳过 . / .. 与隐藏、系统项），并按指定方式排序。
//
//	Returns:	std::vector<std::wstring>    子项完整路径
//	Parameter:	IN  const std::wstring& folder    要枚举的文件夹
//              IN  int mode    McSortMode；NONE 按名称
//              IN  bool descending    降序
//
//	changelog:  New
//****************************************************************************
std::vector<std::wstring> McEnumerateFolder(const std::wstring& folder, int mode, bool descending)
{
    std::vector<McSortKey> keys;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(McJoinPath(folder, L"*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
            {
                continue;
            }
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
            {
                continue;  // desktop.ini、Thumbs.db 之类
            }
            WIN32_FILE_ATTRIBUTE_DATA attributes;
            attributes.dwFileAttributes = data.dwFileAttributes;
            attributes.ftCreationTime = data.ftCreationTime;
            attributes.ftLastAccessTime = data.ftLastAccessTime;
            attributes.ftLastWriteTime = data.ftLastWriteTime;
            attributes.nFileSizeHigh = data.nFileSizeHigh;
            attributes.nFileSizeLow = data.nFileSizeLow;
            keys.push_back(MakeSortKey(McJoinPath(folder, data.cFileName), &attributes));
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    SortKeys(keys, mode == MCFENCE_SORT_NONE ? MCFENCE_SORT_NAME : mode, descending);
    std::vector<std::wstring> result;
    result.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i)
    {
        result.push_back(keys[i].path);
    }
    return result;
}

//****************************************************************************
//	Method:		McStoreIsManagedItem
//	FullName:	McStoreIsManagedItem
//	author:
//	date:		2026/11/07
//	Function:   判断路径是否位于 Items 仓库之内。
//
//	Returns:	bool    在仓库内返回 true
//	Parameter:	IN  const std::wstring& path    要判断的路径
//
//	changelog:  New
//****************************************************************************
bool McStoreIsManagedItem(const std::wstring& path)
{
    const std::wstring full = McFullPath(path);
    return !full.empty() && IsManagedItem(full);
}

//****************************************************************************
//	Method:		McStoreRecycleManaged
//	FullName:	McStoreRecycleManaged
//	author:
//	date:		2026/11/07
//	Function:   把 Items 仓库里的副本删除到回收站（静默，可从回收站恢复），再删除变空的容器目录。
//
//	Returns:	void
//	Parameter:	IN  const std::vector<std::wstring>& paths    要删除的副本路径
//
//              仓库外的路径直接跳过：这里绝不能碰到用户自己的文件
//
//	changelog:  New
//****************************************************************************
void McStoreRecycleManaged(const std::vector<std::wstring>& paths)
{
    std::vector<wchar_t> from;
    std::vector<std::wstring> containers;
    for (size_t i = 0; i < paths.size(); ++i)
    {
        const std::wstring full = McFullPath(paths[i]);
        if (full.empty() || !IsManagedItem(full) || !McPathExists(full))
        {
            continue;
        }
        from.insert(from.end(), full.begin(), full.end());
        from.push_back(L'\0');
        containers.push_back(McParentDirectory(full));
    }
    if (from.empty())
    {
        return;
    }
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op;
    ZeroMemory(&op, sizeof(op));
    op.wFunc = FO_DELETE;
    op.pFrom = &from[0];
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const int result = SHFileOperationW(&op);
    if (result != 0)
    {
        McLogFormat(L"回收仓库副本失败 0x%X", result);
    }
    for (size_t i = 0; i < containers.size(); ++i)
    {
        // 只删空目录；容器根本身（Items）不会出现在这里。
        if (!McIEquals(containers[i], McFullPath(McItemsRoot())))
        {
            RemoveDirectoryW(containers[i].c_str());
        }
    }
}
