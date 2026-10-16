# Colosseum Native: plan

Target: Pokémon Colosseum, USA (`GC6E01`, main.dol SHA-1 `870e8b96…458f`),
running natively on Windows (x86-64) and Android (arm64-v8a), with the player's
own disc image picked at first launch. Nothing from the game is in this repository.

## How it is built

| Layer | What | Source |
|---|---|---|
| Game code | main.dol recompiled ahead of time to C, then compiled per platform | DolRecomp |
| Runtime | memory, OS, DVD, timing, audio, input, GX graphics | ModernGekko (Dolphin video core) |
| App | window, F1 settings menu, hotkeys, save states, disc picker, touch controls | ported from mz-native |
| Android | SDL2 activity + system file picker + Gradle/CI | ported from mz-native |

First recompile (2026-10-16): 623,456 instructions, 0 unknown opcodes, all code in
main.dol (no REL modules), 106 possible self-modifying-code sites to review.

## Milestones

1. **Boot (Linux, headless):** runtime + recompiled code reach the title screen; frame count verified.
2. **Windows build:** single-file exe, Vulkan / D3D12 / D3D11, F1 menu, disc picker, `ColosseumNative.ini`.
3. **Graphics options** (below) wired to the menu with presets: Original, Balanced, High, Ultra.
4. **144 fps:** decouple rendering from the 30/60 Hz game tick and interpolate (below).
5. **Android build:** arm64 build of the same code, Vulkan, touch controls, APK from GitHub Actions.
6. **Full-game validation:** story, Mt. Battle, saves (memory card file), Snag Machine, battles, movies.

## Graphics options

| Group | Options |
|---|---|
| Display | windowed / borderless / exclusive; renderer (Vulkan, D3D12, D3D11, OpenGL); VSync; HDR |
| Resolution | internal resolution auto or 1×–8× (6× = 4K); output resampling |
| Aspect | 4:3, 16:9, 21:9, 32:9, stretch: game camera FOV widened, HUD kept in place |
| Anti-aliasing | MSAA 2×/4×/8×, SSAA |
| Textures | anisotropic 16×, texture packs (`Load/Textures/GC6E01`), true colour, mipmap detection |
| Lighting | per-pixel lighting, fog toggle, higher-resolution shadows |
| Post-processing | shader slot, sharpening, colour/gamma correction, brightness/contrast/saturation |
| Performance | async shader compilation with ubershaders (no stutter), shader precompile |
| Frame rate | target 30 / 60 / 120 / 144 / 165 / 240 / display; interpolated frames on/off |

Most of these map directly to Dolphin's VideoConfig, which ModernGekko embeds
(`iEFBScale`, `iMultisamples`, `bSSAA`, `iMaxAnisotropy`, `bHiresTextures`,
`bEnablePixelLighting`, `bDisableFog`, `bWidescreenHack`, `sPostProcessingShader`,
`bHDR`, `iShaderCompilationMode`, …). Widescreen without stretching and higher-
resolution shadows need patches to the recompiled game code (camera projection,
HUD placement, shadow render-target size).

## 144 fps approach

The game's logic must keep running at its original tick or everything speeds up.
To present 144 new frames per second:

1. Measure the game's tick and find its frame-advance routine in the recompiled code.
2. Keep logic at the original rate; on each display refresh, render the scene with
   camera, model, bone and particle transforms interpolated between the last two
   logic ticks (the method used by the N64 recomp ports' high-frame-rate modes).
3. Patch anything that assumes one draw per tick (UI counters, animated textures).
4. "Frame generation off" keeps the original behaviour (repeat frames) as a fallback.

## Delivery

The app ships no game data. On first launch the player picks their Colosseum
disc image (ISO or CISO); the app checks `GC6E01` and the main.dol hash and reads
everything else from it. DolRecomp is GPL-3.0, so this project is GPL-3.0 as well.
