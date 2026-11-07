// McSurface.h - 每个围栏的逐像素 alpha 绘制面。
//
// Windows XP 没有 DWM、也没有 DirectComposition，因此从 XP 到 11 唯一都正确的
// 呈现路径，就是分层窗口 + UpdateLayeredWindow。GDI+ 直接画进自顶向下的
// 32bpp 预乘 DIB —— 正是 UpdateLayeredWindow 需要的布局。
#pragma once

#include "framework.h"

#include <algorithm>
#if defined(NOMINMAX)
using std::max;
using std::min;
#endif
#include <gdiplus.h>

#include <string>


class McSurface
{
public:
    McSurface();
    ~McSurface();

    // (Re)allocates the backing bitmap; returns false when the size is unusable.
    bool Resize(int width, int height);
    void Release();

    bool IsValid() const
    {
        return m_bitmap != NULL;
    }
    int width() const
    {
        return m_width;
    }
    int height() const
    {
        return m_height;
    }
    Gdiplus::Graphics& graphics()
    {
        return *m_graphics;
    }

    // Pushes the buffer to the window; the window keeps its position and size.
    bool Present(HWND hwnd);

    // Read-only access for WM_PRINTCLIENT (verification screenshots).
    HDC MemoryDc() const
    {
        return m_memory_dc;
    }
    Gdiplus::Bitmap* Image() const
    {
        return m_bitmap;
    }

private:
    McSurface(const McSurface&);
    McSurface& operator=(const McSurface&);

    HBITMAP m_dib;
    HDC m_memory_dc;
    HGDIOBJ m_previous_bitmap;
    void* m_bits;
    Gdiplus::Bitmap* m_bitmap;
    Gdiplus::Graphics* m_graphics;
    int m_width;
    int m_height;
};

// UI font with the C# build's family chain: Microsoft YaHei UI exists from
// Windows Vista on, so XP falls back to SimSun / Tahoma.
Gdiplus::Font* McCreateFont(float points, int dpi, bool bold = false);

// Ellipsised, vertically centred single line inside |bounds|.
void McDrawTextLine(Gdiplus::Graphics& g, const std::wstring& text, Gdiplus::Font* font,
                    const Gdiplus::RectF& bounds, const Gdiplus::Color& color, bool center_vertical);

// Word-wrapped, horizontally centred label with character ellipsis.
void McDrawTextWrapped(Gdiplus::Graphics& g, const std::wstring& text, Gdiplus::Font* font,
                       const Gdiplus::RectF& bounds, const Gdiplus::Color& color);


Gdiplus::Color McArgb(BYTE alpha, COLORREF rgb);

