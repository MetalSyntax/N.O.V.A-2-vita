# N.O.V.A 2 Vita v1.0.1

This is a wrapper/port of **N.O.V.A. 2 – Near Orbit Vanguard Alliance HD** (Gameloft) for the PS Vita.

## Changelog

- v1.0.1: Save hotfix.
  - **Progress now saves on a fresh install.** The game saves to its Android data folder,
    which the port maps to `ux0:data/nova2/saves/`. The game never creates that folder
    (on Android it always exists), and the old README said it was created automatically.
    If it was missing, every save failed silently, so after a restart there was only
    "New Game" and no "Continue". The port now creates `ux0:data/nova2/saves/` at startup.
  - **Already playing?** Just install the new VPK. If `ux0:data/nova2/saves/` already
    existed on your card, your progress was being saved and is kept.
  - Note: like on Android, the game only saves at checkpoints and when a level is finished,
    not when pausing or quitting.

- v1.0: Initial release.
  - **Plays at 60 FPS at native 960x544**, 1:1 with the Vita screen. The engine is given the
    real resolution, so the 3D game and HUD fill the screen without scaling. The static 800x480
    loading/title backgrounds are stretched to fullscreen at sprite level.
  - **Intro video plays unconverted.** `GLVideo.mp4` (H.264 Baseline 640x360 @ 60 fps + AAC
    44.1 kHz) is played exactly as shipped with the game. SceAvPlayer refuses this file, so the
    port uses its own MP4 demuxer with the Vita's **hardware H.264 decoder** (`sceAvcdec`) and
    software AAC audio. Skip it with Cross or Start.
  - **Full physical controls.** Every visible HUD button is mapped to a physical button, using
    the button positions read from the game's own code:
    - Left stick: move. Right stick: camera.
    - R: fire (hold). L: special power. Cross: jump.
    - Circle: reload. Triangle / D-Pad Up: next weapon. D-Pad Down: previous weapon.
    - D-Pad Left / Right: change special power. Square: grenade. Start: pause.
  - **Virtual touch controls at 1% opacity.** The joystick, fire, jump and special-power buttons
    are nearly invisible but still respond to touch. The HUD that shows ammo and grenades stays
    fully visible.
  - **Smooth camera with adjustable sensitivity.** The right stick works like a continuous finger
    drag: hold it to keep turning, light tilt for precise aiming. Press **Select + D-Pad Up/Down**
    to set the sensitivity (1-10, default 4). A bar at the top of the screen shows the level, and
    the setting is saved in `ux0:data/nova2/camera_sens.txt`.
  - **Performance fixes.** Removed per-frame log writes and per-sound log spam. Added a negative
    cache for missing files: the engine looks for a loose `.wav` that does not exist before every
    footstep or gunshot, and that used to stall a frame each time.
  - Software ATC texture decoding, Vox audio through `sceAudioOut`, Gameloft DRM bypassed, and
    save/data paths remapped to `ux0:data/nova2/`.

## Setup Instructions

In order to properly install the game, you'll need to extract the data from the original Android game.

1. Install the `kubridge` and `fd_fix` plugins on your PS Vita.
2. Install `libshacccg.suprx`, if you don't have it already, by following [this guide](https://samilops2.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx).
3. Obtain the original APK of N.O.V.A. 2 HD (`NOVA-2-HD-v1-0-5.apk`) and its game data folder (`/sdcard/gameloft/games/GloftN2HP/` from an Android device where the game was installed and run once).
4. Extract the `.apk` using any zip extractor.
5. Place `libnova2.so` (from `lib/armeabi-v7a/` inside the APK) in `ux0:data/nova2/`.
6. Place `assets/data.save` from the APK in `ux0:data/nova2/assets/`, and the APK's `res/raw/` files in `ux0:data/nova2/res/raw/`.
7. Copy the whole `GloftN2HP/` data folder (the `.gla` files, `sounds_hi.glza`, `oconf.bar`, `GLVideo.mp4`, ...) to `ux0:data/nova2/GloftN2HP/`.
8. Download and install `nova2.vpk` on your PS Vita.

`libStormGLOFT.so` and `libmthook.so` are not needed.

## Controls

- **Left Analog** - Move
- **Right Analog** - Camera (hold to keep turning; light tilt = precise aim)
- **R** - Fire (hold)
- **L** - Use special power
- **Cross** - Jump
- **Circle** - Reload
- **Triangle / D-Pad Up** - Next weapon
- **D-Pad Down** - Previous weapon
- **D-Pad Left / Right** - Change special power
- **Square** - Grenade
- **Start** - Pause
- **Select + D-Pad Up / Down** - Camera sensitivity (1-10)
- **Touch Screen** - Original touch controls (drawn at 1% opacity)

## Known Issues

- Only the boot intro video is supported. Any later video request is skipped.
- Pause, weapon and grenade touch spots were measured from screenshots (they are 3D HUD nodes). If one of them misses on your console, please report it with a log.
- Very heavy combat may still show small hitches on slow memory cards (sound effects are streamed from `sounds_hi.glza`).

## Troubleshooting

**"Looks like you haven't installed the data files" on boot:** `ux0:data/nova2/libnova2.so` is missing or in the wrong folder.

**Black screen / crash while loading:** check that `ux0:data/nova2/GloftN2HP/` contains all the `.gla` files and `sounds_hi.glza`, and that `kubridge`, `fd_fix` and `libshacccg.suprx` are installed.

**If it still crashes**, please report it with:

1. The newest `ux0:data/nova2/logs/nova2_NNNN.log` (from the time of the crash; do not delete it).
2. The newest `ux0:data/psp2core-*-eboot.bin.psp2dmp` core dump.
3. A note on exactly when it happens (boot, intro, menu, which level).

## Credits

- TheFloW for the `so_loader` wrapper and Android porting boilerplate.
- Rinnegatamante for `vitaGL` and porting tools.
- v-atamanenko for `FalsoJNI` and the soloader boilerplate.
- Gameloft for the original game.
