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
   box, end-of-frame events, and EFB copies: an EFB copy zeroes its destination in guest
   RAM, and by replay time the CPU, a frame ahead, may have reused that memory; test 7/8
   crashed on area loads because of it. The replay samples frame N's copies instead) and its XFB copy goes to a private texture, after which the
   live state is restored. Replaying with t = 1 is pixel-identical to the real frame.
4. The presenter shows the in-between frame at the field's time and the real frame half
   a field later (`ViSwap`), so both get equal screen time: 120 frames per second, with
   half a field (8 ms) of added latency. The in-between frame is matched to the XFB
   address it precedes, because the VI shows each XFB one frame after it was drawn.

Also blended: BP TEV colour registers (screen fades, flashes) and inline vertex data
(2D sprites, particles, CPU-animated meshes). Inline draws pair with the nearest draw
of the same kind in the previous frame, and matrix loads whose in-order partner fits
worse than a nearby one take the closest (a blinking cursor sharing a window frame's
texture shifts the order).

Menu: Frame rate 30 / 60 / 120 (interpolated). 144 Hz displays show it best with
VRR (FreeSync/G-Sync); fixed 144 Hz would need in-between frames at varying t, which
is a later step.

## Performance

Measured on the Linux build, Null renderer, uncapped, from a gameplay save state
(Outskirt Stand), 2-core Xeon:

| Change | Frames per second |
|---|---|
| Baseline | 45.7 |
| No mod-hook checks when no mods are loaded; game ID read every 256 slices | 51 |
| + idle-loop skip at 0x800FEB9C | 75 |

- **Idle loop.** A background thread spins in a polling loop (0x800FEA90 to 0x800FEB9C,
  disable interrupts, walk a list, re-enable) whenever the game waits for the next
  field. `StaticRecompIdlePC = 0x800FEB9C` makes the dispatcher hand the rest of the
  time slice back to CoreTiming at that back-edge, which cuts emulated work by about 4x.
  Gameplay, area loads and the interpolation tests are unchanged with it on.
- **Startup.** The runtime no longer SHA-256s every file on the disc (over 1 GB, twice)
  at each start; nothing in the runtime used that hash.
- **First-run quality** follows the graphics card (DXGI dedicated memory): integrated
  graphics get 2x / no MSAA, mid-range 3x / 2x MSAA, high-end 4x / 4x MSAA. The menu's
  quality presets (Performance, Balanced, High, Ultra) change them later.


## Delivery

The app ships no game data. On first launch the player picks their Colosseum
disc image (ISO or CISO); the app checks `GC6E01` and the main.dol hash and reads
everything else from it. DolRecomp is GPL-3.0, so this project is GPL-3.0 as well.
