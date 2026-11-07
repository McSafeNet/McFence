# McFence

McFence 是一个 Windows 桌面围栏工具：把快捷方式、文件和文件夹收进半透明的桌面格子，可拖动排序、
可跨围栏搬运、可拖出到资源管理器、可映射真实文件夹、可锁定与自动折叠。最初由 SimpleGrid 的 C++
移植版整理而来，现在是独立工程，数据格式自有、不再与 SimpleGrid / NoFences 互通。工程骨架沿用
Visual Studio「Windows 桌面应用程序」模板，构建工具链固定为 **Visual Studio 2017 - Windows XP
(v141_xp)**，最低可运行到 **Windows XP SP3**，不依赖 .NET。

## 1. 工程结构

```text
McFence\                    解决方案目录
├─ McFence.sln
├─ McFence\                 工程目录（全部源码与资源）
│  ├─ McFence.cpp            入口：wWinMain / MyRegisterClass / InitInstance / WndProc / About
│  ├─ McFence.h              进程级声明（hInst、szTitle、szWindowClass、关于框）
│  ├─ McFence.rc             资源：图标、菜单、快捷键、关于框、应用程序清单
│  ├─ McFence.manifest       supportedOS + Per-Monitor V2 DPI 声明
│  ├─ Resource.h             资源与控件 / 菜单命令 ID
│  ├─ framework.h / targetver.h
│  ├─ McCompat.h             XP 安全边界：版本、动态 API 表、工程常量、窗口类名
│  ├─ McUtil.*               路径 / 字符串 / GUID / 时间 / 文件复制
│  ├─ McLog.*                app_diag.log
│  ├─ McModel.h              围栏与项目数据模型、排序方式
│  ├─ McStore.*              XML、fences.index、Items 仓库、排序与文件夹枚举
│  ├─ McSettings.*           settings.ini 全局选项、开机启动（HKCU\...\Run）
│  ├─ McShellOps.*           拖出（自建 CF_HDROP 数据对象）、复制/移动/回收、Shell「新建」菜单、选文件夹
│  ├─ McDesktop.*            SHELLDLL_DefView、桌面空白处判断、桌面双击钩子、显示器纠正、文件夹监视
│  ├─ McSurface.*            分层窗口绘制面（GDI+ → 预乘 DIB → UpdateLayeredWindow）
│  ├─ McIcons.*              图标 / 缩略图缓存（jumbo→超大→大→关联图标）
│  ├─ McShellMenu.*          文件的真实 Shell 右键菜单（IContextMenu2/3）
│  ├─ McShortcut.*           .lnk 解析与启动、资源管理器定位
│  ├─ McFenceWnd.*           围栏窗口：绘制、命中、多选/框选、拖放、菜单、吸附、折叠、滚动、DPI
│  └─ McDialogs.*            标题栏高度 / 选项 两个对话框
├─ tools\
│  ├─ seed.ps1               造隔离测试数据
│  └─ verify.ps1             一键验收（构建 + PE 检查 + XML 比对 + 渲染快照）
├─ Release\x86\McFence.exe   x86 构建产物
└─ Release\x64\McFence.exe   x64 构建产物
```

## 2. 命名与编码风格

沿用 McFence 模板的风格，并按工程名统一加前缀：

| 对象 | 规则 | 例子 |
|---|---|---|
| 文件 | `Mc` + PascalCase | `McFenceWnd.cpp`、`McStore.h` |
| 跨文件函数 | `Mc` + PascalCase | `McStoreWriteFence()`、`McJoinPath()`、`McDumpSurface()` |
| 类 / 结构 | `Mc` + PascalCase | `McSurface`、`McIconCache`、`McFenceInfo` |
| 类成员 | `m_` + 名称；静态成员 `s_` | `m_surface`、`m_drop_index`、`s_registry` |
| 内部函数 | PascalCase | `Paint()`、`HitTestItem()`、`EscapeXml()` |
| 宏 / 常量 | `MCFENCE_` 或全大写 | `MCFENCE_DATA_FOLDER`、`MCFENCE_WM_ICON_READY` |
| 资源 ID | 模板原有命名 | `IDC_MCFENCE`、`IDM_MCFENCE_LOCK`、`IDC_MCFENCE_VALUE` |
| 局部变量 | 匈牙利式 | `hWnd`、`szSelfTest`、`nViews`、`pfnPreferredMode` |
| 注释 | 中文，`//` 或模板的 `//  函数: / 目标: / 注释:` 三段式 | 见 `McFence.cpp` |

## 3. 构建

VS2019（含 v141_xp 工具集）打开 `McFence.sln`，选 `Release|x64` 或 `Release|x86` 直接生成；
或者命令行：

```powershell
$msbuild = 'D:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild .\McFence.sln /p:Configuration=Release /p:Platform=x64
& $msbuild .\McFence.sln /p:Configuration=Release /p:Platform=x86
```

产物：`Release\x64\McFence.exe`（子系统 5.02）、`Release\x86\McFence.exe`（子系统 5.01）。

一键验收：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify.ps1
```

它会做四件事：构建两个平台 → 检查 PE 头与导入表（不得出现 `api-ms-win-*`、`ucrtbase`、
`vcruntime`、`dwmapi`、`d2d1`）→ 用隔离数据验证 XML 读入后原样重写**逐字节不变**
→ 渲染三个围栏（普通、折叠、映射文件夹）并把分层缓冲导出 PNG。

## 4. 为什么能跑到 XP

`v141_xp` 把 `_WIN32_WINNT` 与导入库都固定在 XP 时代，所以任何 Vista 之后的能力都只能在
运行时探测（`McCompat.h` 里的动态入口表）：

| 能力 | Windows XP | Windows 7 | Windows 8 / 8.1 | Windows 10 / 11 |
|---|---|---|---|---|
| 面板呈现（逐像素 alpha） | `UpdateLayeredWindow` | 同左 | 同左 | 同左 |
| 背景模糊 | 无（设计如此） | 无（分层窗口不参与 Aero 模糊） | 无 | 未文档化 `SetWindowCompositionAttribute` |
| 高清图标 | 仅大图标 | Jumbo / 超大图标 | 同左 | 同左 |
| 文件夹图标 | `SHGetFileInfo` 回退 | `SHGetStockIconInfo` | 同左 | 同左 |
| DPI | 无感知 | 系统 DPI（运行时 `SetProcessDPIAware`） | Per-Monitor | Per-Monitor V2（清单声明） |
| 缩略图 | GDI+ `GetThumbnailImage` | 同左 | 同左 | 同左 |
| 拖出到资源管理器 | 自建 `IDataObject` + `DoDragDrop` | 同左 | 同左 | 同左 |
| 自然排序 | 运行时解析 `StrCmpLogicalW` | 同左 | 同左 | 同左 |
| 桌面图标命中 | `oleacc` 运行时加载（不跨进程写内存） | 同左 | 同左 | 同左 |

## 5. 数据

```text
%LOCALAPPDATA%\McFence\<guid>\__fence_metadata.xml   每个围栏的配置（<McFence version="2">）
%LOCALAPPDATA%\McFence\fences.index                 围栏 GUID 列表（决定加载顺序）
%LOCALAPPDATA%\McFence\settings.ini                 全局选项（可用记事本改）
%LOCALAPPDATA%\McFence\Items\<guid32>\<原始名称>     拖入项目的存放位置（默认移入，按住 Ctrl 复制）
%LOCALAPPDATA%\McFence\app_diag.log                 诊断日志
HKCU\Software\Microsoft\Windows\CurrentVersion\Run\McFence   开机启动（不需要管理员）
```

围栏 XML 元素：`Id`、`Name`、`PosX`/`PosY`/`Width`/`Height`（96 dpi 逻辑像素）、`Locked`、`CanMinify`、
`TitleHeight`、`Folder`（非空即映射文件夹围栏，此时不写 `Files`）、`Sort`（0 手动 1 名称 2 日期 3 大小
4 类型）、`SortDescending`、`Files`。读取时仍能识别早期 `<FenceInfo>` 根，下次保存即升级。

`settings.ini` 的 `[McFence]` 节：`DoubleClickDesktop`、`KeepOnShowDesktop`、`Snap`（0/1），
`HotkeyModifiers`（MOD_ALT=1 MOD_CONTROL=2 MOD_SHIFT=4 MOD_WIN=8 之和）、`HotkeyKey`（虚拟键码，0 不注册）。
默认：双击桌面开、Win+D 保留关、吸附开、热键 Ctrl+Alt+F。

## 5.1 功能一览

| 功能 | 操作 | 来源 |
|---|---|---|
| 托盘图标 | 双击：显示/隐藏全部；右键：新建、映射文件夹、开机启动、选项、数据文件夹、退出 | openFrence |
| 全局热键 | 默认 Ctrl+Alt+F 显示/隐藏全部栅栏，选项里可改或清除 | openFrence / DeskBox |
| 双击桌面空白处 | 显示/隐藏全部栅栏（`WH_MOUSE_LL` + 辅助功能接口判断不在图标上） | DeskBox 改写 |
| 多选 | 单击、Ctrl+单击、空白处框选（Ctrl 框选为切换）、Ctrl+A、Esc 取消 | openFrence |
| 拖入 / 拖出 | 默认移动、按住 Ctrl 复制（来源不允许移动时复制）；选中项拖出所有栅栏之外即成为系统拖拽，可放到资源管理器、桌面、其他程序 | DeskBox 思路，XP 自实现 |
| 栅栏间拖动 | 普通→普通移动记录；普通→映射 / 映射→映射移动真实文件；映射→普通把真实文件移入 Items | 新增 |
| 文件夹映射 | 「新建文件夹映射栅栏」：显示真实文件夹内容，变化自动刷新（250 ms 防抖） | openFrence / DeskBox |
| 排序 | 名称（自然排序）/ 日期 / 大小 / 类型，升降序；文件夹总在前 | openFrence |
| 「新建」子菜单 | 右键空白处：资源管理器自己的「新建」（文件夹、文本文档、第三方模板），建好即原地重命名 | openFrence |
| 项目重命名 | F2 / 右键「重命名」/ 新建后：直接在图标标签上编辑（不弹窗），主文件名全选、扩展名固定；Enter 或点别处保存，Esc 放弃；拒绝 `\ / : * ? " < > |` | openFrence |
| 栅栏重命名 | 双击标题栏（或右键「重命名栅栏」）在标题栏上原地编辑，按键同上 | openFrence |
| 名称显示 | 显示带扩展名的完整文件名；快捷方式（注册了 NeverShowExt 的 .lnk / .url 等）和资源管理器一样隐藏扩展名，并按桌面原始尺寸绘制快捷方式箭头 | openFrence |
| 删除 | Del：普通栅栏移除记录并把 Items 中的文件放进回收站；映射栅栏把真实文件放进回收站 | 新增 |
| 吸附 | 拖动/缩放时吸附工作区边缘与其他栅栏（8 px 间距），按住 Alt 临时关闭 | DeskBox 改写 |
| 整体移动 | 按住 Ctrl 拖标题栏，所有未锁定栅栏一起移动 | DeskBox 改写 |
| Win+D 保留 | 选项里开启后，栅栏的所有者设为 `SHELLDLL_DefView`（不跨进程 SetParent） | DeskBox 方案 |
| 显示器纠正 | 启动、显示设置变化、显示全部时，跑到屏幕外的栅栏挪回最近显示器 | openFrence |
| 资源管理器重启 | 栅栏自动重建、重新挂接所有者，托盘图标重新添加 | 新增 |

## 6. 开发 / 测试开关

| 环境变量 | 作用 |
|---|---|
| `MCFENCE_DATA_ROOT` | 指定数据目录，避免碰真实配置 |
| `MCFENCE_INSTANCE` | 互斥体后缀，允许与正式实例并存 |
| `MCFENCE_SELFTEST=1` | 载入后把每个围栏原样重写一遍然后退出（XML 读写往返回归） |
| `MCFENCE_DUMP_SURFACE=<png>` | 把分层缓冲合成到中性背景存 PNG（多个围栏依次 `_2`、`_3`），然后退出 |

## 7. 已知限制

1. 圆角由 GDI+ 裁剪实现，并补了 1px 内描边；文字用 GDI+ 抗锯齿绘制，逐像素 alpha 表面无法使用 ClearType。
2. Win10/11 用 `SetWindowCompositionAttribute` 开背景模糊；Win7 上为半透明；XP 上没有模糊。
3. 栅栏内部 / 栅栏之间的拖动用鼠标捕获实现，不显示系统拖拽图像；拖出栅栏后才是系统 OLE 拖拽。
4. 「Win+D 保留」与「双击桌面」都依赖资源管理器的窗口结构（`Progman`/`WorkerW`/`SHELLDLL_DefView`），
   在 XP、7、10、11 上已按结构查找，但第三方桌面替换程序下可能不生效。
5. 双击桌面需要低级鼠标钩子，少数安全软件会提示；不需要时在选项里关掉即可卸载钩子。
6. 映射文件夹只监视直接子项，不递归；隐藏和系统文件不显示。

## 8. 人工验收清单

1. 删除数据目录后启动：出现名为「我的栅栏」的栅栏，托盘出现图标。
2. 拖动标题移动、拖四角/四边缩放，靠近屏幕边缘或另一个栅栏时会吸附；按住 Alt 不吸附；
   按住 Ctrl 拖标题，所有栅栏一起走。重启后位置尺寸保持（4 秒节流写盘）。
3. 从资源管理器拖文件进来：文件被移入 `Items\<guid32>\`；按住 Ctrl 拖入则复制，原文件仍在。
4. 单击、Ctrl+单击、空白处框选、Ctrl+A；Esc 清除选择。
5. 选几个图标拖到另一个栅栏 / 同栅栏换位置；拖到资源管理器窗口或桌面：文件被移动过去（按住 Ctrl 复制）。
6. 右键空白处 →「新建」子菜单里有文件夹、快捷方式、文本文档等（不直接摊在一级菜单里）；
   选「文本文档」：出现在栅栏里并直接进入原地重命名。双击标题栏：原地编辑栅栏名称。
7. 「新建文件夹映射栅栏」选一个文件夹：内容显示出来；在资源管理器里增删改名，栅栏 1 秒内跟着变；
   往映射栅栏里拖文件会移动进该文件夹（Ctrl 复制），Del 会放进回收站。
8. 排序：名称排序时 file2 在 file10 前；切换日期 / 大小 / 类型、递增 / 递减。
9. F2 在图标标签上原地重命名（无弹窗）；Del 删除；Enter / 双击打开；Shift+右键是完整 Shell 菜单。
10. Ctrl+Alt+F、托盘双击、双击桌面空白处：全部栅栏隐藏 / 显示；双击桌面图标不会触发。
11. 选项里开「Win+D 时保留栅栏」，按 Win+D：栅栏仍在。
12. 托盘「开机启动」勾选后 `HKCU\...\Run\McFence` 出现；取消后消失。
13. 结束并重启 Explorer：栅栏重新出现并贴底，托盘图标恢复；重复启动不会出现第二个实例。
