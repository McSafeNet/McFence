// McSettings.h - 全局选项（<数据目录>\settings.ini）与开机自启动（HKCU\...\Run）。
//
// 选项用 Get/WritePrivateProfile* 读写：Windows XP 就有，不需要自己写解析器，
// 用户也可以直接用记事本修改。
#pragma once

#include "framework.h"

#include <string>


struct McSettings
{
    bool double_click_desktop;   // 双击桌面空白处显示/隐藏全部围栏
    bool keep_on_show_desktop;   // Win+D 显示桌面时保留围栏（挂到 SHELLDLL_DefView 名下）
    bool snap;                   // 拖动/缩放时吸附屏幕边缘与其他围栏
    UINT hotkey_modifiers;       // MOD_* 组合；0 表示不注册全局热键
    UINT hotkey_key;             // 虚拟键码
    int language;                // McLanguage；settings.ini 里没有时按系统界面语言取默认值

    McSettings()
        : double_click_desktop(true), keep_on_show_desktop(false), snap(true),
          hotkey_modifiers(MOD_CONTROL | MOD_ALT), hotkey_key('F'), language(1)
    {
    }
};

// 界面语言（openFrence 方式：每处文本同时给出英文与中文，按当前语言挑一个）。
enum McLanguage
{
    MCFENCE_LANG_EN = 0,
    MCFENCE_LANG_ZH = 1
};

// 进程内唯一的选项实例；首次调用时从 settings.ini 载入。
McSettings& McSettingsGet();
void McSettingsSave();

// "Ctrl+Alt+F" 这样的显示文本；未设置热键时返回空串。
std::wstring McHotkeyText(UINT modifiers, UINT key);

// 按当前界面语言返回 |en| 或 |zh|；两个参数都应是字符串字面量（返回值不复制）。
const wchar_t* McLoc(const wchar_t* en, const wchar_t* zh);

bool McAutostartEnabled();
bool McAutostartSet(bool enable);
