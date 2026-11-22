# F4SR

**English** | [简体中文](README_CN.md)

Super resolution and frame generation for Fallout 4, as an F4SE plugin.

**Download:** [Nexus Mods](https://www.nexusmods.com/fallout4/mods/111754) · [GitHub Releases](https://github.com/carrotciallo/f4sr/releases)

F4SR replaces the game's TAA with a modern temporal upscaler and can add generated frames on top:

- **Upscaling:** AMD FSR 3.1, AMD FSR 4.1, or Intel XeSS. Each has four render-resolution presets, from Native (AA only) down to Performance (50%).
- **Frame generation:** FSR-FG (2X) or XeSS-FG.
- **ENB compatible.** The image is upscaled before ENB's post-processing, so ENB still runs at full output resolution.
- **Menu-safe.** Terminals, the Pip-Boy, VATS and other menus render correctly at reduced render resolutions.
- **Reactive mask.** Particles, fire, glass and water are not smeared by temporal accumulation.
- **In-game settings** through MCM. Most options apply live.

> Status: **release candidate**. It is tested on one configuration only (see [Tested on](#tested-on)). Bug reports with `f4sr.log` attached are very welcome.

## Requirements

| | |
|---|---|
| Game | Fallout 4 **1.10.163** (the pre-"next-gen" build). Later builds (1.10.984, 1.11.x) are **not** supported. |
| Script extender | [F4SE](https://f4se.silverlock.org/) 0.6.23 |
| Address Library | [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327) (the 1.10.163 file) |
| MCM (optional) | [Mod Configuration Menu](https://www.nexusmods.com/fallout4/mods/21497), for in-game settings |
| GPU | Any GPU with D3D12 support. FSR 4.1 is heaviest; on RDNA3 it runs as INT8. FSR 3.1 is lightest. |
| OS | Windows 10/11, or Linux with Proton |

## Installation

1. Install with a mod manager, or copy the `Data` folder into the game folder.
2. Copy `fmt.dll` and `spdlog.dll` next to `Fallout4.exe`.
3. Start the game through F4SE.
4. Open **MCM → F4SR**, or edit `Data/F4SE/Plugins/f4sr.ini`.

Disable other upscaler or frame-generation mods for Fallout 4 first. Two plugins hooking the same render path will conflict.

## Settings

The MCM page covers everything most players need:

| Setting | What it does |
|---|---|
| Upscaling | Master switch. |
| Upscaler | FSR 3.1 / FSR 4.1 / XeSS. |
| Render Resolution | Native (AA only), Quality (67%), Balanced (59%), Performance (50%). |
| Sharpening | FSR's RCAS sharpening. Keep it low if ENB already sharpens. |
| Transparency Mask | Reactive mask for transparent effects. Recommended on. Needs a restart. |
| Frame Generation | Master switch. Best on a variable-refresh (FreeSync / G-Sync) display. |
| Frame Generation Engine | FSR-FG or XeSS-FG. Needs a restart. |
| XeSS-FG Multiplier | 2X / 3X / 4X. |
| Allow Lower Render Resolutions with ENB | Allows Quality, Balanced and Performance while ENB is installed. |
| Status Panel | Small top-left panel showing: upscaler, render → output size, upscaler GPU time, mask, and FG real → shown rate. |

Precedence: values saved by MCM (`Data/MCM/Settings/f4sr.ini`) override `Data/F4SE/Plugins/f4sr.ini`. The plugin ini also holds a few advanced and troubleshooting keys, and each one is documented in the file.

### Choosing a frame-generation multiplier

Pick the multiplier where *real FPS × multiplier* is close to your refresh rate. Going higher adds latency without adding visible frames. On a 165 Hz display, for example, 41 real FPS suits 4X and 55 real FPS suits 3X.

## ENB notes

- ENB's post-processing runs on the upscaled image. Effects that depend on scene depth get a depth buffer remapped to the output resolution.
- At reduced render resolutions, ENB's own sharpening can make fine edges shimmer slightly. Lower ENB's sharpening, or set F4SR's Sharpening to 0, if this bothers you.

## Known issues

- Only Fallout 4 1.10.163 is supported.
- Fine-edge shimmer at reduced render resolutions with ENB sharpening, as described under ENB notes.
- Menus other than terminals were checked less thoroughly at reduced render resolutions: power-armor HUD, workbench and VATS. If a translucent rectangle appears in the top-left corner of the screen, please report which menu showed it.
- Frame generation adds input latency, as all frame generation does.

## Troubleshooting

- **Log file:** `Documents/My Games/Fallout4/F4SE/f4sr.log`. Attach it to any bug report.
- **Ghosting or double images:** turn on *Force History Reset* in MCM. If the ghosting disappears, the problem is in the motion vectors. Please report it with the log.
- **Nothing happens:** check the log for `frame pipeline ready`. Check that F4SE and the Address Library match the 1.10.163 build.
- **Status panel shows "not dispatching":** the upscaler is not running. The log names the reason.

## Tested on

- Fallout 4 1.10.163, F4SE 0.6.23, with ENB
- AMD Radeon RX 7900 XT, 3840×2160 at 165 Hz, VRR
- Linux, Proton, gamescope

Reports from Windows, NVIDIA and Intel GPUs, and other resolutions are especially welcome.

## Building

- **Toolchain:** clang-cl and lld-link with the MSVC CRT and Windows SDK, plus vcpkg (`x64-windows-clang` triplet).
- **Dependencies:** CommonLibF4, the FidelityFX SDK and the XeSS SDK under `sdk/`.
- **Commands:**
  ```sh
  . tools/env.sh
  cmake --build build --target f4sr
  tools/package.sh   # assembles package/
  ```

## Credits

- [Community Shaders](https://github.com/doodlum/skyrim-community-shaders) and doodlum's Fallout 4 work. F4SR's render-resolution proxy targets and several hook anchors are ported from them.
- CommonLibF4 and the F4SE team.
- AMD FidelityFX SDK (FSR 3.1, FSR 4.1, FSR frame generation).
- Intel XeSS SDK (XeSS super resolution, XeSS frame generation, XeLL).

## License

F4SR is licensed under the **GNU General Public License v3.0** (see [LICENSE](LICENSE)).

Third-party components ship under their own licenses, reproduced in `licenses/`:

- AMD FidelityFX SDK runtime DLLs: AMD license (binary redistribution, no modification).
- Intel XeSS SDK DLLs: Intel Simplified Software License (binary redistribution, no modification).
- fmt and spdlog: MIT.
