# `tosu-ingame-overlay`
tosu ingame overlay

Windows: supports OpenGL(stable, lazer), dx9(stable compat), dx11(lazer)

Linux: supports OpenGL and Vulkan (lazer), see [Linux](#linux)

Overlay library used on Windows: [asdf-overlay](https://github.com/storycraft/asdf-overlay)

## Performance
On Windows this projects uses shared gpu surface for rendering overlay, so no cpu work is involved.
It have same or less performance overhead than adding a OBS browser source.
Which is very small and it has no noticeable input latency.

On Linux frames are copied through shared memory (`/dev/shm`) and uploaded to the game's GPU
only when the overlay repaints, so the cost scales with how often overlays change.

## Linux

On Linux the overlay is drawn by `libtosu-overlay.so`, a small library that runs inside osu!lazer.
It cannot be attached to an already running game, so **osu! has to be started through `tosu-overlay-run`**.

1. Enable the in-game overlay in tosu (`ENABLE_INGAME_OVERLAY=true` or in the dashboard).
2. Start osu! with the launcher from the overlay folder (`game-overlay/hook/` next to the tosu binary):

   ```sh
   /path/to/tosu/game-overlay/hook/tosu-overlay-run /path/to/osu.AppImage
   ```

   For Steam, Lutris, Heroic or other launchers, put this in the launch options / command prefix:

   ```
   /path/to/tosu/game-overlay/hook/tosu-overlay-run %command%
   ```
3. Press the overlay keybind (default `Control + Shift + Space`) in game to configure overlays.
   While configuration mode is active, mouse and keyboard go to the overlay instead of osu!.

`tosu-overlay-run` only sets environment variables and starts the game:

| variable | purpose |
| --- | --- |
| `LD_PRELOAD=…/libtosu-overlay.so` | OpenGL renderer and input capture |
| `VK_ADD_LAYER_PATH=…/hook` and `VK_INSTANCE_LAYERS=VK_LAYER_TOSU_overlay` | Vulkan renderer |

Set `TOSU_OVERLAY_DEBUG=1` for verbose logs on the game's stderr.

### How it works

```
tosu ──ipc──▶ tosu-ingame-overlay (Electron, offscreen)
                     │  BGRA frames, keybind          ▲ input events, game size
                     ▼                                │
             /dev/shm/tosu-overlay-<osu pid>  ◀───────┘
                     ▲
                     │ libtosu-overlay.so inside osu!
                     ├─ OpenGL: eglSwapBuffers / glXSwapBuffers hook
                     ├─ Vulkan: VK_LAYER_TOSU_overlay, draws before vkQueuePresentKHR
                     └─ SDL3: event filter / PeepEvents hook for keybind and input capture
```

osu!lazer resolves EGL, GLX and SDL through `dlsym`, so the library interposes `dlsym` to wrap the
few functions it needs. The shared memory layout is defined in
[`native/linux/src/shared.h`](native/linux/src/shared.h) and mirrored in
[`src/linux/shared.ts`](src/linux/shared.ts).

### Building

Requirements: CMake, a C compiler, `glslc` (shaderc). Vulkan headers are used from the system
when installed, otherwise CMake downloads them.

```sh
pnpm build:overlay:linux   # from the repository root
```

The result is in `pack/linux-unpacked/`. Copy its contents to `game-overlay/` next to the tosu binary.

Native test programs (an SDL3 window rendering like lazer does, and a stand-in overlay host):

```sh
cmake -S native/linux -B native/linux/build -DTOSU_OVERLAY_TESTS=ON
cmake --build native/linux/build
native/linux/build/tosu-overlay-run native/linux/build/tests/overlay_test vk 10 &
node native/linux/tests/fake_host.ts $! 8
```
