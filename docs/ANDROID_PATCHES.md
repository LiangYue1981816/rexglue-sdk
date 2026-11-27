# 本 SDK 树已应用的补丁

本树 = ReXGlue 提交 `923c1a5` + 《战争机器 2》重编译项目 `sdk/patches/` 的**全套 95 个补丁**，
按文件名顺序应用。该项目的 `scripts/build-sdk.ps1`（Windows）与 `scripts/build-android.ps1`
（Android）也是拿同一套补丁序列从干净 checkout 编出来的，因此本树的代码与它发布的构建一致。

| 项 | 值 |
|---|---|
| 基线提交 | `923c1a5` — feat(ui): apply the monitor, window size and fullscreen mode without a restart |
| 补丁来源 | `santiquiroz/gears-of-war-2-pc-port` → `sdk/patches/` |
| 补丁作者 | Santiago Quiroz Upegui（主体；移植与外部贡献见下表「来源」列） |
| 补丁数量 | **95**（`0002` … `0100`，无 `0008` `0009` `0064` `0098`） |
| 最后同步 | 2026-11-27（补到 `0100`） |
| 应用情况 | 95 条全部应用，无跳过 |

本树里带 `gow2` 前缀的文件名、cvar、环境变量与宏另有清单，见 [GoW2_PATCHS.md](GoW2_PATCHS.md)。

上游的序列是**只追加**的：已存在的编号文件不会被重写，新能力以更大的编号追加。所以同步
永远只做「加文件」，不会改动本树已应用的条目。

## 上游出新补丁时怎么跟

1. 更新补丁来源（`git -C <GoW2 仓库> pull`，或直接取上面的地址）。
2. 列出它的 `sdk/patches/*.patch`。**编号大于 `0100` 且不在下表里的，就是新增的。**
3. 逐条应用到本树，编号从小到大：`git apply --whitespace=nowarn <文件>`。
   任一条失败就停下并重读那条补丁——失败意味着本树上下文已与上游不同，不是"可选"。
4. 把新增的行追加到下表，并更新表格上方的「补丁数量」与「最后同步」。

**不要在本树上整套重放**：95 条都已在内，且它们彼此累积（后一条的上下文包含前一条的改动）。
整套重放必须先回到干净的 `923c1a5`，再按序应用。

**按序应用，不能并行 check**：`0095` 与 `0094` 改的是 `native_surfaces.h` 同一段，`0095` 的 diff
基于 `0094` 之后的状态。单独 `git apply --check 0095-*.patch` 会报 "patch does not apply"，那是
顺序问题，不是冲突——先 `0094` 再 `0095` 就干净。同理，任何新补丁都要按文件名序逐条落地。

## 与上游的唯一差异

为了让 Windows 在没有 FidelityFX SDK 时也能链接 `rexgpu-xenos.dll`，改了一行 stub：

| 文件 | 上游补丁 | 本树 | 原因 |
|---|---|---|---|
| `src/graphics/d3d12/gow2_fsr.cpp:308` | `bool FsrFrameGenerationProvider(FsrProvider*) { return false; }` | `bool FsrFrameGenerationProvider(FsrProvider*, int) { return false; }` | 头文件声明是 `(FsrProvider*, int wanted_major = 0)`，`#ifdef GOW2_HAS_FFX` 分支定义的也是两参数形态；`#else` 的 stub 只写了一个参数，于是没有 `ffx-sdk` 时两参数符号无定义、链接失败。`GOW2_HAS_FFX` 只在 `${REXGLUE_ROOT}/../ffx-sdk/Kits/FidelityFX/api/include/ffx_api.h` 存在时才定义，上游构建始终有这个 checkout，所以那段 stub 在他们那里从未被编译。**上游修好 stub 后请撤回这一行。** |

除此之外与上游逐字一致。

## 编译状态

| 目标 | 结果 | 产物 |
|---|---|---|
| Windows x64 (Release) | 通过（`0002`–`0091`） | `out/win-amd64/Release/rexglue.exe`、`rexruntime.dll`、`rexgpu-xenos.dll` |
| Android arm64（arm64-v8a / API 29 / NDK r29） | 通过（`0002`–`0091`） | `out/android-arm64/librexruntime.so`、`librexgpu-xenos.so`、`libSDL3.so`、`libc++_shared.so` |

**`0092`–`0100` 应用后尚未重新验证编译**（2026-11-27）。这段是 13 个文件改动 + 3 个纯新增头文件
（`gow2_fence_wait.h`、`gow2_precache_plan.h`、`gow2_menu_escape_back.h`），都不需要改 CMake 源列表；
`0096`/`0097` 的 cvar 由 SDK 自行注册，不依赖游戏侧定义。

两端都由仓库根目录的 `build-rexglue-sdk.ps1` 产出；不带参数即两端都编，`-SkipWindows` /
`-SkipAndroid` 只编单端。Android 的 `libc++_shared.so` 由脚本从 NDK sysroot 拷到产物目录。

Android 三个 `.so` 的 `NEEDED` 只有 `libSDL3.so`、`libc++_shared.so` 与系统库，所以上面四个
文件就是完整的 APK `jniLibs` 集合。

## 补丁全景（按领域分组）

| 领域 | 补丁 |
|---|---|
| 超分与插帧（6） | `0002` `0030` `0069` `0072` `0074` `0093` |
| System Link / 联机（4） | `0003` `0010` `0011` `0012` |
| 材质包（4） | `0004` `0023` `0073` `0096` |
| 输入（9） | `0005` `0006` `0022` `0029` `0046` `0048` `0060` `0061` `0097` |
| Android / Linux / POSIX 平台（15） | `0007` `0052` `0053` `0055` `0075` `0076` `0078` `0079` `0080` `0081` `0083` `0084` `0085` `0086` `0092` |
| XAM / 宿主 UI（4） | `0013` `0047` `0049` `0050` |
| D3D12 专有（10） | `0017` `0021` `0024` `0025` `0026` `0027` `0051` `0071` `0090` `0099` |
| Native renderer（18） | `0032` `0036`–`0045` `0056` `0057` `0058` `0066` `0067` `0094` `0095` |
| Vulkan / GPU 通用（11） | `0016` `0018` `0019` `0020` `0028` `0062` `0063` `0082` `0089` `0091` `0100` |
| 运行时 / 图形通用（14） | `0014` `0015` `0031` `0033` `0034` `0035` `0054` `0059` `0065` `0068` `0070` `0077` `0087` `0088` |

## 逐条清单

「内容」列摘自上游补丁 README 的说明（英文原文，未翻译，便于与上游逐条对照）。

| # | 补丁文件 | 来源 | 内容 |
|---|---|---|---|
| 1 | `0002-temporal-upscalers.patch` | 移植自 gears-judgement-recomp | Temporal upscaling (FSR 3.1 / FSR 4 through AMD's signed FidelityFX SDK 2.3 DLLs, DLSS on NVIDIA): jitter, motion vectors from the view-projection in the vertex… |
| 2 | `0003-system-link-trace.patch` | 作者自写 | System Link milestone M0/M1: `system_link_trace` logs every network and session call (`[netlink]`), unimplemented calls return defined errors, `getsockopt` works… |
| 3 | `0004-texture-packs.patch` | 作者自写 | Texture dump and replacement (`texture_dump`, `texture_replace`, `texture_pack_dir`): content-hashed DDS dumps in `textures/<TITLEID>/dump`, replacements (same size… |
| 4 | `0005-pad-deadzone-and-file-overrides.patch` | 作者自写 | `GOW2_VFS_OVERRIDES` lets the game read individual files from elsewhere (a cached, edited copy of the player's own Coalesced bundles) without touching the game… |
| 5 | `0006-mnk-mouse-stick-switch.patch` | 作者自写 | `mnk_mouse_stick` (on by default) lets the game stop the keyboard/mouse driver from turning mouse motion into right-stick deflection while it keeps the mouse… |
| 6 | `0007-android-arm64-port.patch` | 作者自写 | The Android arm64 port of the runtime (NDK r29+, API 29). Build: an `android-arm64` platform, no `-march=armv8-a` that would lower the game's `armv8.2-a`, SDL3 as… |
| 7 | `0010-system-link-sessions.patch` | 作者自写 | System Link milestone M2a (`docs/COOP_N_PLAYERS.md`, Stage 1, item 2). Sockets: `sendto`/`recvfrom` addresses in network order (port 1000 no longer leaves as… |
| 8 | `0011-network-crossplay.patch` | 作者自写 | System Link milestone M2b (`docs/COOP_N_PLAYERS.md`, Stage 4 networking), on top of the opt-in 0010. Its network settings do nothing until `net_link_active` is on… |
| 9 | `0012-coop-beacon-gate.patch` | 作者自写 | Co-op beacon gate for 3+ players (`docs/COOP_N_PLAYERS.md`, Stage 4 version gate). The game sets `net_coop_hash` (a new UINT64 cvar, 0 by default) to the combined… |
| 10 | `0013-host-ui-hooks.patch` | 作者自写 | Host UI hooks for the in-game PC Options menu ([docs/PC_OPTIONS.md](../../docs/PC_OPTIONS.md)). `xam_input_block` gives the game a neutral pad and no keystrokes… |
| 11 | `0014-fault-report.patch` | 作者自写 | Crash reports a player can send. On Android, a fault no guest handler claims logs one `FAULT unclaimed` line before it is chained to the previous handler: the… |
| 12 | `0015-old-kernel-fixed-mappings.patch` | 作者自写 | Guest memory on Linux and Android kernels older than 4.17 (Snapdragon 845 phones ship 4.9). `AllocFixed` maps fixed addresses with `MAP_FIXED_NOREPLACE` and relies… |
| 13 | `0016-turnip-no-sparse-by-default.patch` | 作者自写 | Shared memory without sparse binding on Mesa Turnip unless the player sets `vulkan_sparse_shared_memory` (issue #1: a recent Turnip on an Adreno 630 advertises… |
| 14 | `0017-d3d12-stats-log.patch` | 作者自写 | Windows performance counters, off by default: `d3d12_stats_log` logs one `D3D12STATS` line every five seconds with frames, draws, draws skipped while their pipeline… |
| 15 | `0018-gpu-backend-cvar.patch` | 作者自写 | `gpu_backend` selects `any` (the default), `d3d12` or `vulkan` when loading the GPU plugin. The runtime owns the cvar and selection so an existing Windows… |
| 16 | `0019-vulkan-texture-exponent-bias-word.patch` | 作者自写 | The Vulkan shader translator read a texture fetch's result exponent bias (`exp_adjust`, fetch constant word 3 bits 13:18) from word 4, whose bits 12:21 are the LOD… |
| 17 | `0020-vulkan-resolve-write-barrier.patch` | 作者自写 | Shared memory usage `kComputeWrite` (unscaled resolves write guest memory from a compute shader) now includes `VK_ACCESS_SHADER_WRITE_BIT`, so the barrier before a… |
| 18 | `0021-d3d12-host-vsync.patch` | 作者自写 | The D3D12 presenter always presented with sync interval 0 and tearing allowed, so `vsync = true` only paced the guest and the picture still tore on high refresh… |
| 19 | `0022-mnk-keys-switch.patch` | 作者自写 | `mnk_keys` (default true). Off, the keyboard no longer drives the emulated pad buttons and sticks, so a game that reads keys itself does not get them twice; the… |
| 20 | `0023-texture-variants.patch` | 作者自写 | Texture pack variants. `texture_replace_variant` (default empty) names a folder `<TITLEID>/replace_<variant>` whose files win over `replace/` while it is set… |
| 21 | `0024-nvidia-rov-default.patch` | 作者自写 | `render_target_path_d3d12` unset now picks the pixel-shader-interlock (ROV) path on NVIDIA as well as Intel (AMD keeps host render targets; a GPU without ROV… |
| 22 | `0025-pipeline-no-stall.patch` | 作者自写 | Stutter fix. Two places made the command thread wait for a pipeline compile (300 ms and more each): the end of every submission created the queued pipelines itself… |
| 23 | `0026-edram-virtual-tiles.patch` | 作者自写 | Gears of War 2 wider than 1280 (ultrawide, `docs/WIDESCREEN.md`) sets 12-bit EDRAM render target bases for its half-resolution passes (a depth buffer and two colour… |
| 24 | `0027-d3d12-pipeline-library.patch` | 作者自写 | Driver-compiled D3D12 pipelines kept between runs (issue #6): an `ID3D12PipelineLibrary` in `<cache>/shaders/local/<title>.<rov/rtv>.d3d12.plib`, loaded before the… |
| 25 | `0028-vulkan-loader-opener.patch` | 作者自写 | `VulkanInstance::SetLoaderOpener` lets the app open the Vulkan loader itself (issue #17: a custom Turnip driver through libadrenotools on Android); a null handle… |
| 26 | `0029-pad-button-remap.patch` | 作者自写 | Controller button remapping (issue #12): the SDL controller driver keeps the physically held buttons and hands the game `pad_remap::Remap` of them, one string… |
| 27 | `0030-dlss-runtime-flag.patch` | 作者自写 | A runtime built with NVIDIA's DLSS SDK registers `gow2_dlss_runtime`, so the PC Options menu offers DLSS with self-built DLSS DLLs next to the release exe (issue… |
| 28 | `0031-ui-paint-on-demand.patch` | 作者自写 | Outside Android the ImGui drawer asked the presenter to repaint continuously while any dialog existed, and the PC Options menu and the achievement notification… |
| 29 | `0032-gpu-native-skeleton.patch` | 作者自写 | Native renderer, phase 1 (`docs/NATIVE_RENDERER_PHASE1.md`, issue #7): `gpu_native` (off) keeps the command processor reading every packet, so fences, interrupts… |
| 30 | `0033-unclipped-draw-extent-on-cpu.patch` | 作者自写 | `execute_unclipped_draw_vs_on_cpu` back to Xenia's default, `true` (the port had it `false`). Draws without clipping and with the 8192 scissor (Direct3D's clears… |
| 31 | `0034-msaa-fill-at-1x.patch` | 作者自写 | Native renderer branch: `gpu_msaa_fill_at_1x` (off) draws Direct3D's 4x MSAA clears (embedded shaders, constant fills) as 1x draws at twice the size on the same… |
| 32 | `0035-runtime-defaults-for-late-cvars.patch` | 作者自写 | `SetFlagByName` on a cvar that is not registered yet (a GPU plugin cvar, before `rexgpu-xenos.dll` loads) was rejected, and `GetFlagSource` answered "default" for… |
| 33 | `0036-native-surfaces.patch` | 作者自写 | Native renderer branch, phase 2a: `gpu_native_surfaces` (off). Render targets are host surfaces: EDRAM ownership copies between them are dropped except depth to… |
| 34 | `0037-native-resolves.patch` | 作者自写 | Native renderer branch, phase 2b: `gpu_native_resolves` (off; needs `gpu_native_surfaces`). After a resolve fires the texture watches, textures that view the… |
| 35 | `0038-native-resolve-shader.patch` | 作者自写 | Native renderer branch, phase 2b: a compute shader (`src/graphics/shaders/native_resolve.cs.hlsl`, bytecode from `fxc /T cs_5_1`) for the resolve pairs that are not… |
| 36 | `0039-native-wide-surfaces.patch` | 作者自写 | Native renderer branch, phase 3: with `gpu_native_surfaces` the host render target path reads the 12-bit EDRAM bases (`color_base_bit_11` / `depth_base_bit_11`, set… |
| 37 | `0040-native-resolves-vulkan.patch` | 作者自写 | Native renderer branch: `gpu_native_resolves` on Vulkan. `vkCmdCopyImage` for the bit-compatible pairs and `native_resolve.cs.glsl` (three SPIR-V variants, one per… |
| 38 | `0041-native-resolves-scaled.patch` | 作者自写 | Native renderer branch: native resolves at resolution scale above 1 (D3D12 and Vulkan). The resolve shaders' half-pixel fill is reproduced: destination host pixel… |
| 39 | `0042-native-resolve-hooks.patch` | 作者自写 | Native renderer branch, phase 5: a hook point after each native resolve (D3D12; `D3D12TextureCache::ApplyNativeResolveHooks`, with the destination address, format… |
| 40 | `0043-native-depth-resolve-scale.patch` | 作者自写 | Native renderer branch: native resolves of D24FS8 depth into `k_24_8_FLOAT` textures double the value. The host render target keeps guest float24 depth [0, 2) as… |
| 41 | `0044-native-resolves-packed-mips.patch` | 作者自写 | Native renderer branch: native resolves also write single-level textures flagged with packed mips when the base level is outside the mip tail (wider and taller than… |
| 42 | `0045-native-resolves-16-16-float-fill.patch` | 作者自写 | Native renderer branch: the `k_16_16_FLOAT` pair goes through `native_resolve` when the resolve has an exponent bias or a half-pixel fill (resolution scaling), so… |
| 43 | `0046-local-split-screen-users.patch` | 作者自写 | Split-screen, first step (issue #16): `local_users` (1-4, default 1) signs in local players 2-4 with profiles derived from the primary one (XUID + index, gamertag… |
| 44 | `0047-xgi-contexts-properties-trace.patch` | 作者自写 | System Link versus/Horde groundwork (issue #9): XGI `SetContext` / `SetProperty` keep their values per user (`GetContext` returns them), session details report game… |
| 45 | `0048-input-pad-first-user.patch` | 作者自写 | Split-screen with keyboard and one controller (issue #16): `input_pad_first_user` (0-3, default 0) shifts real controllers to start at that guest user, so with 1… |
| 46 | `0049-xlivebase-mute-list-query.patch` | 作者自写 | Horde over System Link (issue #9): XLiveBase message `0x0005800E` (`XUserMuteListQuery`) answers success, as Xenia Canary does. Gears of War 2 sends it for every… |
| 47 | `0050-xuser-read-stats-empty-views.patch` | 作者自写 | Horde over System Link (issue #9): `XUserReadStats` writes a valid empty result (one view per requested spec, with its view id and no rows) instead of leaving the… |
| 48 | `0051-shader-storage-responsive-window.patch` | 移植自 @Flippage GoW3 项目 | First start after a graphics change (suggested by @Flippage, issue #18): when the blocking shader storage load runs on the UI thread, it now waits in 100 ms slices… |
| 49 | `0052-vulkan-android-pass-timing.patch` | 作者自写 | Diagnostic, off by default (`vulkan_android_pass_timing`): GPU time per Vulkan render pass with timestamps written outside each pass (valid on tiling GPUs), read… |
| 50 | `0053-vulkan-android-barrier-sources.patch` | 作者自写 | Diagnostic, with 0052's `vulkan_android_pass_timing`: every `PushBufferMemoryBarrier`/`PushImageMemoryBarrier` site tags its source (upload, index, texture, rt… |
| 51 | `0054-upload-whole-blocks.patch` | 作者自写 | `gpu_upload_whole_blocks` (on by default on Android, off elsewhere): when a draw needs guest memory uploaded, also upload the pages of each 256 KB block it touches… |
| 52 | `0055-vulkan-android-dispatch-sources.patch` | 作者自写 | Diagnostic, with 0052's `vulkan_android_pass_timing`: every compute dispatch site tags its source (swap, resolve, texture load, render target transfer, clear, depth… |
| 53 | `0056-native-frontend-draws.patch` | 作者自写 | Full native renderer, step S2a (`docs/superpowers/specs/2026-10-23-full-native-renderer-design.md`): a type-3 NOP carrying `'NATV'` and a host pointer to a… |
| 54 | `0057-native-frontend-shaders-and-groups.patch` | 作者自写 | Full native renderer, steps S2b and S2c: snapshot `'NDS2'` adds registers 0x2080, 0x2280-0x2294, 0x2300-0x2325, 0x2380-0x239F and the BOOL/LOOP constants… |
| 55 | `0058-native-frontend-all-draws.patch` | 作者自写 | Full native renderer, step S3: snapshot `'NDS3'` records the draw opcode (DRAW_INDX or DRAW_INDX_2) and its payload length; the `'NATV'` marker may span more than 4… |
| 56 | `0059-vertex-buffer-request-every-frame.patch` | 移植自 @Flippage GoW3 项目 | Issue #20, found and fixed by @Flippage in the Gears of War 3 recomp: `IssueDraw` (D3D12 and Vulkan) no longer skips requesting a vertex buffer whose fetch constant… |
| 57 | `0060-pad-activity.patch` | 移植自 @Flippage GoW3 项目 | Last physical controller activity for button prompts that follow the device the player used last (issue #25). The SDL and XInput drivers note the host uptime and… |
| 58 | `0061-vfs-overrides-file.patch` | 作者自写 | `GOW2_VFS_OVERRIDES_FILE`: the file-override table of 0005 also reads a UTF-8 file of `guest=host` lines (blank lines and `#` comments ignored; the variable wins on… |
| 59 | `0062-vulkan-without-vertex-stores.patch` | 作者自写 | Issue #27: the Vulkan backend starts on GPUs without the `vertexPipelineStoresAndAtomics` feature (Mali, e.g. Mali-G720), which `VulkanDevice::CreateIfSupported`… |
| 60 | `0063-vulkan-device-pick.patch` | 作者自写 | Issue #34: Vulkan on Windows picked physical device 0, the integrated GPU, on a PC with a dedicated one too (both scored the same optional features).… |
| 61 | `0065-shader-storage-log.patch` | 作者自写 | Logs the shader and pipeline storage loaded at startup and cumulative shader translations and pipeline creations during play. |
| 62 | `0066-native-frontend-bulk-apply.patch` | 作者自写 | Native front end draws apply their snapshot in bulk: the 0x2000-0x23FF groups (no backend hooks them) are copied straight into the register file, and fetch slots… |
| 63 | `0067-native-frontend-timing.patch` | 作者自写 | `gpu_native_frontend_timing` (off by default): times each native draw on the command thread (snapshot apply, shader binding, draw issue) and logs the averages per… |
| 64 | `0068-few-cores-warning-once.patch` | 作者自写 | On a CPU with fewer than 6 logical cores the "Too few processor cores" warning is logged once instead of on every thread core change (issue #37 had thousands of… |
| 65 | `0069-upscale-depth-view-filter.patch` | 作者自写 | The temporal upscaler (FSR 3.1, FSR 4, DLSS) only takes a plain 2D R32_FLOAT texture as the scene depth, and skips frames whose colour is not a plain 2D texture.… |
| 66 | `0070-bulk-register-writes.patch` | 作者自写 | Command-stream register writes in bulk (performance plan item 2): ranges inside 0x2000-0x23FF, which no base or backend hook acts on, are copied straight into the… |
| 67 | `0071-hdr10-screenshots.patch` | 作者自写 | HDR10 output and F12 screenshots in the D3D12 presenter. `present_hdr = true` (needs a restart and the display in HDR mode) creates an R10G10B10A2 swap chain in the… |
| 68 | `0072-fsr3-frame-generation.patch` | 作者自写 | FSR 3 frame generation through the FidelityFX API. `gow2_frame_generation = "fsr3"` (needs a restart and a temporal upscaler: FSR 3.1 or FSR 4) makes the D3D12… |
| 69 | `0073-texture-packs-stack-skip-ui.patch` | 作者自写 | Texture packs for mods. `texture_pack_dir` takes several folders separated by `;` (a single folder works as before); each resolves to its own… |
| 70 | `0074-frame-generation-auto-provider.patch` | 作者自写 | Frame generation picks the newest provider the driver offers for the GPU (the version query passes the device): ML-based FSR 4 frame generation where it is… |
| 71 | `0075-linux-desktop.patch` | 作者自写 | Native Linux build (`scripts/build-linux.sh`, `docs/LINUX.md`): the host stages `librexruntime.so` beside itself as on macOS, and the Android input-system lock and… |
| 72 | `0076-linux-renderdoc-opt-in.patch` | 作者自写 | Desktop Linux only: RenderDoc is used only when it is already in the process (the game was started from RenderDoc; `dlopen` with `RTLD_NOLOAD`), unless… |
| 73 | `0077-live-present-sharpness.patch` | 作者自写 | The presenter re-reads `present_cas_additional_sharpness`, `present_fsr_sharpness_reduction` and `present_dither` when they change (cvar change callbacks, applied… |
| 74 | `0078-posix-event-waits.patch` | 作者自写 | Linux, Android and macOS: multi-handle waits (`WaitAny`/`WaitAll`) and alertable waits and sleeps block until a handle they wait on is signaled or an APC is queued… |
| 75 | `0079-timer-queue-sleeps.patch` | 作者自写 | All platforms: the timer queue's dispatch thread (guest timers, `HighResolutionTimer`) sleeps on a condition variable until the earliest timer is due or a new one… |
| 76 | `0080-vblank-sleep-to-next.patch` | 作者自写 | Linux, Android and macOS: the guest vblank thread sleeps until the next vblank (at least 1 ms) instead of 1 ms at a time: 60 wakeups a second instead of 1000 with… |
| 77 | `0081-linux-small-code-model.patch` | 作者自写 | Linux x86_64: the SDK and the game build with the small code model like every other target, without `-mcmodel=large` and `--no-relax`. The small model reaches ±2 GB… |
| 78 | `0082-vulkan-texture-descriptor-reuse.patch` | 作者自写 | Vulkan: a draw keeps the texture descriptor set of a stage when it binds the same image views and samplers as the set written last in this frame, instead of writing… |
| 79 | `0083-sdk-bsymbolic-non-weak-functions.patch` | 作者自写 | Linux and Android: the SDK's shared libraries link with `-Wl,-Bsymbolic-non-weak-functions` when the linker takes it (lld), so their calls to their own functions… |
| 80 | `0084-posix-unlink-guest-memory-at-open.patch` | 作者自写 | Linux and macOS: the shared-memory object that backs guest memory loses its name in `/dev/shm` right after `shm_open`, instead of when the emulator closes it. The… |
| 81 | `0085-android-surface-lifecycle.patch` | 作者自写 | Android: the game survives leaving the screen and coming back (home button, another app, a notification, the launch options). Android destroys the window's surface… |
| 82 | `0086-posix-write-watch-fast-path.patch` | 作者自写 | Linux and Android: a guest write to a watched page (guest memory the GPU caches, such as textures and vertex buffers) goes straight to the write watches. The fault… |
| 83 | `0087-command-processor-draw-allocations.patch` | 作者自写 | All platforms: two costs the GPU command thread paid on every draw. The Vulkan and D3D12 `IssueDraw` asked `IsReadbackMemexportEnabled` before checking whether the… |
| 84 | `0088-read-pointer-writeback-blocks.patch` | 作者自写 | All platforms, off by default: `gpu_read_pointer_writeback_blocks = true` writes the ring buffer's read pointer back to the guest after every block of packets the… |
| 85 | `0089-vulkan-tight-render-area.patch` | 作者自写 | Vulkan (Android, Linux, and Windows with `gpu_backend = vulkan`): each render pass begins with a render area that covers only what it can write, the union of the… |
| 86 | `0090-fidelityfx-deadlock-and-context-retirement.patch` | 作者自写 | Windows (D3D12 with FidelityFX): fixes two crashes with FSR and frame generation. First, `FrameGeneration::Dispatch` no longer locks `mutex_`. The FidelityFX frame… |
| 87 | `0091-vulkan-tessellation.patch` | 作者自写 | Vulkan (Linux, Proton with `gpu_backend = vulkan`, Windows with `gpu_backend = vulkan`): the tessellated draws (the Riftworm's flesh in act 5, quad patches with… |
| 88 | `0092-android-stereo-fold.patch` | 作者自写 | Android: the SDL audio driver folds the guest 5.1 mix to stereo itself (ITU-R BS.775 `StereoFold` weights) instead of submitting 6 channels. Android's default playback… |
| 89 | `0093-drop-duplicate-render-scale-cvar.patch` | 作者自写 | Removes the SDK's `gow2_render_scale` (a double from 0002 that nothing read). The game defines its own since 0.4.7, and the second registration of the same name was… |
| 90 | `0094-native-transfer-policy-and-screenshot-request.patch` | 作者自写 | Two live debug knobs. `gpu_native_keep_transfers` (string, `default`) chooses which EDRAM ownership transfers survive in native surfaces mode (`all`, `none`, or a… |
| 91 | `0095-native-keep-depth-to-16-16.patch` | 作者自写 | Native surfaces keep the EDRAM ownership transfers from depth to `k_16_16` colour too (issue #26: motion blur smeared objects that move with the camera). Found with… |
| 92 | `0096-adaptive-texture-precache.patch` | 作者自写 | Opt-in bounded history-based D3D12 replacement-texture preload, source revalidation and on-demand fallback. See `docs/TEXTURE_PRECACHE.md`. |
| 93 | `0097-menu-escape-back.patch` | optimistic-blip（PR #59） | Escape works as B (back) in menus and the pause menu while the keyboard drives the emulated pad; during gameplay Escape stays pause. `gow2_menu_escape_back`… |
| 94 | `0099-d3d12-fence-failure-handling.patch` | 作者自写 | Reject removed-device fence sentinel before reclamation; check Signal/event/wait results and propagate failure to draw/readback callers. |
| 95 | `0100-vulkan-storage-buffer-range-clamp.patch` | 作者自写 | Debug cvar `vulkan_max_storage_buffer_range_mb` (0 = off): lowers the device's `maxStorageBufferRange` so a PC GPU takes the four-binding shared memory path that… |

`0098` 号不存在，上游跳号；`0008` `0009` `0064` 同样从未出现。
