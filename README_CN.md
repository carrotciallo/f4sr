# F4SR

[English](README.md) | **简体中文**

为《辐射 4》提供超分辨率与帧生成的 F4SE 插件。

F4SR 用现代时域超分替换游戏自带的 TAA，并可在此之上叠加生成帧：

- **超分：** AMD FSR 3.1、AMD FSR 4.1 或 Intel XeSS。每种都有四档渲染分辨率预设，从原生（仅抗锯齿）到性能（50%）。
- **帧生成：** XeSS-FG（2X–4X，默认）或 FSR-FG（2X）。
- **兼容 ENB。** 使用 XeSS-FG（默认）时，游戏和 ENB 以较低的渲染分辨率运行，UI 以输出分辨率绘制（“UI Native”管线）。旧的 “Classic” 管线先超分再运行 ENB。
- **兼容 ReShade。** 效果可以和 ENB 一起在超分之前、HUD 之下运行，并能拿到游戏深度。
- **菜单安全。** 终端、哔哔小子、VATS 等菜单在降低渲染分辨率时也能正确显示。
- **反应遮罩（Reactive mask）。** 粒子、火焰、玻璃和水面不会被时域累积涂抹。
- **游戏内设置** 通过 MCM 调整，多数选项即时生效。

> 状态：**1.1.0-rc.1，推荐版本。** 包含 UI Native 管线（默认）、ReShade 超分前渲染，以及 1.1.0 各 alpha 版的修复。作为候选发布版，如果没有发现新问题，它就会成为 1.1.0。上一版 1.0.0-rc.4 仍可在 Nexus Mods 的 *Old files* 和 GitHub 的 `v1.0.0-rc.4` 发布中下载。F4SR 在 Linux 上开发，Windows 上的结果来自玩家反馈（见[测试环境](#测试环境)）。欢迎附带 `f4sr.log` 提交 bug 报告。

## 环境要求

| | |
|---|---|
| 游戏 | 《辐射 4》**1.10.163**（"次世代"更新之前的版本）。更高版本（1.10.984、1.11.x）**不受支持**。 |
| 脚本扩展 | [F4SE](https://f4se.silverlock.org/) 0.6.23 |
| 地址库 | [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327)（1.10.163 对应文件） |
| MCM（可选） | [Mod Configuration Menu](https://www.nexusmods.com/fallout4/mods/21497)，用于游戏内设置 |
| GPU | 任何支持 D3D12 的 GPU。FSR 4.1 负载最高，在 RDNA3 上以 INT8 方式运行；FSR 3.1 最轻。 |
| 系统 | Windows 10/11，或 Linux + Proton |
| 运行库 | 最新的 [Microsoft Visual C++ v14 Redistributable（x64）](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)；若 Proton 环境缺少运行库，装入游戏自己的 prefix |

## 安装

1. 在 MO2 中直接安装 ZIP。包根已包含 `F4SE/` 和 `MCM/`，无需 Root Builder。手动安装时，把这两个文件夹合并到游戏的 `Data/`。
2. 通过 F4SE 启动游戏。
3. 打开 **MCM → F4SR**，或编辑 `Data/F4SE/Plugins/f4sr.ini`。

从旧版本升级时，替换旧 F4SR 模组（自 rc.2 起文件结构未变），保留 `Data/MCM/Settings/f4sr.ini`。从 1.0.0-rc.4 升级时，“渲染管线”选项默认为 UI Native；想保持 1.0 的行为，在 MCM 里选 Classic。想退回 1.0.0-rc.4，从 *Old files* 安装即可。RC2 起不再附带散装 `fmt.dll` 或 `spdlog.dll`；仅当确认 `Fallout4.exe` 旁的文件来自 RC1、且没有其他插件使用时，再清理它们。

先禁用其他《辐射 4》超分或帧生成模组。两个插件挂同一条渲染路径会冲突。

## 设置

MCM 页面覆盖了大多数玩家需要的选项：

| 设置 | 作用 |
|---|---|
| 超分 | 总开关。 |
| 渲染管线 | UI Native（默认）或 Classic。见[渲染管线](#渲染管线)。仅在选择 XeSS-FG 时显示；FSR-FG 始终使用 Classic。需要重启。 |
| 超分引擎 | FSR 3.1 / FSR 4.1 / XeSS。 |
| 渲染分辨率 | 原生（仅抗锯齿）、质量（67%）、平衡（59%）、性能（50%）。默认为平衡。UI Native 下关闭菜单、回到游戏后应用；不支持热切换的配置仍需重启。 |
| 锐化 | FSR 的 RCAS 锐化。若 ENB 已做锐化，请调低。 |
| 透明反应遮罩 | 针对透明特效的反应遮罩，建议开启。需要重启。 |
| 帧生成 | 总开关。在可变刷新率（FreeSync / G-Sync）显示器上效果最好。使用 XeSS-FG 时，第一次开启需要重启游戏才生效，之后可即时开关。 |
| 帧生成引擎 | XeSS-FG（默认）或 FSR-FG。需要重启。见[如何选帧生成引擎](#如何选帧生成引擎)。 |
| XeSS-FG 倍率 | 2X（默认）/ 3X / 4X。仅在选择 XeSS-FG 时显示。 |
| ReShade 效果画在 UI 之前 | 让 ReShade 效果画在 HUD 之下，而不是盖在 HUD 上。UI Native 下，效果和 ENB 一起在超分之前运行。见 [ReShade 说明](#reshade-说明)。 |
| ReShade 深度 | 在 XeSS-FG 下，以及 Classic 低于原生档时，把游戏深度提供给 ReShade，供环境光遮蔽、景深、雾等效果使用。默认开启，即时生效。 |
| 状态面板 | 左上角小面板，显示：版本和渲染管线、FPS（开帧生成时为真实帧 → 显示帧）、超分引擎和档位、渲染 → 输出尺寸及超分 GPU 耗时、帧生成状态，以及 ReShade 模式。错误以橙色列在最上方；出现导致功能关闭的错误时，即使关闭此项也会自动显示 30 秒。 |
| 记录系统信息（Log System Info） | 默认开启。在 `f4sr.log` 开头写一段简短的系统概况，方便反馈问题（见[日志文件与隐私](#日志文件与隐私)）。下次启动生效。 |

优先级：MCM 保存的值（`Data/MCM/Settings/f4sr.ini`）覆盖 `Data/F4SE/Plugins/f4sr.ini`。插件 ini 里还有少数高级与排障选项，每项都在文件内有说明。

### 如何选帧生成引擎

XeSS-FG 是默认引擎，也是推荐的引擎。与 FSR-FG 相比：

- **帧间隔更均匀。** 生成帧的间隔更平稳。F4SR 的 FSR-FG 以保守的同步模式提交每对帧，真实帧率上限为刷新率的一半。
- **输入延迟更低**，得益于 Intel XeLL。
- **画面更干净**，运动物体、细小细节和透明物周围的拖影更少。
- **支持 2X 到 4X。** FSR-FG 只有 2X。

使用 UI Native 管线（默认）时，即使关闭帧生成，XeSS-FG 也会接管游戏交换链，所以帧生成可以即时开关。使用 Classic 时，XeSS-FG 只在启动时帧生成已开启的情况下接管交换链；如果在游戏运行中才开启帧生成，需要重启一次。

FSR-FG 作为备用方案保留，例如 XeSS-FG 在你的系统上无法启动时（状态面板和 `f4sr.log` 会写明原因）。FSR-FG 始终使用 Classic 管线。

### 如何选帧生成倍率

选择让 *真实帧率 × 倍率* 接近显示器刷新率的倍率。再往上只会增加延迟，不会增加可见帧。例如在 165 Hz 显示器上，41 的真实帧率适合 4X，55 适合 3X。

固定刷新率的显示设备同样可用。有玩家反馈在 50 Hz 电视上 XeSS-FG 运行良好，而其他帧生成模组在那里无法正常工作；25 的真实帧率开 2X 即可填满 50 Hz。

## 渲染管线

### UI Native（XeSS-FG 下的默认）

- 游戏和 ENB 以渲染分辨率运行。F4SR 先对完成的画面做超分，游戏再以输出分辨率在上面绘制 UI。ENB 设置界面及 `Present` 阶段的后处理会合入输出画面。
- 低于原生档时比 Classic 更快，因为 ENB 和 ReShade 也按渲染分辨率运行。实测（RX 7900 XT，3840×2160 输出，FSR 4.1，ENB 0.501 和 ReShade，帧生成关闭）：质量档 68.6 对 57.8 帧，平衡档 74.3 对 61.5，性能档 81.8 对 64.6。原生档下 Classic 略快（48.8 对 45.3 帧）。

  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/perf-4k-dark.svg">
    <img src="docs/images/perf-4k-light.svg" alt="3840x2160 下的真实帧率：原生 UI Native 45.3 / Classic 48.8；质量 68.6 / 57.8；平衡 74.3 / 61.5；性能 81.8 / 64.6" width="760">
  </picture>

- ENB 的锐化和边缘类效果在超分之前运行，Classic 在低渲染分辨率下可能出现的细边缘闪烁不会出现。
- 超分引擎：FSR 3.1、FSR 4.1 和 XeSS-SR。帧生成开关均可。
- **在 MCM 切换渲染分辨率后，关闭菜单并回到游戏。** 在窗口／无边框模式下，无论使用兼容 ENB 0.501 的交换链还是不装 ENB，游戏（及 ENB）都会在同一进程重建缓冲。切换可能短暂停顿，超分和帧生成随后在数秒内恢复。如果状态面板提示重启，请退出到桌面再通过 F4SE 启动；仅重新读档不够。
- 输出为 3840×2160 时，质量档为 **2560×1440**，平衡档为 **2258×1270**，性能档为 **1920×1080**；原生档为 **3840×2160**，使用所选超分引擎进行抗锯齿。`Fallout4Prefs.ini` 中的 `iSize W` 和 `iSize H` 保持为输出分辨率。
- `bPathBNonLinearColor`（插件 ini，默认开启）告诉超分引擎它读到的画面是伽马编码的；无论是否使用 ENB，SDR 输出都是如此。只有预设输出线性颜色时才需要关闭。
- 独占全屏（`bFull Screen=1`）会以无边框全屏方式运行，因为画面是通过 F4SR 自己在游戏窗口上的交换链输出的。
- **回退：** UI Native 无法启动时（例如 XeSS-FG 无法接管交换链），F4SR 本次运行改用 Classic，把 MCM 选项切换为 Classic，并在状态面板显示错误。排除原因后，在 MCM 重新选择 UI Native 即可再次尝试。

### UI Native 下的 ENB 预设

ENB 现在在超分之前、按渲染分辨率运行。大多数预设看起来没有区别，但有几点不同：

- **ENB 自带的时域抗锯齿**（预设 prepass 或 postpass 着色器里的 TAA）现在会读到超分用的亚像素镜头抖动，可能发糊或拖影。请关闭它，超分本身已经负责抗锯齿。
- **按像素计算尺寸的效果**（锐化半径、胶片颗粒、噪点、部分镜头效果）先按渲染分辨率绘制再被放大，会和 ReShade 效果一样按相同倍数显得更大（见 [ReShade 说明](#ui-native-下的画在-ui-之前)中的表格）。颗粒和噪点经过超分后也可能变柔和或轻微闪烁。
- 已用 ENB 0.501 测试。其他版本应该也能用，但如果画面异常（偏移、尺寸不对、闪烁、ENB 菜单点不动），请把渲染管线切换为 Classic，并附上 `f4sr.log` 和 ENB 版本反馈。
- 不装 ENB 时，UI Native 同样可用，包括即时切换渲染分辨率。

### Classic

- FSR-FG 使用此管线，也可以在 MCM 中手动选择。F4SR 先超分再运行 ENB；ENB 的后处理以输出分辨率在超分后的画面上运行。
- **ENB 下的较低渲染分辨率**（质量、平衡、性能）：F4SR 需要把 ENB 自身的部分绘制调整到较低分辨率。这些绘制按代码特征查找：在 ENB 0.420、0.496 和 0.501 中都能找到（游戏内实测为 0.501），0.307 及更早版本中找不到。找不到时，F4SR 保持原生分辨率（仅抗锯齿，不提升帧率），状态面板和 `f4sr.log` 会写明原因。如果画面反而异常（错位、缩在角落、闪烁），请把渲染分辨率调回原生（仅抗锯齿），并附上 `f4sr.log` 和你的 ENB 版本报告。
- 依赖场景深度的效果会拿到一份重映射到输出分辨率的深度缓冲。
- 在较低渲染分辨率下，ENB 自带的锐化可能让细边缘轻微闪烁。若介意，可调低 ENB 锐化，或把 F4SR 的锐化设为 0。

## ReShade 说明

- ReShade 可与 ENB 共存（ENB 为 `d3d11.dll`，ReShade 为 `dxgi.dll`）。**ReShade 效果画在 UI 之前** 和 **ReShade 深度** 需要带 add-on 支持的 ReShade 版本。
- **不开启“画在 UI 之前”时**，ReShade 效果在 UI 之后作用于输出画面，每显示一帧（包括生成帧）执行一次，GPU 开销随帧生成倍数增加。

### UI Native 下的“画在 UI 之前”

- 效果在 ENB 之后、超分之前，作用于渲染分辨率的画面。HUD 不受影响，超分会为结果抗锯齿，生成帧通过插值得到效果，而不是重新执行。由于按渲染分辨率运行，开销也更低。
- 深度是同一帧、同一分辨率的深度，基于深度的效果能精确对齐。
- **按像素计算尺寸的效果会显得更大**，因为它们先按渲染分辨率绘制，再被放大：贴图叠加（Layer）、边框、ASCII、CRT 扫描线、胶片颗粒等。放大倍数由渲染分辨率档位决定：

  | 档位 | 渲染比例 | 按像素计算的效果显示为 | 尺寸参数乘以 |
  |---|---|---|---|
  | 原生 | 100% | ×1.00 | 1.00 |
  | 质量 | 67% | ×1.50 | 0.67 |
  | 平衡 | 59% | ×1.70 | 0.59 |
  | 性能 | 50% | ×2.00 | 0.50 |

  例如平衡档下，把 Layer 的 `Layer_Scale` 设为原来的 0.59 倍。切换档位后需要重新调整。状态面板和 `f4sr.log` 会显示当前倍数。如果需要像素精确的叠加效果，请关闭“画在 UI 之前”。
- 切换档位，或开关“画在 UI 之前”时，ReShade 会按新尺寸重新加载效果（短暂停顿）。ReShade 界面里的设置照常即时生效。
- 固定在屏幕上的效果（暗角、颗粒、边框）会经过超分。测试中它们保持干净；如发现拖影请反馈。

### Classic 下的“画在 UI 之前”

- 使用 FSR-FG 或不开帧生成时，效果在超分和 ENB 之后、HUD 之前运行。开启帧生成时，固定在屏幕上的效果在运动中可能拖影。
- XeSS-FG 搭配 Classic 时不支持“画在 UI 之前”，效果仍盖在 HUD 之上。请使用 UI Native。

### XeSS-FG 下的限制

- 游戏自身交换链上的 ReShade 实例不会执行 Present。依赖游戏绘制或决定效果渲染时机的插件（ReShade Effect Shader Toggler、REST）不起作用。深度改由 **ReShade 深度** 提供，而不是 ReShade 自己的深度检测。
- **画在 UI 之前** 可以取代 ReShade Effect Shader Toggler 等插件“让效果避开 HUD”的用途，请停用这类插件。
- 不开启“画在 UI 之前”时，生成帧上的深度类效果使用上一个真实帧的深度，快速运动时可能略有滞后。

## High FPS Physics Fix

High FPS Physics Fix（HFPF）可能会改动游戏的分辨率和交换链。和 F4SR 一起使用时，请在 `Data/F4SE/Plugins/HighFPSPhysicsFix.ini` 的 `[Display]` 段中这样设置：

```ini
BorderlessUpscale=true
ResizeBuffersDisable=false
```

`[Display]` 里的其他项保持默认，尤其是 `Resolution` 和 `ResolutionScale` 请保持注释状态。

- 如果有模组把显示器分辨率塞给游戏，F4SR 会把它改回渲染分辨率，状态面板会提示 "another mod changed the game resolution"。
- 如果游戏仍以显示器分辨率渲染，面板会显示 "Render Resolution ignored: game runs at monitor size"。这时超分只起原生抗锯齿的作用，帧率不会提升。请检查上面的 `[Display]` 设置。
- 设置 `ResizeBuffersDisable=true` 时，在 MCM 里修改渲染分辨率需要重启游戏才会生效，面板会给出提示。

`f4sr.log` 会在启动时记录 HFPF 的 `[Display]` 设置。

## 已知问题

- 仅支持《辐射 4》1.10.163。
- 仅 Classic：低渲染分辨率加 ENB 锐化时细边缘闪烁，见 [Classic](#classic)。
- VATS 在低渲染分辨率下检查得比其他菜单少。若屏幕左上角出现半透明矩形，请报告是哪个菜单。
- Windows 显示缩放（DPI）高于 100% 的情况尚未确认。
- 帧生成会增加输入延迟，所有帧生成技术都如此。

## 排障

- **日志文件：** `Documents/My Games/Fallout4/F4SE/f4sr.log`。提交 bug 报告时请附带。每次启动都会覆盖，出问题后请先复制一份再开游戏。
- **重影或双影：** 在 MCM 里开启 *Force History Reset*。若重影消失，问题出在运动矢量上。请附日志报告。
- **完全没效果：** 在日志里找 `frame pipeline ready`。确认 F4SE 和地址库与 1.10.163 版本匹配。
- **状态面板的 Upscale 下显示 "not running"：** 超分没有在运行，日志里写了原因。

### 日志文件与隐私

从 1.1.0-rc.1 起，`f4sr.log` 开头会写一段简短的系统概况，这样分享日志时就能直接回答排查时最先要问的问题：

- F4SR、游戏和 F4SE 的版本；
- Windows 版本（或 Wine/Proton 版本及宿主系统）；
- CPU 型号、核心数和内存大小；
- 每块显卡的厂商/设备 ID、显存和驱动版本；
- 每台显示器的分辨率、刷新率和 HDR 状态；
- `Fallout4Prefs.ini` 中的显示设置；
- `Fallout4.exe` 旁的代理 DLL（ENB、ReShade 等）及其版本，以及 `HighFPSPhysicsFix.ini` 的 `[Display]` 段；
- F4SR 的主要设置。

日志只写在你自己的硬盘上，F4SR 不会向任何地方发送数据。日志不记录用户名和计算机名，所有日志行里的文件夹路径都会被简化：游戏目录写成 `<game>`，用户目录和"文档"目录写成 `<profile>` 和 `<documents>`，其他任何 `\Users\<名字>\` 路径显示为 `<user>`。如果不想记录系统概况，请关闭 **MCM → F4SR → Diagnostics → Log System Info**（或在 `Data/F4SE/Plugins/f4sr.ini` 中设置 `bLogSystemInfo=0`），下次启动生效。

## 测试环境

- 《辐射 4》1.10.163，F4SE 0.6.23，装有 ENB，在以下环境：
  - AMD Radeon RX 7900 XT，3840×2160 @ 165 Hz，VRR，Linux（Proton、gamescope）；有无 ReShade 均测试过。
  - AMD Radeon RX 7600，Windows（用户报告，1.0.0-rc 版本）。
  - NVIDIA GeForce RTX 4060，3840×2160，Windows，XeSS-FG，50 Hz 电视（用户报告，1.1.0-alpha.2）。
  - 3440×1440，Windows，Fallout: London（用户报告，1.1.0-alpha.2）。

在 Windows 上，1.1.0-alpha.1 会让游戏窗口每次启动都变小（它把缩小后的分辨率存进了 `Fallout4Prefs.ini`），之后的版本已修复。用过 alpha.1 的话，请把 `文档\My Games\Fallout4\Fallout4Prefs.ini` 里的 `iSize W` 和 `iSize H` 手动改回你的分辨率一次。

特别欢迎来自 Intel GPU、其他分辨率以及 Windows 显示缩放高于 100% 的反馈。

## 构建

- **工具链：** clang-cl 与 lld-link，配合 MSVC CRT、Windows SDK 和 vcpkg（发行包使用 `x64-windows-clang-staticdeps`：静态链接 fmt/spdlog，保留动态 CRT）。打包需要 `llvm-readobj`、`zip` 和 `unzip`。
- **依赖：** `third_party/` 下的 CommonLibF4，以及 `sdk/` 下的 FidelityFX SDK、XeSS SDK。
- **命令：**
  ```sh
  tools/build-f4sr.sh --staticdeps
  tools/package.sh build-sf dist/f4sr-v1.1.0-rc.1.zip
  ```

## 致谢

- [Community Shaders](https://github.com/doodlum/skyrim-community-shaders) 与 doodlum 的《辐射 4》工作。F4SR 的渲染分辨率代理目标与若干钩子锚点移植自该项目。
- CommonLibF4 与 F4SE 团队。
- AMD FidelityFX SDK（FSR 3.1、FSR 4.1、FSR 帧生成）。
- Intel XeSS SDK（XeSS 超分、XeSS 帧生成、XeLL）。

## 许可证

F4SR 本体采用 **GNU 通用公共许可证 v3.0**（见 [LICENSE](LICENSE)）。

第三方组件按各自许可证随包分发，全文见 `licenses/`：

- AMD FidelityFX SDK 运行时 DLL：AMD 许可证（二进制再分发，禁止修改）。
- Intel XeSS SDK DLL：Intel 简化软件许可证（二进制再分发，禁止修改）。
- fmt 与 spdlog：MIT。
