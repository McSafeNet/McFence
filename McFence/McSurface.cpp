#include "McSurface.h"
#include "McLog.h"
#include "McUtil.h"


namespace
{

const wchar_t* const s_FontFamilies[] = {
    L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI", L"Tahoma", L"SimSun", L"Arial"};

//****************************************************************************
//	Method:		PickFontFamily
//	FullName:	PickFontFamily
//	author:
//	date:		2026/11/06
//	Function:   从候选字体名称列表中探测系统上第一个可用的字体族，并以静态变量缓存结果；
//              若都不可用则回退为通用无衬线字体族的克隆。
//
//	Returns:	Gdiplus::FontFamily*    已探测到（或回退得到）的字体族指针，为进程内静态缓存对象。
//	Parameter:	无
//
//              返回的指针是静态缓存单例，调用方不得 delete；探测逻辑非线程安全（无同步保护 probed 标志）。
//
//	changelog:  New
//****************************************************************************
Gdiplus::FontFamily* PickFontFamily()
{
    static Gdiplus::FontFamily* cached = NULL;
    static bool probed = false;
    if (!probed)
    {
        probed = true;
        for (size_t i = 0; i < sizeof(s_FontFamilies) / sizeof(s_FontFamilies[0]); ++i)
        {
            Gdiplus::FontFamily* family = new Gdiplus::FontFamily(s_FontFamilies[i]);
            if (family->IsAvailable())
            {
                cached = family;
                break;
            }
            delete family;
        }
        if (!cached)
        {
            cached = Gdiplus::FontFamily::GenericSansSerif()->Clone();
        }
    }
    return cached;
}

}  // namespace

Gdiplus::Color McArgb(BYTE alpha, COLORREF rgb)
{
    return Gdiplus::Color(alpha, GetRValue(rgb), GetGValue(rgb), GetBValue(rgb));
}

McSurface::McSurface()
    : m_dib(NULL), m_memory_dc(NULL), m_previous_bitmap(NULL), m_bits(NULL), m_bitmap(NULL),
      m_graphics(NULL), m_width(0), m_height(0)
{
}

McSurface::~McSurface()
{
    Release();
}

//****************************************************************************
//	Method:		Release
//	FullName:	McSurface::Release
//	author:
//	date:		2026/11/06
//	Function:   释放表面占用的全部 GDI/GDI+ 资源（Graphics、Bitmap、内存 DC、DIB 节），
//              并把所有成员重置为初始状态。
//
//	Returns:	void
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
void McSurface::Release()
{
    delete m_graphics;
    m_graphics = NULL;
    delete m_bitmap;
    m_bitmap = NULL;
    if (m_memory_dc)
    {
        if (m_previous_bitmap)
        {
            SelectObject(m_memory_dc, m_previous_bitmap);
            m_previous_bitmap = NULL;
        }
        DeleteDC(m_memory_dc);
        m_memory_dc = NULL;
    }
    if (m_dib)
    {
        DeleteObject(m_dib);
        m_dib = NULL;
    }
    m_bits = NULL;
    m_width = 0;
    m_height = 0;
}

//****************************************************************************
//	Method:		Resize
//	FullName:	McSurface::Resize
//	author:
//	date:		2026/11/06
//	Function:   按指定宽高重新创建用于呈现的 32 位 DIB 节与 GDI+ Bitmap/Graphics；
//              若尺寸未变则直接复用，创建失败时回滚并返回 false。
//
//	Returns:	bool    成功创建（或已是所需尺寸）返回 true，参数非法或资源创建失败返回 false。
//	Parameter:	IN  int width    目标表面宽度（像素）。
//              IN  int height    目标表面高度（像素）。
//
//              负的 biHeight 构造出顶向下 DIB，以匹配 GDI+ 的行序。
//
//	changelog:  New
//****************************************************************************
bool McSurface::Resize(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }
    if (m_bitmap && m_width == width && m_height == height)
    {
        return true;
    }
    Release();

    BITMAPINFO info;
    ZeroMemory(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    // Negative height gives a top-down DIB, matching GDI+'s row order.
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(NULL);
    m_dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &m_bits, NULL, 0);
    m_memory_dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!m_dib || !m_memory_dc || !m_bits)
    {
        Release();
        return false;
    }
    // Keep the bitmap selected so WM_PRINTCLIENT can blit it at any time.
    m_previous_bitmap = SelectObject(m_memory_dc, m_dib);

    m_bitmap = new Gdiplus::Bitmap(width, height, width * 4, PixelFormat32bppPARGB,
                                  static_cast<BYTE*>(m_bits));
    if (m_bitmap->GetLastStatus() != Gdiplus::Ok)
    {
        Release();
        return false;
    }
    m_graphics = new Gdiplus::Graphics(m_bitmap);
    if (m_graphics->GetLastStatus() != Gdiplus::Ok)
    {
        Release();
        return false;
    }
    m_graphics->SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    m_graphics->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    m_graphics->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    m_graphics->SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    m_graphics->SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    // ClearType cannot be used on a translucent surface; anti-aliased grayscale text is.
    m_graphics->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    m_width = width;
    m_height = height;
    return true;
}

//****************************************************************************
//	Method:		Present
//	FullName:	McSurface::Present
//	author:
//	date:		2026/11/06
//	Function:   调用 UpdateLayeredWindow 把当前表面内容（含每像素 Alpha）贴到指定分层窗口上。
//
//	Returns:	bool    呈现成功返回 true；表面未创建或系统调用失败返回 false。
//	Parameter:	IN  HWND hwnd    要呈现到的分层窗口句柄。
//
//              UpdateLayeredWindow 失败只记录一次日志，避免重复刷屏。
//
//	changelog:  New
//****************************************************************************
bool McSurface::Present(HWND hwnd)
{
    if (!m_dib || !m_memory_dc)
    {
        return false;
    }
    HDC screen = GetDC(NULL);
    SIZE size;
    size.cx = m_width;
    size.cy = m_height;
    POINT source;
    source.x = 0;
    source.y = 0;
    BLENDFUNCTION blend;
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    BOOL ok = UpdateLayeredWindow(hwnd, screen, NULL, &size, m_memory_dc, &source, 0, &blend,
                                  ULW_ALPHA);
    ReleaseDC(NULL, screen);
    if (!ok)
    {
        // Log once: a window without a layered surface would silently Show the
        // previous frame, which is hard to diagnose from a bug report.
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            McLogLastError(L"UpdateLayeredWindow");
        }
    }
    return ok != FALSE;
}

//****************************************************************************
//	Method:		McCreateFont
//	FullName:	McCreateFont
//	author:
//	date:		2026/11/06
//	Function:   按给定磅值和 DPI 计算像素大小，使用探测到的首选字体族创建 GDI+ 字体；
//              创建失败则回退为通用无衬线字体。
//
//	Returns:	Gdiplus::Font*    新创建的字体对象指针。
//	Parameter:	IN  float points    字体大小（磅）。
//              IN  int dpi    目标 DPI，小于等于 0 时按 96 处理。
//              IN  bool bold    是否使用粗体样式。
//
//              返回的 Font 对象由调用方拥有，需要自行 delete。
//
//	changelog:  New
//****************************************************************************
Gdiplus::Font* McCreateFont(float points, int dpi, bool bold)
{
    if (dpi <= 0)
    {
        dpi = 96;
    }
    const float pixels = points * static_cast<float>(dpi) / 72.0f;
    Gdiplus::FontFamily* family = PickFontFamily();
    Gdiplus::Font* font = new Gdiplus::Font(
        family, pixels, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
        Gdiplus::UnitPixel);
    if (font->GetLastStatus() != Gdiplus::Ok)
    {
        delete font;
        font = new Gdiplus::Font(Gdiplus::FontFamily::GenericSansSerif(), pixels,
                                 Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    }
    return font;
}

namespace
{

//****************************************************************************
//	Method:		ConfigureStringFormat
//	FullName:	ConfigureStringFormat
//	author:
//	date:		2026/11/06
//	Function:   为 StringFormat 设置居中对齐、省略号截断，并根据是否换行设置对应的格式标志。
//
//	Returns:	void
//	Parameter:	OUT Gdiplus::StringFormat& format    待配置的字符串格式对象。
//              IN  bool wrapping    true 表示允许换行（行数受限），false 表示单行不换行不裁剪。
//
//	changelog:  New
//****************************************************************************
void ConfigureStringFormat(Gdiplus::StringFormat& format, bool wrapping)
{
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    format.SetFormatFlags(wrapping ? Gdiplus::StringFormatFlagsLineLimit
                                   : (Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip));
}

}  // namespace

//****************************************************************************
//	Method:		McDrawTextLine
//	FullName:	McDrawTextLine
//	author:
//	date:		2026/11/06
//	Function:   在指定矩形区域内绘制单行文本（不换行，超长用省略号截断），可选垂直居中或靠上对齐。
//
//	Returns:	void
//	Parameter:	IN OUTGdiplus::Graphics& g    用于绘制的 GDI+ 绘图对象，函数会修改其文本渲染提示并在其上绘制文本。
//              IN  const std::wstring& text    要绘制的文本内容，空字符串时不绘制。
//              IN  Gdiplus::Font* font    使用的字体，若为 NULL 则不绘制。
//              IN  const Gdiplus::RectF& bounds    文本绘制的边界矩形。
//              IN  const Gdiplus::Color& color    文本颜色。
//              IN  bool center_vertical    是否在边界内垂直居中，否则靠上对齐。
//
//	changelog:  New
//****************************************************************************
void McDrawTextLine(Gdiplus::Graphics& g, const std::wstring& text, Gdiplus::Font* font,
                    const Gdiplus::RectF& bounds, const Gdiplus::Color& color, bool center_vertical)
{
    if (!font || text.empty())
    {
        return;
    }
    Gdiplus::StringFormat format;
    ConfigureStringFormat(format, false);
    format.SetLineAlignment(center_vertical ? Gdiplus::StringAlignmentCenter
                                            : Gdiplus::StringAlignmentNear);
    Gdiplus::SolidBrush brush(color);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), font, bounds, &format, &brush);
}

//****************************************************************************
//	Method:		McDrawTextWrapped
//	FullName:	McDrawTextWrapped
//	author:
//	date:		2026/11/06
//	Function:   在指定矩形区域内绘制可自动换行的多行文本，靠上对齐，超出区域部分按行数截断。
//
//	Returns:	void
//	Parameter:	IN OUTGdiplus::Graphics& g    用于绘制的 GDI+ 绘图对象，函数会在其上绘制文本。
//              IN  const std::wstring& text    要绘制的文本内容，空字符串时不绘制。
//              IN  Gdiplus::Font* font    使用的字体，若为 NULL 则不绘制。
//              IN  const Gdiplus::RectF& bounds    文本绘制的边界矩形。
//              IN  const Gdiplus::Color& color    文本颜色。
//
//	changelog:  New
//****************************************************************************
void McDrawTextWrapped(Gdiplus::Graphics& g, const std::wstring& text, Gdiplus::Font* font,
                       const Gdiplus::RectF& bounds, const Gdiplus::Color& color)
{
    if (!font || text.empty())
    {
        return;
    }
    Gdiplus::StringFormat format;
    ConfigureStringFormat(format, true);
    format.SetLineAlignment(Gdiplus::StringAlignmentNear);
    Gdiplus::SolidBrush brush(color);
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), font, bounds, &format, &brush);
}

