// McShortcut.h - 启动围栏项目、解析 .lnk。
//
// 这里直接用 IShellLink +
// IPersistFile：环节更少、更快，而且 Windows XP 就支持。
#pragma once

#include "framework.h"

#include <string>


bool McResolveShortcut(const std::wstring& lnk_path, std::wstring& target, std::wstring& arguments,
                      std::wstring& working_directory);

// FenceEntry.Open(): .lnk pointing at an existing .exe starts the target directly
// (no ShellExecute, so Explorer cannot hang); everything else goes to the shell.
bool McOpenPath(const std::wstring& path);

// McOpenPath on a pool thread (with COM initialised), so the fence never blocks.
void McOpenPathAsync(const std::wstring& path);

// "Open in Explorer" - select the item instead of just opening the folder.
bool McLocateInExplorer(const std::wstring& path);

// McLocateInExplorer on a pool thread (with COM initialised).
void McLocateInExplorerAsync(const std::wstring& path);

