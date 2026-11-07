// McLog.h - app_diag.log 写入器。
#pragma once

#include "framework.h"

#include <string>



void McLog(const std::wstring& message);
void McLogFormat(const wchar_t* fmt, ...);
void McLogHr(const wchar_t* what, HRESULT hr);
void McLogLastError(const wchar_t* what);
void McLogWin32(const wchar_t* what, DWORD error);


