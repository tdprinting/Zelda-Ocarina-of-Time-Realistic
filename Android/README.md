# Android

Android build of the game, aimed at the AYN Odin 2 Portal (Snapdragon 8 Gen 2, Adreno 740) but
usable on any 64-bit Android 8.0+ device with OpenGL ES 3.0.

## Playing

1. Make `oot.o2r` (and `oot-mq.o2r` for Master Quest, if you like) from your own ROM with the
   Windows or Linux build, and copy it to the device.
2. Install the APK from the `android` CI run (or build it, below).
3. On first launch the app asks for the archive: pick `oot.o2r`, plus `oot-mq.o2r` if you have it.
   The app copies them into its own folder, so you can delete the originals afterwards.

The app never contains the game: no ROM or ROM-derived files are in the APK, only the port's own
`soh.o2r`.

Controls: the Odin's built-in gamepad works like any SDL game controller. **Select** opens the
game menu (enhancements, graphics, controller remapping), and the menu is navigable with the
gamepad or by touch.

Files live in `Android/data/com.tdprinting.zoot/files/` on the device's storage: settings
(`shipofharkinian.json`), saves, `mods/`, and the archives. If an update needs a fresh `oot.o2r`,
the game says so and the app asks for one again on its next start.

## Building

Needs the Android SDK with NDK `28.2.13676358` and CMake `4.1.2` (both via `sdkmanager`), JDK 17+,
and a host C++ toolchain with the Linux build dependencies (`linux-build-deps/apt.txt`) for the
asset tools.

```sh
export ANDROID_HOME=~/Android/Sdk
Android/build-apk.sh            # release APK, signed with the debug key
Android/build-apk.sh debug
```

The script first builds `soh.o2r` with a host-only configure (`-DSOH_TOOLS_ONLY=ON`), because the
asset tools can't run inside the Android cross-compile, then runs gradle, which builds the game as
`libsoh.so` through the NDK. Set `SOH_O2R=/path/to/soh.o2r` to reuse one you already have. You can
also open `Android/` in Android Studio once `app/src/main/assets/soh.o2r` exists.

## How it fits together

- `LauncherActivity` copies `soh.o2r` out of the APK after each install or update, and asks for the
  ROM archive when none is present. There's no in-game ROM extractor on Android.
- `GameActivity` is SDL's `SDLActivity`; it loads `libsoh.so` (the game with SDL linked in) and calls
  `SDL_main`.
- Rendering is libultraship's OpenGL ES 3 backend (`USE_OPENGLES`).
- `cmake/dependencies.cmake` builds Ogg, Vorbis, Opus, opusfile and SDL_net, which the NDK lacks.
- The Java files in `org/libsdl/app` are SDL's own, copied from the SDL release libultraship fetches
  (`release-2.32.10`). Update them together with that SDL version.
