// McModel.h - 落盘数据模型（__fence_metadata.xml，McFence 自有格式 version 2）。
#pragma once

#include "framework.h"

#include <string>
#include <vector>


enum McEntryType
{
    MCFENCE_ENTRY_FILE = 0,
    MCFENCE_ENTRY_FOLDER = 1
};

struct McEntry
{
    std::wstring path;
    McEntryType type;

    McEntry() : type(MCFENCE_ENTRY_FILE) {}
    McEntry(const std::wstring& p, McEntryType t) : path(p), type(t) {}

    std::wstring display_name() const;  // full file name including extension
};

// 排序方式。自有项目围栏：选中后把列表按该方式重排一次；映射文件夹围栏：持续生效。
enum McSortMode
{
    MCFENCE_SORT_NONE = 0,
    MCFENCE_SORT_NAME = 1,
    MCFENCE_SORT_DATE = 2,
    MCFENCE_SORT_SIZE = 3,
    MCFENCE_SORT_TYPE = 4
};

// 一个围栏的持久化信息；成员顺序即 XML 元素顺序。
struct McFenceInfo
{
    std::wstring id;  // GUID in lower-case "D" form, no braces
    std::wstring name;
    int pos_x;
    int pos_y;
    int width;
    int height;
    bool locked;
    bool can_minify;
    int title_height;
    std::wstring folder;  // 映射的文件夹；空表示围栏自己管理 files 列表
    int sort;             // McSortMode
    bool sort_descending;
    std::vector<std::wstring> files;  // 映射文件夹围栏不落盘，运行时由目录枚举得到

    McFenceInfo()
        : pos_x(0), pos_y(0), width(300), height(300), locked(false), can_minify(false),
          title_height(35), sort(MCFENCE_SORT_NONE), sort_descending(false)
    {
    }
};

bool McEntryFromPath(const std::wstring& path, McEntry& out);
bool McItemExists(const std::wstring& path);

// 按 |mode| 排序路径列表（名称资源管理器式自然排序）。
void McSortPaths(std::vector<std::wstring>& paths, int mode, bool descending);

// 映射文件夹的直接子项（跳过隐藏 / 系统项），已按 |mode| 排好序。
std::vector<std::wstring> McEnumerateFolder(const std::wstring& folder, int mode, bool descending);

