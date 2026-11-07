// McStore.h - 持久化：__fence_metadata.xml、fences.index、Items 仓库。
#pragma once

#include "McModel.h"

#include <string>
#include <vector>


extern const wchar_t* const MCFENCE_META_FILE;
extern const wchar_t* const MCFENCE_INDEX_FILE;

std::wstring McStoreFenceDirectory(const std::wstring& id);

// Reads one fence directory. Tolerates unknown elements and a missing BOM.
bool McStoreReadFence(const std::wstring& directory, McFenceInfo& out);

// Writes the metadata through a temp file plus ReplaceFile, then keeps the index.
bool McStoreWriteFence(const McFenceInfo& info);

void McStoreEnsureIndex(const std::wstring& directory);
bool McStoreRemoveFence(const McFenceInfo& info);

// Directories listed by fences.index, falling back to a scan when the index is
// missing.
std::vector<std::wstring> McStoreFenceDirectories();

// Loads every fence; failures are reported through |errors| and skipped so one
// damaged directory cannot stop the others.
int McStoreLoadAll(std::vector<McFenceInfo>& out, std::vector<std::wstring>& errors);

// Creates a fence at the default geometry (100, 250, 300x300); |folder| non-empty
// makes it a folder-mapped fence.
McFenceInfo McStoreCreateFence(const std::wstring& name, const std::wstring& folder = std::wstring());

// New empty Items\<guid32>\ container (for the shell "New" submenu); empty on failure.
std::wstring McStoreNewItemContainer();

// Moves (or copies) a dropped file/folder into Items\<guid32>\ and returns the stored path,
// or an empty string when the item could not be moved / copied.
std::wstring McStoreImportItem(const std::wstring& source, bool move);

// |path| 位于 Items 仓库内（是本程序自己的副本，而不是用户的原文件）。
bool McStoreIsManagedItem(const std::wstring& path);

// 把仓库里的副本删到回收站（不确认），并删掉随之变空的 <guid32> 容器。仓库外的路径一律忽略。
void McStoreRecycleManaged(const std::vector<std::wstring>& paths);


