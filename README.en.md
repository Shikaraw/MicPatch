# MicPatch

[简体中文](README.md)

MicPatch is a lightweight Windows 11 x64 mixer that combines up to 16 microphones, audio files, and playback-device sources into a single input for your calling app. Each source has its own gain, mute, and meter; files have independent playback and looping.

Playback-device sources capture **everything** playing on that device. If the remote party's voice is heard on that device, you may send it back to them. MicPatch blocks a loopback source on the exact endpoint used for mix output; it does not cancel echoes across different devices.

## Set up a call with VB-CABLE

1. Install [VB-CABLE](https://vb-audio.com/Cable/) from VB-Audio if it is not already installed. MicPatch does not bundle it.
2. Add your physical microphone, an audio file, or both. In MicPatch, choose **CABLE Input (VB-Audio Virtual Cable)** as the output (not **CABLE In 16ch**).
3. In your calling app, choose the matching recording device: usually **CABLE Output (VB-Audio Virtual Cable)**, or **Virtual Mic (VB-Audio Virtual Cable)** if the recording endpoint has been renamed. Keep the calling app's speakers set to your headphones.
4. Start sending in MicPatch. For a file, also press Play. Master Mute silences the mix without pausing the file. If the calling app suppresses music, check its noise suppression or music mode.

## Settings and session restore in v0.1.1

MicPatch automatically saves the source list and per-source controls for the next launch. You can turn this off in Settings. Remember Output defaults to on for new installations; upgrades preserve the previous preference. Restoring the last session does not start sending or playing, restore a playback position, select a fallback device, or change Windows defaults.

Click **设置** (Settings) to select **English**; Simplified Chinese is the default. Choose Graphite, Ocean, or Light; choose a global mute shortcut (`Ctrl+Alt+M`, `Ctrl+Alt+Shift+M`, or Off); or start with Master Mute on. Restart MicPatch after saving to apply startup preferences.

## Build from source

Use Visual Studio 2022 with the x64 C++ toolset, Windows SDK, and CMake 3.25 or later:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable is `build/Release/MicPatch.exe`. The separate [MicPatch virtual microphone driver](driver/README.md) is still experimental: this repository does not provide a signed release driver package. Use VB-CABLE for the verified route.

The application is covered by the [root MIT license](LICENSE). The SysVAD-derived driver retains its separate [Microsoft Public License](driver/LICENSE).
