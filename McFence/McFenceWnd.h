// McFenceWnd.h - 一个桌面围栏：窗口、绘制、命中测试与输入。
//
// 无边框分层弹出窗口，始终贴在 Z 序底部；GDI+ 绘制图标网格。两种内容来源：
//   * 普通围栏：m_info.files 是 Items 仓库里的自有副本，顺序由用户拖动或排序决定；
//   * 映射围栏：m_info.folder 指向一个真实文件夹，内容随目录变化自动刷新。
// 交互：单击 / Ctrl 单击 / 框选多选；围栏内与跨围栏拖动用鼠标捕获实现，拖出所有围栏
// 之外时转为系统 OLE 拖拽，可以放到资源管理器、桌面或其他程序里。
#pragma once

#include "McDesktop.h"
#include "McModel.h"
#include "McShellMenu.h"
#include "McShellOps.h"
#include "McSurface.h"

#include <gdiplus.h>

#include <string>
#include <utility>
#include <vector>


class McFenceWnd
{
public:
    // Creates the window, shows it and registers it; NULL on failure.
    static McFenceWnd* Create(const McFenceInfo& info);

    // Repaints every live fence (used when a background icon finishes).
    static void McInvalidateAll();

    static size_t McLiveCount();

    // 桌面图标边长（逻辑像素，见 McDesktopIconSize）。变了就让每个围栏按新尺寸重排、重绘。
    static void McSetIconSize(int logical_size);

    // Asks every fence to close without the "remove this fence?" prompt.
    static void McCloseAll();

    // 显示 / 隐藏全部围栏（托盘、热键、双击桌面）。
    static void McSetAllHidden(bool hidden);
    static bool McAllHidden();
    static bool McIsFenceWindow(HWND hwnd);

    // 应用的隐藏主窗口：围栏被系统意外销毁（资源管理器重启带走了所有者）时请它重建。
    static void McSetAppWindow(HWND hwnd);

    // 按当前选项把每个围栏挂到 / 摘离 SHELLDLL_DefView（Win+D 时保留围栏）。
    static void McApplyDesktopOwner();

    // Verification switches used by tools\verify.ps1.
    static void McRewriteAllFences();
    static bool McDumpAllSurfaces(const std::wstring& path);

    static const UINT k_icon_ready_message = MCFENCE_WM_ICON_READY;
    static const UINT k_restore_message = MCFENCE_WM_RESTORE;

    // Verification helper (MCFENCE_DUMP_SURFACE): writes what the layered
    // window receives, composited over a neutral desktop colour, as a PNG.
    bool McDumpSurface(const std::wstring& path);

    static LRESULT CALLBACK McFenceWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

private:
    struct McRenderedItem
    {
        int model_index;
        McEntry item;
    };

    struct McDropTarget;

    explicit McFenceWnd(const McFenceInfo& info);
    ~McFenceWnd();

    bool CreateFenceWindow();
    static bool PngEncoder(CLSID* out);
    LRESULT handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    void Invalidate();
    void paint();
    void RefreshRendered();
    void ComputeScale();
    void ApplyDpi(int dpi);
    int InitialDpi() const;
    void ReloadFonts();
    int ScreenDpi() const;
    int scaled(int value) const;
    float scaled_f(float value) const;
    int ItemHeight() const;
    int ColumnCount() const;
    RECT ItemBounds(int visual) const;

    void minify();
    void unminify();
    void OnMouseLeft();
    void UpdateBlurRegion();
    void ApplyTitleHeight();
    void ApplyDesktopOwnerTo();
    void EnsureOnScreen();

    void SaveNow();
    void RequestSave(int timer_id);
    void SavePosition();
    void SaveSize();

    // 映射文件夹围栏
    bool IsMapped() const
    {
        return !m_info.folder.empty();
    }
    void ReloadFolder();
    void StartFolderWatch();

    // 选择
    bool IsSelected(const std::wstring& path) const;
    void SelectOnly(const std::wstring& path);
    void ToggleSelected(const std::wstring& path);
    void SelectAll();
    void PruneSelection();
    std::vector<std::wstring> SelectedInOrder() const;
    void UpdateMarquee(POINT client);

    std::wstring HitTestItem(POINT client);
    int DropInsertIndex(POINT client) const;
    int VisualToModelIndex(int visual) const;
    int ModelToVisualIndex(int model) const;
    void SetDropIndex(int index);

    void DrawItem(Gdiplus::Graphics& g, const McRenderedItem& item, int x, int y);
    void DrawDropIndicator(Gdiplus::Graphics& g);
    void DrawMarquee(Gdiplus::Graphics& g);

    void ShowFenceMenu(POINT screen);
    void RunFenceCommand(UINT command, const std::vector<std::wstring>& items);
    void RemoveRecords(const std::vector<std::wstring>& items);
    void RecycleItems(const std::vector<std::wstring>& items);
    void OpenFenceItem(const std::wstring& path);
    void ApplySort(int mode, bool descending);
    void CreateFromNewMenu(UINT command);

    // 原地重命名（openFrence 方式）：不弹窗口，编辑框直接画在围栏表面上，按键由围栏窗口处理。
    // 项目：F2 / 右键「重命名」/ 新建后；栅栏：双击标题栏 / 右键「重命名栅栏」。
    enum McRenameTarget
    {
        MCFENCE_RENAME_NONE = 0,
        MCFENCE_RENAME_ITEM,
        MCFENCE_RENAME_TITLE
    };
    bool IsRenaming() const
    {
        return m_rename_target != MCFENCE_RENAME_NONE;
    }
    void BeginItemRename(const std::wstring& path);
    void BeginTitleRename();
    void StartRenameSession();
    void EndRename(bool commit, bool interactive = true);
    void RenameInsert(const std::wstring& text);
    void RenameChar(wchar_t ch);
    void RenameKeyDown(WPARAM key);
    void RenameCaretFromPoint(POINT client);
    void DrawRenameEditor(Gdiplus::Graphics& g, int client_width);
    void PlaceImeWindow();
    void TakeFocus();
    void ScrollItemIntoView(const std::wstring& path);

    static HCURSOR DragCursor(bool can_drop);
    void StartInternalDrag();
    void UpdateInternalDrag(POINT screen);
    void FinishInternalDrag(POINT screen, bool cancel);
    void StartDragOut();
    McFenceWnd* FenceAtPoint(POINT screen) const;

    // 窗口移动 / 缩放时的吸附与 Ctrl 整体移动
    void BeginMoveSize();
    void SnapMoving(RECT* rect);
    void SnapSizing(RECT* rect, WPARAM edge);
    void SnapTargets(const RECT& rect, RECT& work_area, std::vector<RECT>& others) const;

    static void MoveFenceItems(McFenceWnd* source, McFenceWnd* target,
                               const std::vector<std::wstring>& paths, int insert_index);
    static bool RemovePath(std::vector<std::wstring>& items, const std::wstring& path);
    void AcceptFileDrop(const std::vector<std::wstring>& paths, int insert_index, bool move);
    void SetFenceHeight(int height);

    HWND m_hwnd;
    McFenceInfo m_info;
    McSurface m_surface;
    int m_dpi;
    float m_scale;
    int m_item_width;
    int m_item_padding;
    int m_text_height;
    int m_icon_box;
    int m_title_offset;
    int m_title_height;
    int m_logical_title_height;
    Gdiplus::Font* m_title_font;
    Gdiplus::Font* m_icon_font;
    std::vector<McRenderedItem> m_rendered;
    std::vector<std::wstring> m_selection;
    std::vector<std::wstring> m_marquee_base;   // 框选开始前的选择（Ctrl 框选时保留）
    std::wstring m_hovering_item;
    std::wstring m_drag_candidate;
    std::vector<std::wstring> m_drag_set;
    POINT m_drag_start;
    POINT m_marquee_start;                      // 内容坐标（已加上滚动偏移）
    POINT m_marquee_end;
    bool m_marquee;
    bool m_marquee_toggle;
    bool m_collapse_on_up;                      // 在已选中的多项上按下但没拖：松开时只留这一项
    bool m_drag_in_progress;
    int m_drop_index;
    bool m_is_minified;
    bool m_allow_close;
    bool m_initialized;
    bool m_tracking_mouse;
    bool m_tracking_nc_mouse;
    bool m_restore_posted;
    bool m_blurred;
    bool m_removed;            // directory deleted: never write the metadata again
    bool m_pending_position;   // throttled saves waiting on their timer
    bool m_pending_size;
    bool m_group_move;         // Ctrl+拖标题：所有围栏一起移动
    RECT m_move_origin;
    std::vector<std::pair<HWND, RECT> > m_group_origins;
    int m_prev_height;
    int m_scroll_height;
    int m_scroll_offset;
    ULONGLONG m_last_mouse_invalidate;
    ULONGLONG m_last_position_save;
    ULONGLONG m_last_size_save;
    McShellMenu m_shell_menu;
    McNewMenu m_new_menu;
    std::wstring m_new_menu_container;  // 普通围栏「新建」用的临时 Items 子目录
    McFolderWatch m_watch;
    McDropTarget* m_drop_target;
    int m_rename_target;            // McRenameTarget
    std::wstring m_rename_path;     // 正在重命名的项目
    std::wstring m_rename_text;     // 可编辑部分（文件为主文件名）
    std::wstring m_rename_ext;      // 固定不动的扩展名（文件夹为空）
    size_t m_rename_caret;
    bool m_rename_pristine;         // 刚进入编辑：主文件名处于全选，第一次输入会替换它
    bool m_rename_caret_on;         // 光标闪烁相位
    float m_rename_scroll;          // 文字比框宽时的水平平移
    float m_rename_origin;          // 上次绘制时文字起点（客户区 x），点击定位光标用
    Gdiplus::RectF m_rename_box;    // 上次绘制的编辑框（客户区坐标）
    POINT m_rename_caret_point;     // 输入法组字窗口位置

    static std::vector<McFenceWnd*> s_registry;
    static bool s_hidden;
    // 正在由某个围栏发起的 OLE 拖出：落回围栏上时按内部移动处理，而不是再导入一份副本。
    static McFenceWnd* s_outgoing_source;
    static std::vector<std::wstring> s_outgoing_paths;
    static bool s_outgoing_handled;
    static HWND s_app_window;
    static int s_icon_size;    // 跟随桌面的图标边长（逻辑像素）
};
