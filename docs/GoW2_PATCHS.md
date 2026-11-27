# 与 GoW2 项目绑定的标识符清单

本树是《战争机器 2》重编译项目所用 SDK 的逐字副本，因此含一批**带 `gow2` 前缀的名字**。
本文件把它们一条条列清楚，用于将来把这棵树做成「跨项目通用 SDK」时定位改动面。

统计口径（2026-11-27，补丁 `0002`–`0100` 全应用后）：

| 类别 | 数量 |
|---|---|
| 文件名含 `gow2` | 13 |
| 命名空间含 `gow2` | 3 种 |
| SDK 注册的 `gow2_*` cvar | 17 |
| 引用但**不由 SDK 注册**的 `gow2_*` cvar | 2（游戏侧提供） |
| 环境变量 `GOW2_*` | 7 |
| 编译期宏 `GOW2_*` | 5 类 |
| 运行时探测文件名 | 1（`gow2_trace.now`） |

## 耦合程度分档

判断某一项「能不能直接拿去给别的游戏用」，看的是**改名字够不够**，分三档：

| 档 | 含义 | 处理成本 |
|---|---|---|
| **A 只脏名字** | 逻辑与具体游戏无关，改文件名/cvar 名即可复用 | 重命名 |
| **B 名字 + 游戏相关默认值** | 逻辑通用，但默认值来自 GoW2（如常量寄存器位置），换游戏必须改默认值 | 重命名 + 调参 |
| **C 真依赖游戏侧** | 依赖游戏自己注册的 cvar 或游戏特有的输入/菜单约定 | 重写或删除 |

## 文件名

| 文件 | 命名空间 | 档 | 内容 |
|---|---|---|---|
| `include/rex/graphics/d3d12/gow2_temporal.h` | `rex::graphics::d3d12::gow2`（内部还有一层 `gow2`） | B | 时间域超分基础：场景子像素抖动（Halton 序列）、动画向量构造 |
| `src/graphics/d3d12/gow2_temporal.cpp` | `rex::graphics::d3d12::gow2` | B | 上述实现。读取 `gow2_vp_register` / `gow2_campos_register` 指定的常量寄存器取视图投影矩阵与相机世界坐标 |
| `include/rex/graphics/d3d12/gow2_dlss.h` | `rex::graphics::d3d12::gow2` | B | NVIDIA DLSS 接入（需要 DLSS SDK 才有符号） |
| `src/graphics/d3d12/gow2_dlss.cpp` | `rex::graphics::d3d12::gow2` | B | 上述实现；用 `GOW2_HOME` 找 SDK DLL |
| `include/rex/graphics/d3d12/gow2_fsr.h` | `rex::graphics::d3d12::gow2` | B | AMD FidelityFX FSR 3.1 / FSR 4 接入 |
| `src/graphics/d3d12/gow2_fsr.cpp` | `rex::graphics::d3d12::gow2` | B | 上述实现；`GOW2_FSR_DEBUG` 控制调试输出 |
| `include/rex/graphics/d3d12/gow2_framegen.h` | `rex::graphics::d3d12::gow2` | B | FSR 3 插帧（FidelityFX API） |
| `src/graphics/d3d12/gow2_framegen.cpp` | `rex::graphics::d3d12::gow2` | B | 上述实现 |
| `src/graphics/d3d12/gow2_upscale.cpp` | `rex::graphics::d3d12::gow2` | B | 超分主流程：把相机动画向量 → DLSS/FSR → 写回场景色。**本文件已不含 `gow2_render_scale`**（补丁 `0093` 删除，改为游戏侧自行定义） |
| `include/rex/graphics/d3d12/gow2_trace.h` | `rex::graphics::d3d12`（无 gow2 命名空间） | A | 帧探针：工作目录出现 `gow2_trace.now` 就记录后续若干帧的 resolve/swap 参数。**函数名也带 gow2**：`Gow2TraceBeginFrame`、`Gow2TraceActive`、`Gow2TraceEndFrame`，变量 `gow2_trace_frame`/`gow2_trace_remaining`；调用点在 `d3d12/command_processor.cpp` 与 `d3d12/render_target_cache.cpp` |
| `include/rex/ui/d3d12/gow2_fence_wait.h` | `rex::ui::d3d12::fence_wait` | **A** | 补丁 `0099` 引入。fence 等待与失败判定（`Await`、`SignalAndAwait`），`Status` 区分 `kDeviceRemoved`/`kSignalFailed`/`kEventFailed`/`kWaitFailed`/`kIncomplete`。**命名空间与函数名全无 gow2** |
| `src/graphics/d3d12/gow2_precache_plan.h` | `rex::graphics::d3d12::precache` | **A** | 补丁 `0096` 引入。替换纹理预加载的规划：解析 `[texpack]` 日志、按最近使用排优先级、算内存预算（`ParseHistory`、`Prioritize`、`BudgetBytes`、`CopyMipRows`、`Fits`）。**命名空间与函数名全无 gow2** |
| `src/input/mnk/gow2_menu_escape_back.h` | `rex::input::gow2_menu_escape` | **C** | 补丁 `0097` 引入。菜单态把 Escape 当 B。依赖外部传入的 `native_keys`，其值来自游戏侧 cvar `gow2_native_keys` |

## cvar（SDK 注册）

全部 17 个都带 `gow2_` 前缀，写进 toml、命令行和 `cvar` 列表时用户直接可见。

| cvar | 类型 | 默认 | 定义位置 | 档 | 含义 |
|---|---|---|---|---|---|
| `gow2_temporal` | bool | `false` | `gow2_temporal.cpp` | B | 开启场景子像素抖动（DLSS/FSR 的基础） |
| `gow2_jitter_scale` | double | `1.0` | `gow2_temporal.cpp` | A | 抖动幅度倍数（调试：调大即可看见抖动） |
| `gow2_jitter_phases` | int32 | `8` | `gow2_temporal.cpp` | A | Halton 抖动序列长度 |
| `gow2_vp_register` | int32 | `0` | `gow2_temporal.cpp` | **B** | 存放视图投影矩阵的 VS 浮点常量首寄存器（4 行）——**GoW2 的寄存器约定** |
| `gow2_campos_register` | int32 | `13` | `gow2_temporal.cpp` | **B** | 存放相机世界坐标的 VS 浮点常量行——**GoW2 的寄存器约定** |
| `gow2_upscale_status` | string | `""` | `gow2_temporal.cpp` | A | FPS 覆盖层显示的超分状态行（GPU 插件写） |
| `gow2_dlss` | bool | `false` | `gow2_upscale.cpp` | B | 对场景色跑 DLSS（需 `gow2_temporal`） |
| `gow2_upscaler` | string | `"off"` | `gow2_upscale.cpp` | A | `off`/`dlss`/`fsr3`/`fsr4`/`auto` |
| `gow2_upscale_quality` | int32 | `-1` | `gow2_upscale.cpp` | A | -1 自动，0 性能 … 5 原生（DLAA） |
| `gow2_dlss_hdr` | bool | `false` | `gow2_upscale.cpp` | A | 告知 DLSS 场景色为 HDR/线性 |
| `gow2_mv_debug` | int32 | `0` | `gow2_upscale.cpp` | A | 1 把动画向量写进场景色，2 直通拷贝 |
| `gow2_upscale_typed_copy` | bool | `false` | `gow2_upscale.cpp` | A | 给 FSR 与 DLSS 相同的有类型拷贝（调试用） |
| `gow2_dlss_runtime` | bool | `true` | `gow2_dlss.cpp` | A | 只读：本运行时是否用 DLSS SDK 编出 |
| `gow2_adaptive_precache` | bool | `false` | `texture_replacement.cpp` | **A** | 补丁 `0096`。按本地会话历史预热材质包纹理（需重启） |
| `gow2_precache_budget_mb` | int32 | `3072` | `texture_replacement.cpp` | **A** | 预热可用额外 CPU 内存上限（64–3072，独立于 VRAM） |
| `gow2_precache_startup_ms` | int32 | `6000` | `texture_replacement.cpp` | **A** | 启动预热时间预算（100–10000，在 DDS 之间检查） |
| `gow2_menu_escape_back` | bool | `true` | `mnk_input_driver.cpp` | **C** | 补丁 `0097`。菜单里 Escape 当 B；仅在键盘模拟手柄时生效 |

## 引用但不由 SDK 注册的 cvar

这两个由**游戏侧**定义，SDK 只 `Query`。用 `rex::cvar::Query` 读取，未注册时返回默认值，不会崩，但功能不生效。

| cvar | 读取位置 | 作用 |
|---|---|---|
| `gow2_native_keys` | `mnk_input_driver.cpp`（补丁 `0097`） | 游戏是否自己读按键。为 false 时 Escape→B 不生效 |
| `gow2_frame_generation` | `gow2_framegen.cpp` | `fsr3` 开启 FSR 3 插帧；由游戏侧注册以便菜单联动 |

## 环境变量

| 变量 | 读取位置 | 作用 |
|---|---|---|
| `GOW2_VFS_OVERRIDES` | `filesystem/devices/host_path_device.cpp` | 单个 guest 文件重定向到宿主文件（`guest=host` 列表） |
| `GOW2_VFS_OVERRIDES_FILE` | 同上 | 上者的 UTF-8 文件形式；变量优先于文件 |
| `GOW2_HOME` | `gow2_dlss.cpp`、`gow2_fsr.cpp` | 查找 FidelityFX / DLSS SDK 载入路径 |
| `GOW2_FSR_DEBUG` | `gow2_fsr.cpp` | FSR 调试输出 |
| `GOW2_TARGET_W` / `GOW2_TARGET_H` | `gow2_upscale.cpp` | 覆盖超分目标分辨率（调试） |
| `GOW2_D3D12_DEBUG` | `ui/d3d12/d3d12_provider.cpp` | 启用 D3D12 调试层 |

## 编译期宏

| 宏 | 作用 |
|---|---|
| `GOW2_HAS_FFX` | 找到 FidelityFX SDK（`ffx_api.h`）时为真，启用真实 FSR 实现；否则走 stub |
| `GOW2_FFX_SDK` | FidelityFX SDK 根目录 |
| `GOW2_HAS_DLSS` / `GOW2_DLSS_SDK` | 同上，DLSS 侧 |
| `GOW2_DEV_TOOLS` | 开发者工具开关（帧探针等） |

## 运行时探测文件

| 名字 | 位置 | 作用 |
|---|---|---|
| `gow2_trace.now` | 进程工作目录 | 存在时 `gow2_trace.h` 开始记录后续若干帧的 resolve/swap 参数 |

## 做成通用 SDK 时的处理建议

| 动作 | 对象 | 代价 |
|---|---|---|
| **只改名字**（推荐） | 13 个文件名、17 个 cvar 名、7 个环境变量、5 个宏 | 与上游 patch 后续同步会**全部冲突**，每次上游更新都要手工重打 |
| **只改名字，保留 cvar 兼容** | cvar 可用 `REXCVAR_DEFINE_*` 的原名 + `alias` 方式双挂，配置文件不会失效 | 小，但 SDK 的 cvar 系统需支持别名 |
| **删除** | `gow2_menu_escape_back.h` + `mnk_input_driver.cpp` 里的路由调用（补丁 `0097`） | 无（不依赖其他文件），但以后想用要重打 |
| **参数化** | `gow2_vp_register` / `gow2_campos_register` 的默认值 | 无痛：换项目时在 toml 里改这两个值即可，不必改代码 |

最短路径：**A 档全部重命名，B 档保留并在每个项目的 toml 里指定寄存器，C 档删除**。这样这棵树对任何 Xenos 游戏都能用，而 GoW2 侧只需在自己的 toml 里补回 `gow2_*` 名字（或直接改用新名字）。
