# F4SR

[English](README.md) | **简体中文**

为《辐射 4》提供超分辨率与帧生成的 F4SE 插件。

F4SR 用现代时域超分替换游戏自带的 TAA，并可在此之上叠加生成帧：

- **超分：** AMD FSR 3.1、AMD FSR 4.1 或 Intel XeSS。每种都有四档渲染分辨率预设，从原生（仅抗锯齿）到性能（50%）。
- **帧生成：** FSR-FG（2X）或 XeSS-FG。
- **兼容 ENB。** 画面在 ENB 后处理之前完成超分，ENB 仍在完整输出分辨率下运行。
- **菜单安全。** 终端、哔哔小子、VATS 等菜单在降低渲染分辨率时也能正确显示。
- **反应遮罩（Reactive mask）。** 粒子、火焰、玻璃和水面不会被时域累积涂抹。
- **游戏内设置** 通过 MCM 调整，多数选项即时生效。

> 状态：**发布候选（release candidate）**。目前仅在一个配置上测试过（见[测试环境](#测试环境)）。欢迎附带 `f4sr.log` 提交 bug 报告。

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

从 RC1 升级时，替换旧 F4SR 模组，保留 `Data/MCM/Settings/f4sr.ini`。新版不再附带散装 `fmt.dll` 或 `spdlog.dll`。仅当确认 `Fallout4.exe` 旁的文件来自 RC1、且没有其他插件使用时，再清理它们。

先禁用其他《辐射 4》超分或帧生成模组。两个插件挂同一条渲染路径会冲突。

## 设置

MCM 页面覆盖了大多数玩家需要的选项：

| 设置 | 作用 |
|---|---|
| 超分 | 总开关。 |
| 超分引擎 | FSR 3.1 / FSR 4.1 / XeSS。 |
| 渲染分辨率 | 原生（仅抗锯齿）、质量（67%）、平衡（59%）、性能（50%）。 |
| 锐化 | FSR 的 RCAS 锐化。若 ENB 已做锐化，请调低。 |
| 透明反应遮罩 | 针对透明特效的反应遮罩，建议开启。需要重启。 |
| 帧生成 | 总开关。在可变刷新率（FreeSync / G-Sync）显示器上效果最好。 |
| 帧生成引擎 | FSR-FG 或 XeSS-FG。需要重启。 |
| XeSS-FG 倍率 | 2X / 3X / 4X。 |
| 允许 ENB 下使用更低渲染分辨率 | 装有 ENB 时允许质量、平衡和性能档。 |
| 状态面板 | 左上角小面板，显示：超分引擎、渲染 → 输出尺寸、超分 GPU 耗时、遮罩状态、帧生成真实 → 显示帧率。 |

优先级：MCM 保存的值（`Data/MCM/Settings/f4sr.ini`）覆盖 `Data/F4SE/Plugins/f4sr.ini`。插件 ini 里还有少数高级与排障选项，每项都在文件内有说明。

### 如何选帧生成倍率

选择让 *真实帧率 × 倍率* 接近显示器刷新率的倍率。再往上只会增加延迟，不会增加可见帧。例如在 165 Hz 显示器上，41 的真实帧率适合 4X，55 适合 3X。

## ENB 说明

- ENB 的后处理在超分后的画面上运行。依赖场景深度的效果会拿到一份重映射到输出分辨率的深度缓冲。
- 在较低渲染分辨率下，ENB 自带的锐化可能让细边缘轻微闪烁。若介意，可调低 ENB 锐化，或把 F4SR 的锐化设为 0。

## 已知问题

- 仅支持《辐射 4》1.10.163。
- 如 ENB 说明所述，低渲染分辨率加 ENB 锐化时细边缘闪烁。
- 除终端外的菜单在低渲染分辨率下检查得较少：动力装甲 HUD、工作台和 VATS。若屏幕左上角出现半透明矩形，请报告是哪个菜单。
- 帧生成会增加输入延迟，所有帧生成技术都如此。

## 排障

- **日志文件：** `Documents/My Games/Fallout4/F4SE/f4sr.log`。提交 bug 报告时请附带。
- **重影或双影：** 在 MCM 里开启 *Force History Reset*。若重影消失，问题出在运动矢量上。请附日志报告。
- **完全没效果：** 在日志里找 `frame pipeline ready`。确认 F4SE 和地址库与 1.10.163 版本匹配。
- **状态面板显示 "not dispatching"：** 超分没有在运行，日志里写了原因。

## 测试环境

- 《辐射 4》1.10.163，F4SE 0.6.23，装有 ENB
- AMD Radeon RX 7900 XT，3840×2160 @ 165 Hz，VRR
- Linux、Proton、gamescope

特别欢迎来自 Windows、NVIDIA 与 Intel GPU 以及其他分辨率的报告。

## 构建

- **工具链：** clang-cl 与 lld-link，配合 MSVC CRT、Windows SDK 和 vcpkg（发行包使用 `x64-windows-clang-staticdeps`：静态链接 fmt/spdlog，保留动态 CRT）。打包需要 `llvm-readobj`、`zip` 和 `unzip`。
- **依赖：** `third_party/` 下的 CommonLibF4，以及 `sdk/` 下的 FidelityFX SDK、XeSS SDK。
- **命令：**
  ```sh
  tools/build-f4sr.sh --staticdeps
  tools/package.sh build-sf dist/f4sr-v1.0.0-rc.2.zip
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
