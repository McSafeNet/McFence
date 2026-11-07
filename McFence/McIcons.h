// McIcons.h - Shell 图标与缩略图缓存。
//
// 先返回廉价占位图，真实位图由
// 后台线程构建（最多两个并发），完成后让围栏窗口重绘。
#pragma once

#include "McModel.h"

#include <algorithm>
#if defined(NOMINMAX)
using std::max;
using std::min;
#endif
#include <gdiplus.h>

#include <string>
#include <utility>
#include <vector>


class McIconCache
{
public:
    static McIconCache& Instance();

    // Window that receives MCFENCE_WM_ICON_READY when a bitmap finishes loading.
    void SetNotifyWindow(HWND hwnd)
    {
        m_notify = hwnd;
    }

    // Icon for a fence item scaled into a |pixel_size| square. Never returns NULL.
    Gdiplus::Bitmap* GetIcon(const McEntry& item, int pixel_size);

    bool IsImage(const std::wstring& path) const;

    // Drops every cached bitmap (icon size changed). UI thread only, outside painting:
    // nothing may still hold a pointer returned by GetIcon.
    void Clear();

    // Releases every cached bitmap; call after the windows are gone.
    void Shutdown();

private:
    McIconCache();

    struct McIconJob;

    static DWORD WINAPI ThreadProc(void* parameter);

    Gdiplus::Bitmap* Lookup(const std::wstring& key);
    Gdiplus::Bitmap* StorePlaceholder(const std::wstring& key, int pixel_size);
    void QueueBuild(const std::wstring& key, const McEntry& item, int pixel_size, bool thumbnail);
    void BuildSync(const std::wstring& key, const McEntry& item, int pixel_size, bool thumbnail);

    HWND m_notify;
    CRITICAL_SECTION m_lock;
    volatile LONG m_pending;   // jobs queued or running
    volatile bool m_stopping;  // set by Shutdown: no new jobs, late results dropped
    std::vector<std::pair<std::wstring, Gdiplus::Bitmap*> > m_cache;
    std::vector<Gdiplus::Bitmap*> m_owned;
};

// Helpers shared with the fence view.
Gdiplus::Bitmap* McScaleToSquare(Gdiplus::Image* source, int size);
Gdiplus::Bitmap* McIconToBitmap(HICON icon);
HICON McLoadFolderIcon();
HICON McLoadApplicationIcon();

