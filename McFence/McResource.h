// McResource.h - 包含 Resource.h 的唯一入口。
//
// Resource.h 由 Visual Studio 资源编辑器维护，每次保存 .rc 都会按系统 ANSI 代码页（中文系统为 GBK）
// 重写它，开头那行“Microsoft Visual C++ 生成的包含文件”的中文注释在 /utf-8 下会报 C4828。
// 手工转成 UTF-8 只能管到下一次编辑资源，所以在这里屏蔽这条警告；文件里只有注释是非 ASCII，
// 宏定义本身不受影响。
#pragma once

#pragma warning(push)
#pragma warning(disable : 4828)
#include "Resource.h"
#pragma warning(pop)
