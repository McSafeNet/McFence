// McUtil.h - 全局共用的字符串 / 路径 / 时间工具。
#pragma once

#include "framework.h"

#include <string>
#include <vector>


std::wstring McExeDirectory();
std::wstring McLocalAppData();                 // %LOCALAPPDATA% (CSIDL_LOCAL_APPDATA)
std::wstring McDataRoot();                     // <local app data>\SimpleGrid
std::wstring McItemsRoot();                    // <data root>\Items

bool McEnsureDirectory(const std::wstring& dir);
bool McRemoveDirectory(const std::wstring& dir);
bool McFileExists(const std::wstring& path);
bool McDirectoryExists(const std::wstring& path);
bool McPathExists(const std::wstring& path);

std::vector<std::wstring> McListDirectories(const std::wstring& dir);
std::vector<std::wstring> McListFiles(const std::wstring& dir);

std::wstring McJoinPath(const std::wstring& a, const std::wstring& b);
std::wstring McParentDirectory(const std::wstring& path);
std::wstring McFileName(const std::wstring& path);       // "notes.txt"
std::wstring McFileExtension(const std::wstring& path);  // ".txt" (as stored, not lowercased)
std::wstring McFullPath(const std::wstring& path);

bool McIEquals(const std::wstring& a, const std::wstring& b);
bool McIEndsWith(const std::wstring& text, const std::wstring& suffix);
bool McIStartsWith(const std::wstring& text, const std::wstring& prefix);
std::wstring McToLower(std::wstring text);
std::wstring trim(const std::wstring& text);
std::wstring McReplaceAll(std::wstring text, const std::wstring& from, const std::wstring& to);

std::wstring McFormat(const wchar_t* fmt, ...);
std::wstring McNewGuid();               // lower-case "D" form
bool McIsGuidString(const std::wstring& text);

std::wstring McNowStamp();                     // 2026-09-10 14:03:11.123
ULONGLONG McTicks();

bool McCopyFile(const std::wstring& src, const std::wstring& dst, bool overwrite);
bool McCopyDirectory(const std::wstring& src, const std::wstring& dst);
bool McWriteFileBytes(const std::wstring& path, const std::string& bytes);
bool McReadFileBytes(const std::wstring& path, std::string& bytes);

std::string McToUtf8(const std::wstring& text);
std::wstring McFromUtf8(const std::string& text);
void McAppendUtf8Bom(std::string& bytes);

