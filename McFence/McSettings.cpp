#include "McSettings.h"
#include "McLog.h"
#include "McUtil.h"


namespace
{

const wchar_t* const k_section = L"McFence";
const wchar_t* const k_run_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::wstring SettingsPath()
{
    return McJoinPath(McDataRoot(), MCFENCE_SETTINGS_FILE);
}

bool ReadBool(const std::wstring& path, const wchar_t* key, bool fallback)
{
    return GetPrivateProfileIntW(k_section, key, fallback ? 1 : 0, path.c_str()) != 0;
}

void WriteInt(const std::wstring& path, const wchar_t* key, int value)
{
    const std::wstring text = McFormat(L"%d", value);
    WritePrivateProfileStringW(k_section, key, text.c_str(), path.c_str());
}

//****************************************************************************
//	Method:		LoadSettings
//	FullName:	LoadSettings
//	author:
//	date:		2026/11/07
//	Function:   从 settings.ini 读取全局选项；文件或键缺失时保留默认值。
//
//	Returns:	void
//	Parameter:	OUT McSettings& settings    读取结果
//
//	changelog:  New
//****************************************************************************
void LoadSettings(McSettings& settings)
{
    const std::wstring path = SettingsPath();
    settings.double_click_desktop = ReadBool(path, L"DoubleClickDesktop", settings.double_click_desktop);
    settings.keep_on_show_desktop = ReadBool(path, L"KeepOnShowDesktop", settings.keep_on_show_desktop);
    settings.snap = ReadBool(path, L"Snap", settings.snap);
    settings.hotkey_modifiers = GetPrivateProfileIntW(k_section, L"HotkeyModifiers",
                                                      settings.hotkey_modifiers, path.c_str());
    settings.hotkey_key = GetPrivateProfileIntW(k_section, L"HotkeyKey", settings.hotkey_key,
                                                path.c_str());
    settings.hotkey_modifiers &= (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
    settings.hotkey_key &= 0xFF;
    // 第一次运行（还没写过 Language）跟随系统界面语言：中文系统用中文，其余用英文。
    const int system_language = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE
                                    ? MCFENCE_LANG_ZH
                                    : MCFENCE_LANG_EN;
    settings.language = GetPrivateProfileIntW(k_section, L"Language", system_language, path.c_str()) ==
                                MCFENCE_LANG_EN
                            ? MCFENCE_LANG_EN
                            : MCFENCE_LANG_ZH;
}

}  // namespace

McSettings& McSettingsGet()
{
    static McSettings settings;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        LoadSettings(settings);
    }
    return settings;
}

//****************************************************************************
//	Method:		McSettingsSave
//	FullName:	McSettingsSave
//	author:
//	date:		2026/11/07
//	Function:   把当前全局选项写回 settings.ini。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McSettingsSave()
{
    const McSettings& settings = McSettingsGet();
    const std::wstring path = SettingsPath();
    McEnsureDirectory(McDataRoot());
    WriteInt(path, L"DoubleClickDesktop", settings.double_click_desktop ? 1 : 0);
    WriteInt(path, L"KeepOnShowDesktop", settings.keep_on_show_desktop ? 1 : 0);
    WriteInt(path, L"Snap", settings.snap ? 1 : 0);
    WriteInt(path, L"HotkeyModifiers", static_cast<int>(settings.hotkey_modifiers));
    WriteInt(path, L"HotkeyKey", static_cast<int>(settings.hotkey_key));
    WriteInt(path, L"Language", settings.language);
}

const wchar_t* McLoc(const wchar_t* en, const wchar_t* zh)
{
    return McSettingsGet().language == MCFENCE_LANG_ZH ? zh : en;
}

//****************************************************************************
//	Method:		McHotkeyText
//	FullName:	McHotkeyText
//	author:
//	date:		2026/11/07
//	Function:   把修饰键与虚拟键码格式化为 "Ctrl+Alt+F" 形式的显示文本。
//
//	Returns:	std::wstring    显示文本；未设置热键时返回空串
//	Parameter:	IN  UINT modifiers    MOD_* 组合
//              IN  UINT key    虚拟键码
//
//	changelog:  New
//****************************************************************************
std::wstring McHotkeyText(UINT modifiers, UINT key)
{
    if (key == 0)
    {
        return std::wstring();
    }
    std::wstring text;
    if (modifiers & MOD_CONTROL) text += L"Ctrl+";
    if (modifiers & MOD_ALT) text += L"Alt+";
    if (modifiers & MOD_SHIFT) text += L"Shift+";
    if (modifiers & MOD_WIN) text += L"Win+";
    wchar_t name[64] = L"";
    UINT scan = MapVirtualKeyW(key, 0 /* MAPVK_VK_TO_VSC */);
    LONG lparam = static_cast<LONG>(scan << 16);
    // 方向键、Home/End 等属于扩展键，不带这一位会显示成小键盘上的同名键。
    if ((key >= VK_PRIOR && key <= VK_DOWN) || key == VK_INSERT || key == VK_DELETE)
    {
        lparam |= (1 << 24);
    }
    if (GetKeyNameTextW(lparam, name, 64) > 0)
    {
        text += name;
    }
    else
    {
        text += McFormat(L"0x%02X", key);
    }
    return text;
}

//****************************************************************************
//	Method:		McAutostartEnabled
//	FullName:	McAutostartEnabled
//	author:
//	date:		2026/11/07
//	Function:   检查 HKCU\...\Run 下是否有指向本程序的 McFence 值。
//
//	Returns:	bool    已登记开机启动返回 true
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
bool McAutostartEnabled()
{
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, k_run_key, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    {
        return false;
    }
    wchar_t value[MAX_PATH * 2] = L"";
    DWORD bytes = sizeof(value) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG status = RegQueryValueExW(key, MCFENCE_RUN_VALUE, NULL, &type,
                                         reinterpret_cast<BYTE*>(value), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
    {
        return false;
    }
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    // 程序被挪了位置时，旧值指向别处：视为未启用，用户重新勾选即可修正。
    return std::wstring(value).find(exe) != std::wstring::npos;
}

//****************************************************************************
//	Method:		McAutostartSet
//	FullName:	McAutostartSet
//	author:
//	date:		2026/11/07
//	Function:   写入或删除 HKCU\...\Run\McFence；只动当前用户，不需要管理员权限。
//
//	Returns:	bool    操作成功返回 true
//	Parameter:	IN  bool enable    true 写入，false 删除
//
//	changelog:  New
//****************************************************************************
bool McAutostartSet(bool enable)
{
    HKEY key = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, k_run_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) !=
        ERROR_SUCCESS)
    {
        McLogLastError(L"RegCreateKeyExW(Run)");
        return false;
    }
    LONG status;
    if (enable)
    {
        wchar_t exe[MAX_PATH] = L"";
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        const std::wstring command = std::wstring(L"\"") + exe + L"\"";
        status = RegSetValueExW(key, MCFENCE_RUN_VALUE, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        status = RegDeleteValueW(key, MCFENCE_RUN_VALUE);
        if (status == ERROR_FILE_NOT_FOUND)
        {
            status = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS)
    {
        McLogWin32(L"autostart", static_cast<DWORD>(status));
    }
    return status == ERROR_SUCCESS;
}
