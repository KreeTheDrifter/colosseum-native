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

1. ~~**Boot (Linux):** runtime + recompiled code reach the title screen.~~ Done 2026-10-16:
   Genius Sonority logo, intro, title screen; 1,667 frames, 0 interpreter fallbacks,
   0 failed SMC checks. Needs a window (headless + Vulkan trips an ImGui assert).
2. **Windows build:** cross-compiled with llvm-mingw (`scripts/build_windows.sh`): launcher with
   disc picker / drag-and-drop (ISO, GCM, CISO), runtime, game DLL. Vulkan and OpenGL;
   D3D11/12 later. Then the F1 menu and `ColosseumNative.ini`.
3. **Graphics options** (below) wired to the menu with presets: Original, Balanced, High, Ultra.
4. **120 fps:** done, by rendering interpolated in-between frames on the GPU side (below).
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

## Leads from Dolphin's GC6E01 game settings

Dolphin's `Data/Sys/GameSettings/GC6E01.ini` (vendored in ModernGekko) carries:

- **Memory-card save fix** (`0x801CFC2C`, `0x801CFC7C`): baked into main.dol by
  `tools/disc.py patch-dol`, as ModernGekko's port tool does.
- **"16:9 Widescreen"** Action Replay code: a code cave at `0x80005300`, a branch at
  `0x800A3930`, and the aspect constant `0x8047E724 = 4/3·(4/3)`. The code parts can be
  baked into main.dol; the constant lives outside the DOL's data sections and needs a
  runtime write or a patch to the code that loads it.
- **"60 FPS [Nerdzilla]"**: `0x80005D98 = li r3,0`. Colosseum normally runs at 30 fps;
  this is the starting point for high frame rates (game logic at 60, then interpolate
  to 120/144). Must be checked for game-speed side effects.
- `CPUCull = True` for Shadow Pokémon cutscenes (performance).

Recompiled code can't be patched in memory at run time, so code changes go into
main.dol before recompiling; toggles in the settings need either separate code paths
in the recompiled output or runtime checks added by our patches.

## High frame rates (120 fps): GPU-side frame interpolation

Colosseum's subsystems advance by whole video fields (the frame dispatcher passes the
integer field delta from `VIWaitForRetrace` to every update), so the logic tops out at
60 Hz and a VI overclock just stalls it. Instead, the in-between frames are rendered
inside Dolphin's VideoCommon (`patches/dolphin.patch`, `VideoCommon/FrameInterpolation.*`):

1. Every frame's GPU command stream is recorded (display lists inlined), together with
   a snapshot of the GPU state it started from (BP/CP/XF memory, TMEM, shader-manager
   constants) and every XF load (immediate and indexed) with its values.
2. At frame N's XFB copy, frame N is drawn a second time from the recording, with each
   position/normal/texture/post matrix, light position/direction, projection and
   material/ambient colour blended halfway towards frame N-1. Loads are matched between
   frames by address, a signature of the next draw (primitive, vertex count, vertex
   descriptor, position array, texture 0) and occurrence. Unmatched loads (new objects,
   CPU-animated vertices) keep frame N's values; rotations over ~60° per frame and
   scrolling-texture wrap-arounds are rejected; a frame with under 60% of its matrix
   loads matched is a cut and isn't interpolated.
3. The replay's side effects are suppressed (PE tokens, draw-done interrupts, bounding
   box, end-of-frame events) and its XFB copy goes to a private texture, after which the
   live state is restored. Replaying with t = 1 is pixel-identical to the real frame.
4. The presenter shows the in-between frame at the field's time and the real frame half
   a field later (`ViSwap`), so both get equal screen time: 120 frames per second, with
   half a field (8 ms) of added latency. The in-between frame is matched to the XFB
   address it precedes, because the VI shows each XFB one frame after it was drawn.

Menu: Frame rate 30 / 60 / 120 (interpolated). 144 Hz displays show it best with
VRR (FreeSync/G-Sync); fixed 144 Hz would need in-between frames at varying t, which
is a later step. Not interpolated yet: TEV colour fades driven by BP registers.

## Delivery

The app ships no game data. On first launch the player picks their Colosseum
disc image (ISO or CISO); the app checks `GC6E01` and the main.dol hash and reads
everything else from it. DolRecomp is GPL-3.0, so this project is GPL-3.0 as well.
