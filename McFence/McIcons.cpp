#include "McIcons.h"
#include "McLog.h"
#include "McUtil.h"

#include <commctrl.h>  // IMAGELISTDRAWPARAMS / IMAGEINFO used by commoncontrols.h
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <vector>


namespace
{

const GUID IID_IImageList_SG = {
    0x46EB5926, 0x582E, 0x4017, {0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50}};

enum
{
    MCFENCE_SHIL_LARGE = 0,
    MCFENCE_SHIL_EXTRA_LARGE = 2,
    MCFENCE_SHIL_JUMBO = 4,
    MCFENCE_ILD_TRANSPARENT = 0x1,
    MCFENCE_ILD_PRESERVE_ALPHA = 0x40,
    MCFENCE_SHGFI_SYS_ICON_INDEX = 0x4000,
    MCFENCE_SHGFI_LARGE_ICON = 0x0,
    MCFENCE_SHGFI_ICON = 0x100,
    MCFENCE_SIID_FOLDER = 3,
    MCFENCE_SHGSI_ICON = 0x100,
    MCFENCE_SHGSI_LARGE_ICON = 0x0
};

const wchar_t* const s_ImageExtensions[] = {L".bmp", L".gif", L".jpg", L".jpeg",
                                             L".png", L".tiff", L".tif"};

//****************************************************************************
//	Method:		CopyFileIcon
//	FullName:	CopyFileIcon
//	author:
//	date:		2026/11/06
//	Function:   通过 SHGetFileInfoW 取得指定路径（按给定属性）对应的大图标句柄。
//
//	Returns:	HICON    图标句柄，失败为 NULL，调用方负责 DestroyIcon
//	Parameter:	IN  LPCWSTR path    文件或目录路径
//              IN  DWORD attributes    文件属性，配合 SHGFI_USEFILEATTRIBUTES 等场景使用
//
//	changelog:  New
//****************************************************************************
HICON CopyFileIcon(LPCWSTR path, DWORD attributes)
{
    SHFILEINFOW info;
    ZeroMemory(&info, sizeof(info));
    if (SHGetFileInfoW(path, attributes, &info, sizeof(info), MCFENCE_SHGFI_ICON | MCFENCE_SHGFI_LARGE_ICON) == 0)
    {
        return NULL;
    }
    return info.hIcon;
}

//****************************************************************************
//	Method:		IconFromImageList
//	FullName:	IconFromImageList
//	author:
//	date:		2026/11/06
//	Function:   从系统图像列表（如 Jumbo/Extra Large/Large）按系统图标索引取出指定路径对应的高分辨率图标。
//
//	Returns:	HICON    图标句柄，不支持该列表（如 XP 上的高分辨率列表）或获取失败时返回 NULL
//	Parameter:	IN  const std::wstring& path    文件或目录路径
//              IN  DWORD attributes    文件属性
//              IN  int shil    要使用的系统图像列表类型（SHIL_*）
//
//              依赖 IImageList 的 vtable 方法顺序假设（GetIcon 为 IUnknown 之后第 8 个方法），
//              顺序假设有误会误调用其他方法
//
//	changelog:  New
//****************************************************************************
HICON IconFromImageList(const std::wstring& path, DWORD attributes, int shil)
{
    McPfnSHGetImageList get_list = McShellImageList();
    if (!get_list)
    {
        return NULL;
    }
    if ((shil == MCFENCE_SHIL_JUMBO || shil == MCFENCE_SHIL_EXTRA_LARGE) && !McOsAtLeast(6, 0))
    {
        return NULL;  // XP only ships the large list
    }
    // IImageList from <commoncontrols.h>: GetIcon is the 8th method after
    // IUnknown (Add, ReplaceIcon, SetOverlayImage, Replace, AddMasked, Draw,
    // Remove, GetIcon). A hand-written vtable in another order calls AddMasked
    // instead and never yields a real icon.
    IImageList* list = NULL;
    if (FAILED(get_list(shil, &IID_IImageList_SG, reinterpret_cast<void**>(&list))) || !list)
    {
        return NULL;
    }
    HICON result = NULL;
    SHFILEINFOW info;
    ZeroMemory(&info, sizeof(info));
    if (SHGetFileInfoW(path.c_str(), attributes, &info, sizeof(info), MCFENCE_SHGFI_SYS_ICON_INDEX) != 0 &&
        info.iIcon >= 0)
    {
        if (FAILED(list->GetIcon(info.iIcon, MCFENCE_ILD_TRANSPARENT | MCFENCE_ILD_PRESERVE_ALPHA,
                                 &result)))
        {
            result = NULL;
        }
    }
    list->Release();
    return result;
}

//****************************************************************************
//	Method:		IsShellLink
//	FullName:	IsShellLink
//	author:
//	date:		2026/11/07
//	Function:   判断路径是否为快捷方式（.lnk / .url 等带 SFGAO_LINK 属性的条目）。
//
//	Returns:	bool    是快捷方式时返回 true
//	Parameter:	IN  const std::wstring& path    文件路径
//
//	changelog:  New
//****************************************************************************
bool IsShellLink(const std::wstring& path)
{
    SHFILEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.dwAttributes = SFGAO_LINK;
    return SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ATTRIBUTES | SHGFI_ATTR_SPECIFIED) != 0 &&
           (info.dwAttributes & SFGAO_LINK) != 0;
}

//****************************************************************************
//	Method:		LinkOverlayIcon
//	FullName:	LinkOverlayIcon
//	author:
//	date:		2026/11/07
//	Function:   取系统大图标列表（SHIL_LARGE）里的快捷方式箭头叠加图。桌面按这张图的原始尺寸画箭头，
//              不随图标大小缩放；如果从 48 / 256 列表带叠加取图再缩放，箭头会跟着图标一起变大。
//
//	Returns:	HICON    箭头图标，失败为 NULL，调用方负责 DestroyIcon
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
HICON LinkOverlayIcon()
{
    McPfnSHGetImageList get_list = McShellImageList();
    if (!get_list)
    {
        return NULL;
    }
    IImageList* list = NULL;
    if (FAILED(get_list(MCFENCE_SHIL_LARGE, &IID_IImageList_SG, reinterpret_cast<void**>(&list))) || !list)
    {
        return NULL;
    }
    HICON result = NULL;
    // 图像列表的叠加图编号就是 SHGetIconOverlayIndex 的返回值（实测 +1 会取到别的叠加图）。
    const int overlay = SHGetIconOverlayIndexW(NULL, IDO_SHGIOI_LINK);
    int image = -1;
    if (overlay > 0 && SUCCEEDED(list->GetOverlayImage(overlay, &image)) && image >= 0)
    {
        if (FAILED(list->GetIcon(image, MCFENCE_ILD_TRANSPARENT | MCFENCE_ILD_PRESERVE_ALPHA, &result)))
        {
            result = NULL;
        }
    }
    list->Release();
    return result;
}

//****************************************************************************
//	Method:		BitmapFromAlphaIcon
//	FullName:	BitmapFromAlphaIcon
//	author:
//	date:		2026/11/06
//	Function:   将带 Alpha 通道的 32bpp 图标颜色位图直接拷贝为 ARGB 的 Gdiplus::Bitmap，
//              避免 Bitmap::FromHICON 把透明像素画成黑色；
//              若图标非 32bpp 或没有任何非零 Alpha（旧式掩码图标）
//              则返回 NULL，交调用方回退处理。
//
//	Returns:	Gdiplus::Bitmap*    新建的位图，调用方负责 delete；不满足条件时为 NULL
//	Parameter:	IN  HICON icon    源图标句柄，函数内部不会销毁它
//
//	changelog:  New
//****************************************************************************
// 32bpp icons carry their transparency in the colour bitmap's alpha channel;
// Bitmap::FromHICON ignores it and paints those pixels black. Copy the bits
// straight into an ARGB bitmap instead. NULL when the icon has no alpha (old
// mask-only icons), so the caller can fall back to FromHICON.
Gdiplus::Bitmap* BitmapFromAlphaIcon(HICON icon)
{
    ICONINFO icon_info;
    ZeroMemory(&icon_info, sizeof(icon_info));
    if (!GetIconInfo(icon, &icon_info))
    {
        return NULL;
    }
    Gdiplus::Bitmap* result = NULL;
    BITMAP bm;
    ZeroMemory(&bm, sizeof(bm));
    if (icon_info.hbmColor && GetObjectW(icon_info.hbmColor, sizeof(bm), &bm) &&
        bm.bmBitsPixel == 32 && bm.bmWidth > 0 && bm.bmHeight > 0)
    {
        const int width = bm.bmWidth;
        const int height = bm.bmHeight;
        BITMAPINFO bmi;
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        std::vector<DWORD> pixels(static_cast<size_t>(width) * height);
        HDC dc = GetDC(NULL);
        const int rows = GetDIBits(dc, icon_info.hbmColor, 0, height, &pixels[0], &bmi, DIB_RGB_COLORS);
        ReleaseDC(NULL, dc);
        bool has_alpha = false;
        for (size_t i = 0; rows == height && i < pixels.size(); ++i)
        {
            if ((pixels[i] & 0xFF000000) != 0)
            {
                has_alpha = true;
                break;
            }
        }
        if (has_alpha)
        {
            result = new Gdiplus::Bitmap(width, height, PixelFormat32bppARGB);
            Gdiplus::Rect rect(0, 0, width, height);
            Gdiplus::BitmapData data;
            if (result->GetLastStatus() == Gdiplus::Ok &&
                result->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) ==
                    Gdiplus::Ok)
            {
                for (int y = 0; y < height; ++y)
                {
                    memcpy(static_cast<BYTE*>(data.Scan0) + y * data.Stride, &pixels[y * width],
                           width * sizeof(DWORD));
                }
                result->UnlockBits(&data);
            }
            else
            {
                delete result;
                result = NULL;
            }
        }
    }
    if (icon_info.hbmColor)
    {
        DeleteObject(icon_info.hbmColor);
    }
    if (icon_info.hbmMask)
    {
        DeleteObject(icon_info.hbmMask);
    }
    return result;
}

//****************************************************************************
//	Method:		CropPadded
//	FullName:	CropPadded
//	author:
//	date:		2026/11/07
//	Function:   Jumbo 列表里没有高清图的类型，会把小图标放在 256x256 画布中间留出大片空白；
//              缩放到格子里就显得比桌面上小。不透明区域明显小于画布（两边都不到 60%）时裁到不透明边界。
//
//	Returns:	Gdiplus::Bitmap*    裁剪后的新位图；不需要裁剪或失败时返回 NULL
//	Parameter:	IN  Gdiplus::Bitmap* source    源位图（32bpp ARGB）
//
//              与 openFrence 的 TryLoadIcon(cropPadded) 相同：只对 Jumbo 来源这样做，
//              Large / ExtraLarge 的留白是外壳本身的构图，保持原样才和桌面一致
//
//	changelog:  New
//****************************************************************************
Gdiplus::Bitmap* CropPadded(Gdiplus::Bitmap* source)
{
    const int width = static_cast<int>(source->GetWidth());
    const int height = static_cast<int>(source->GetHeight());
    if (width <= 0 || height <= 0)
    {
        return NULL;
    }
    Gdiplus::Rect rect(0, 0, width, height);
    Gdiplus::BitmapData data;
    if (source->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
    {
        return NULL;
    }
    int left = width;
    int top = height;
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < height; ++y)
    {
        const DWORD* row = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(data.Scan0) + y * data.Stride);
        for (int x = 0; x < width; ++x)
        {
            if ((row[x] >> 24) > 8)
            {
                left = min(left, x);
                right = max(right, x);
                top = min(top, y);
                bottom = max(bottom, y);
            }
        }
    }
    source->UnlockBits(&data);
    if (right < 0)
    {
        return NULL;
    }
    const int crop_width = right - left + 1;
    const int crop_height = bottom - top + 1;
    // openFrence 在不到 90% 时就裁；但桌面本身是整张画布缩放的，文件夹这类本来就留了一圈边的
    // 高清图被裁掉会比桌面上大一号。只裁真正的“小图放大画布”（两边都不到 60%）。
    if (crop_width * 10 >= width * 6 || crop_height * 10 >= height * 6)
    {
        return NULL;
    }
    Gdiplus::Bitmap* cropped = source->Clone(left, top, crop_width, crop_height, PixelFormat32bppARGB);
    if (cropped && cropped->GetLastStatus() != Gdiplus::Ok)
    {
        delete cropped;
        cropped = NULL;
    }
    return cropped;
}

std::wstring CacheKey(const std::wstring& path, int pixel_size)
{
    return McToLower(path) + L"|" + McFormat(L"%d", pixel_size);
}

//****************************************************************************
//	Method:		WorkSemaphore
//	FullName:	WorkSemaphore
//	author:
//	date:		2026/11/06
//	Function:   返回进程内唯一、容量为 2 的信号量（首次调用时创建），
//              用于限制同时构建图标的后台任务数量。
//
//	Returns:	HANDLE    信号量句柄
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
HANDLE WorkSemaphore()
{
    static HANDLE semaphore = NULL;
    if (!semaphore)
    {
        semaphore = CreateSemaphoreW(NULL, 2, 2, NULL);  // SemaphoreSlim(2)
    }
    return semaphore;
}

}  // namespace

//****************************************************************************
//	Method:		McIconToBitmap
//	FullName:	McIconToBitmap
//	author:
//	date:		2026/11/06
//	Function:   把 HICON 转换成 Gdiplus::Bitmap：优先用 BitmapFromAlphaIcon 保留 Alpha 通道，
//              失败则退化为 Bitmap::FromHICON；转换后销毁传入的图标。
//
//	Returns:	Gdiplus::Bitmap*    新建的位图，失败返回 NULL
//	Parameter:	IN  HICON icon    源图标，函数内部会调用 DestroyIcon 释放它
//
//              调用方不应再对传入的 icon 调用 DestroyIcon
//
//	changelog:  New
//****************************************************************************
Gdiplus::Bitmap* McIconToBitmap(HICON icon)
{
    if (!icon)
    {
        return NULL;
    }
    Gdiplus::Bitmap* bitmap = BitmapFromAlphaIcon(icon);
    if (!bitmap)
    {
        bitmap = Gdiplus::Bitmap::FromHICON(icon);
    }
    DestroyIcon(icon);
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok)
    {
        delete bitmap;
        return NULL;
    }
    return bitmap;
}

//****************************************************************************
//	Method:		McScaleToSquare
//	FullName:	McScaleToSquare
//	author:
//	date:		2026/11/06
//	Function:   把任意尺寸的图像按比例缩放后居中绘制到一张 size×size、背景透明的新位图上。
//
//	Returns:	Gdiplus::Bitmap*    新建的正方形位图；source 为空或 size<=0 时返回 NULL
//	Parameter:	IN  Gdiplus::Image* source    源图像
//              IN  int size    目标正方形边长（像素）
//
//	changelog:  New
//****************************************************************************
Gdiplus::Bitmap* McScaleToSquare(Gdiplus::Image* source, int size)
{
    if (!source || size <= 0)
    {
        return NULL;
    }
    Gdiplus::Bitmap* target = new Gdiplus::Bitmap(size, size, PixelFormat32bppPARGB);
    if (!target || target->GetLastStatus() != Gdiplus::Ok)
    {
        delete target;
        return NULL;
    }
    Gdiplus::Graphics graphics(target);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

    const int source_width = static_cast<int>(source->GetWidth());
    const int source_height = static_cast<int>(source->GetHeight());
    if (source_width <= 0 || source_height <= 0)
    {
        return target;
    }
    const float scale = min(static_cast<float>(size) / source_width,
                            static_cast<float>(size) / source_height);
    const int width = max(1, static_cast<int>(source_width * scale + 0.5f));
    const int height = max(1, static_cast<int>(source_height * scale + 0.5f));
    graphics.DrawImage(source, (size - width) / 2, (size - height) / 2, width, height);
    return target;
}

//****************************************************************************
//	Method:		McLoadFolderIcon
//	FullName:	McLoadFolderIcon
//	author:
//	date:		2026/11/06
//	Function:   获取系统文件夹图标：优先用 SHGetStockIconInfo（SIID_FOLDER）取大图标，
//              不支持时（如 XP）退化为复制系统目录所用的文件夹图标。
//
//	Returns:	HICON    文件夹图标句柄，失败为 NULL
//	Parameter:	无
//
//	changelog:  New
//****************************************************************************
HICON McLoadFolderIcon()
{
    McPfnSHGetStockIconInfo stock = McShellStockIcon();
    if (stock)
    {
        McStockIconInfo info;
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);
        if (SUCCEEDED(stock(MCFENCE_SIID_FOLDER, MCFENCE_SHGSI_ICON | MCFENCE_SHGSI_LARGE_ICON, &info)) && info.hIcon)
        {
            return info.hIcon;
        }
    }
    // XP: reuse the icon Explorer shows for the system folder.
    wchar_t system_dir[MAX_PATH];
    if (GetSystemDirectoryW(system_dir, MAX_PATH) == 0)
    {
        return NULL;
    }
    return CopyFileIcon(system_dir, FILE_ATTRIBUTE_DIRECTORY);
}

HICON McLoadApplicationIcon()
{
    return LoadIconW(NULL, MAKEINTRESOURCEW(32512));
}

McIconCache& McIconCache::Instance()
{
    static McIconCache cache;
    return cache;
}

//****************************************************************************
//	Method:		McIconCache
//	FullName:	McIconCache::McIconCache
//	author:
//	date:		2026/11/06
//	Function:   构造函数：初始化临界区和成员字段；
//              在 UI 线程上提前完成一次性探测（系统版本、Shell 图像列表入口、Stock 图标入口、工作信号量）
//              以及系统图像列表的预热调用，避免多个后台图标线程首次并发调用时发生竞态导致图标缺失。
//
//	Returns:	无
//	Parameter:	无
//
//              这些探测/预热调用必须在任何图标工作线程创建之前、于 UI 线程完成
//
//	changelog:  New
//****************************************************************************
McIconCache::McIconCache() : m_notify(NULL), m_pending(0), m_stopping(false)
{
    InitializeCriticalSection(&m_lock);
    // The lazy probes below set "probed" before publishing their result and are
    // not thread-safe; two icon workers racing on the first call could see a
    // NULL entry point and fail that icon. Resolve them here, on the UI thread,
    // before any worker exists.
    McOsVersion();
    McShellImageList();
    McShellStockIcon();
    WorkSemaphore();
    // The system image list is built lazily by the first SHGetFileInfo call;
    // two icon workers hitting that first call together intermittently get no
    // icon at all. Build it here, once, on the UI thread.
    SHFILEINFOW info;
    ZeroMemory(&info, sizeof(info));
    SHGetFileInfoW(L"warmup.txt", FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                   MCFENCE_SHGFI_SYS_ICON_INDEX | SHGFI_USEFILEATTRIBUTES);
    ZeroMemory(&info, sizeof(info));
    SHGetFileInfoW(L"warmup", FILE_ATTRIBUTE_DIRECTORY, &info, sizeof(info),
                   MCFENCE_SHGFI_SYS_ICON_INDEX | SHGFI_USEFILEATTRIBUTES);
}

struct McIconCache::McIconJob
{
    std::wstring key;
    McEntry item;
    int pixel_size;
    bool thumbnail;
    McIconCache* cache;
};

//****************************************************************************
//	Method:		IsImage
//	FullName:	McIconCache::IsImage
//	author:
//	date:		2026/11/06
//	Function:   根据扩展名判断路径是否属于已知图片格式（bmp/gif/jpg/jpeg/png/tiff/tif），
//              用于决定是否走缩略图加载路径。
//
//	Returns:	bool    是已知图片格式返回 true
//	Parameter:	IN  const std::wstring& path    文件路径
//
//	changelog:  New
//****************************************************************************
bool McIconCache::IsImage(const std::wstring& path) const
{
    const std::wstring extension = McToLower(McFileExtension(path));
    if (extension.empty())
    {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_ImageExtensions) / sizeof(s_ImageExtensions[0]); ++i)
    {
        if (extension == s_ImageExtensions[i])
        {
            return true;
        }
    }
    return false;
}

//****************************************************************************
//	Method:		Lookup
//	FullName:	McIconCache::Lookup
//	author:
//	date:		2026/11/06
//	Function:   在缓存数组中按 key 线性查找已有的位图条目。
//
//	Returns:	Gdiplus::Bitmap*    命中返回对应位图指针，未命中返回 NULL
//	Parameter:	IN  const std::wstring& key    缓存键（路径+尺寸）
//
//              调用方需已持有 m_lock
//
//	changelog:  New
//****************************************************************************
Gdiplus::Bitmap* McIconCache::Lookup(const std::wstring& key)
{
    for (size_t i = 0; i < m_cache.size(); ++i)
    {
        if (m_cache[i].first == key)
        {
            return m_cache[i].second;
        }
    }
    return NULL;
}

//****************************************************************************
//	Method:		StorePlaceholder
//	FullName:	McIconCache::StorePlaceholder
//	author:
//	date:		2026/11/06
//	Function:   为缓存条目创建一个占位图（用应用图标缩放而成，
//              失败则生成一张透明位图）并加入缓存表及所有权列表。
//
//	Returns:	Gdiplus::Bitmap*    新建的占位位图指针
//	Parameter:	IN  const std::wstring& key    缓存键
//              IN  int pixel_size    目标像素尺寸
//
//              调用方需已持有 m_lock
//
//	changelog:  New
//****************************************************************************
Gdiplus::Bitmap* McIconCache::StorePlaceholder(const std::wstring& key, int pixel_size)
{
    HICON icon = McLoadApplicationIcon();  // shared icon: never DestroyIcon it
    Gdiplus::Bitmap* source = icon ? BitmapFromAlphaIcon(icon) : NULL;
    if (!source && icon)
    {
        source = Gdiplus::Bitmap::FromHICON(icon);
    }
    Gdiplus::Bitmap* scaled = source ? McScaleToSquare(source, max(8, pixel_size)) : NULL;
    delete source;
    if (!scaled)
    {
        scaled = new Gdiplus::Bitmap(max(8, pixel_size), max(8, pixel_size), PixelFormat32bppPARGB);
    }
    m_cache.push_back(std::make_pair(key, scaled));
    m_owned.push_back(scaled);
    return scaled;
}

//****************************************************************************
//	Method:		GetIcon
//	FullName:	McIconCache::GetIcon
//	author:
//	date:		2026/11/06
//	Function:   获取指定条目在给定像素大小下的图标位图：命中缓存直接返回；
//              未命中则先存入占位图立即返回，同时把真实图标的构建任务投递到后台线程，
//              完成后异步替换缓存内容并通知重绘。
//
//	Returns:	Gdiplus::Bitmap*    当前可用的位图（占位图或已构建完成的真实图标）
//	Parameter:	IN  const McEntry& item    要取图标的文件/文件夹条目
//              IN  int pixel_size    目标像素尺寸，小于 8 会被夹到 8
//
//	changelog:  New
//****************************************************************************
Gdiplus::Bitmap* McIconCache::GetIcon(const McEntry& item, int pixel_size)
{
    if (pixel_size < 8)
    {
        pixel_size = 8;
    }
    const std::wstring key = CacheKey(item.path, pixel_size);
    bool need_queue = false;
    Gdiplus::Bitmap* result = NULL;

    EnterCriticalSection(&m_lock);
    result = Lookup(key);
    if (!result)
    {
        result = StorePlaceholder(key, pixel_size);
        need_queue = true;
    }
    LeaveCriticalSection(&m_lock);

    if (need_queue)
    {
        QueueBuild(key, item, pixel_size, IsImage(item.path));
    }
    return result;
}

//****************************************************************************
//	Method:		ThreadProc
//	FullName:	McIconCache::ThreadProc
//	author:
//	date:		2026/11/06
//	Function:   后台图标构建线程入口：获取工作信号量名额，初始化 COM 后调用 BuildSync 构建图标，
//              完成后释放信号量、递减挂起计数并删除任务上下文；若缓存已处于关闭状态则跳过构建。
//
//	Returns:	DWORD    线程返回码，固定为 0
//	Parameter:	IN  void* parameter    指向 McIconJob 任务上下文，函数内部负责 delete
//
//	changelog:  New
//****************************************************************************
DWORD WINAPI McIconCache::ThreadProc(void* parameter)
{
    McIconJob* context = static_cast<McIconJob*>(parameter);
    McIconCache* cache = context->cache;
    WaitForSingleObject(WorkSemaphore(), INFINITE);
    // Shutdown may have run while this job was queued: GDI+ is going away, so
    // do not touch it at all.
    if (!cache->m_stopping)
    {
        // SHGetFileInfo / the system image lists need COM on the calling thread; a
        // pool thread has none, so some shell icon handlers would silently fail.
        const HRESULT com = CoInitialize(NULL);
        cache->BuildSync(context->key, context->item, context->pixel_size, context->thumbnail);
        if (SUCCEEDED(com))
        {
            CoUninitialize();
        }
    }
    ReleaseSemaphore(WorkSemaphore(), 1, NULL);
    delete context;
    InterlockedDecrement(&cache->m_pending);
    return 0;
}

//****************************************************************************
//	Method:		QueueBuild
//	FullName:	McIconCache::QueueBuild
//	author:
//	date:		2026/11/06
//	Function:   把一个图标构建任务投递到系统线程池（QueueUserWorkItem）；
//              若缓存正在关闭则直接放弃，投递失败则退化为在当前线程同步构建。
//
//	Returns:	void
//	Parameter:	IN  const std::wstring& key    缓存键
//              IN  const McEntry& item    要构建图标的条目
//              IN  int pixel_size    目标像素尺寸
//              IN  bool thumbnail    是否走图片缩略图加载路径
//
//	changelog:  New
//****************************************************************************
void McIconCache::QueueBuild(const std::wstring& key, const McEntry& item, 
    int pixel_size, bool thumbnail)
{
    if (m_stopping)
    {
        return;
    }
    McIconJob* context = new McIconJob;
    context->key = key;
    context->item = item;
    context->pixel_size = pixel_size;
    context->thumbnail = thumbnail;
    context->cache = this;
    InterlockedIncrement(&m_pending);
    // QueueUserWorkItem exists on Windows 2000+, unlike the thread pool APIs the
    // C# build used.
    if (!QueueUserWorkItem(&McIconCache::ThreadProc, context, WT_EXECUTEDEFAULT))
    {
        InterlockedDecrement(&m_pending);
        delete context;
        BuildSync(key, item, pixel_size, thumbnail);
    }
}

//****************************************************************************
//	Method:		BuildSync
//	FullName:	McIconCache::BuildSync
//	author:
//	date:		2026/11/06
//	Function:   同步构建指定条目的图标位图：图片文件读取字节生成 256x256 缩略图再缩放；
//              否则依次尝试 Jumbo/Extra Large/Large 系统图像列表图标、关联图标、
//              文件夹图标、按扩展名的最后兜底图标，取到后转换并缩放到目标尺寸；
//              成功后在缓存表中用真实位图替换占位图并通知主线程重绘，
//              若缓存条目已消失或正在关闭则丢弃结果。
//
//	Returns:	void
//	Parameter:	IN  const std::wstring& key    缓存键
//              IN  const McEntry& item    要构建图标的条目
//              IN  int pixel_size    目标像素尺寸
//              IN  bool thumbnail    是否走图片缩略图加载路径
//
//              图标获取失败只记录日志，不抛异常；真实图标发布后占位图仍保留在 m_owned 中，
//              统一在 Shutdown 时释放
//
//	changelog:  New
//****************************************************************************
void McIconCache::BuildSync(const std::wstring& key, const McEntry& item, int pixel_size,
                            bool thumbnail)
{
    Gdiplus::Bitmap* built = NULL;

    if (thumbnail)
    {
        std::string bytes;
        if (McReadFileBytes(item.path, bytes) && !bytes.empty())
        {
            IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()),
                                                static_cast<UINT>(bytes.size()));
            if (stream)
            {
                Gdiplus::Image* Image = Gdiplus::Image::FromStream(stream);
                if (Image && Image->GetLastStatus() == Gdiplus::Ok)
                {
                    Gdiplus::Image* thumb = Image->GetThumbnailImage(256, 256, NULL, NULL);
                    if (thumb && thumb->GetLastStatus() == Gdiplus::Ok)
                    {
                        built = McScaleToSquare(thumb, pixel_size);
                    }
                    delete thumb;
                }
                delete Image;
                stream->Release();
            }
        }
    }

    if (!built)
    {
        const DWORD attributes = item.type == MCFENCE_ENTRY_FOLDER ? FILE_ATTRIBUTE_DIRECTORY : 0;
        // openFrence 的选法：取画布刚好盖住目标尺寸的最小列表（32 / 48 / 256），缩放最少、
        // 构图和桌面一致；拿不到再往大的退，最后往小的退，再不行是关联图标。
        const int lists[3] = {MCFENCE_SHIL_LARGE, MCFENCE_SHIL_EXTRA_LARGE, MCFENCE_SHIL_JUMBO};
        const int canvas[3] = {32, 48, 256};
        int first = 2;
        for (int i = 0; i < 3; ++i)
        {
            if (canvas[i] >= pixel_size)
            {
                first = i;
                break;
            }
        }
        HICON icon = NULL;
        bool from_jumbo = false;
        for (int i = first; !icon && i < 3; ++i)
        {
            icon = IconFromImageList(item.path, attributes, lists[i]);
            from_jumbo = icon && lists[i] == MCFENCE_SHIL_JUMBO;
        }
        for (int i = first - 1; !icon && i >= 0; --i)
        {
            icon = IconFromImageList(item.path, attributes, lists[i]);
        }
        if (!icon)
        {
            icon = CopyFileIcon(item.path.c_str(), attributes);
        }
        if (!icon && item.type == MCFENCE_ENTRY_FOLDER)
        {
            icon = McLoadFolderIcon();
        }
        if (!icon)
        {
            // Last resort: the icon registered for the extension, without
            // touching the file (SHGFI_USEFILEATTRIBUTES).
            SHFILEINFOW info;
            ZeroMemory(&info, sizeof(info));
            if (SHGetFileInfoW(item.path.c_str(),
                               attributes ? attributes : FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                               MCFENCE_SHGFI_ICON | MCFENCE_SHGFI_LARGE_ICON | SHGFI_USEFILEATTRIBUTES) != 0)
            {
                icon = info.hIcon;
            }
        }
        Gdiplus::Bitmap* source = McIconToBitmap(icon);
        if (source && from_jumbo)
        {
            Gdiplus::Bitmap* cropped = CropPadded(source);
            if (cropped)
            {
                delete source;
                source = cropped;
            }
        }
        if (source)
        {
            built = McScaleToSquare(source, pixel_size);
            delete source;
        }
        if (built && item.type != MCFENCE_ENTRY_FOLDER && IsShellLink(item.path))
        {
            // 箭头叠加图的画布和大图标一样大，箭头本身在画布左下角；按原始尺寸把画布的左下角
            // 对齐图标左下角，和桌面一样。图标比画布还小时才整体缩到图标大小。
            Gdiplus::Bitmap* arrow = McIconToBitmap(LinkOverlayIcon());
            if (arrow)
            {
                const int arrow_size = min(pixel_size, static_cast<int>(arrow->GetHeight()));
                const int arrow_width = static_cast<int>(arrow->GetWidth()) * arrow_size /
                                        max(1, static_cast<int>(arrow->GetHeight()));
                Gdiplus::Graphics graphics(built);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                graphics.DrawImage(arrow, 0, pixel_size - arrow_size, arrow_width, arrow_size);
                delete arrow;
            }
        }
    }

    if (!built)
    {
        McLogFormat(L"图标加载失败 '%s'", item.path.c_str());
        return;
    }

    HWND notify = NULL;
    bool published = false;
    EnterCriticalSection(&m_lock);
    // After Shutdown the cache is gone; a late result is simply dropped.
    for (size_t i = 0; !m_stopping && i < m_cache.size(); ++i)
    {
        if (m_cache[i].first == key)
        {
            // The placeholder stays allocated (and owned) so no window can draw a
            // freed bitmap; it is released in Shutdown().
            m_cache[i].second = built;
            m_owned.push_back(built);
            published = true;
            break;
        }
    }
    notify = m_notify;
    LeaveCriticalSection(&m_lock);

    if (!published)
    {
        delete built;  // the cache entry disappeared meanwhile
        return;
    }
    if (notify)
    {
        PostMessageW(notify, MCFENCE_WM_ICON_READY, 0, 0);
    }
}

//****************************************************************************
//	Method:		Shutdown
//	FullName:	McIconCache::Shutdown
//	author:
//	date:		2026/11/06
//	Function:   关闭图标缓存：置位停止标志拒绝新任务，最多等待 2 秒让正在运行的后台工作线程结束，
//              然后释放全部缓存和占位位图并清空缓存表。
//
//	Returns:	void
//	Parameter:	无
//
//              若 2 秒内仍有线程未结束，只记录日志并继续退出，避免阻塞随后的 GdiplusShutdown
//
//	changelog:  New
//****************************************************************************
void McIconCache::Clear()
{
    // Workers still building an old size find their key gone and drop the result.
    EnterCriticalSection(&m_lock);
    for (size_t i = 0; i < m_owned.size(); ++i)
    {
        delete m_owned[i];
    }
    m_owned.clear();
    m_cache.clear();
    LeaveCriticalSection(&m_lock);
}

void McIconCache::Shutdown()
{
    // Stop new jobs, then give running ones a moment to finish: GdiplusShutdown
    // follows right after this and must not race a worker still drawing.
    m_stopping = true;
    const ULONGLONG deadline = McTicks() + 2000;
    while (InterlockedCompareExchange(&m_pending, 0, 0) > 0 && McTicks() < deadline)
    {
        Sleep(10);
    }
    if (InterlockedCompareExchange(&m_pending, 0, 0) > 0)
    {
        McLog(L"图标线程未在 2 秒内结束，跳过等待");
    }
    EnterCriticalSection(&m_lock);
    for (size_t i = 0; i < m_owned.size(); ++i)
    {
        delete m_owned[i];
    }
    m_owned.clear();
    m_cache.clear();
    LeaveCriticalSection(&m_lock);
}

