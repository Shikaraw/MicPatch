# MicPatch

[简体中文](README.md)

MicPatch is a lightweight Windows 11 x64 mixer that sends a microphone and an audio file to a single input in your calling app. It includes per-source gain and mute, file playback and looping, speech EQ, and automatic music ducking.

## Set up a call with VB-CABLE

1. Install [VB-CABLE](https://vb-audio.com/Cable/) from VB-Audio if it is not already installed. MicPatch does not bundle it.
2. Add your physical microphone, an audio file, or both. In MicPatch, choose **CABLE Input (VB-Audio Virtual Cable)** as the output (not **CABLE In 16ch**).
3. In your calling app, choose the matching recording device: usually **CABLE Output (VB-Audio Virtual Cable)**, or **Virtual Mic (VB-Audio Virtual Cable)** if the recording endpoint has been renamed. Keep the calling app's speakers set to your headphones.
4. Start sending in MicPatch. For a file, also press Play. Master Mute silences the mix without pausing the file. If the calling app suppresses music, check its noise suppression or music mode.

## Settings in v0.1.0

Click **设置** (Settings) to select **English**; Simplified Chinese is the default. Choose Graphite, Ocean, or Light; choose a global mute shortcut (`Ctrl+Alt+M`, `Ctrl+Alt+Shift+M`, or Off); optionally remember the selected output or start with Master Mute on. Restart MicPatch after saving to apply startup preferences. Remembering an output never starts sending, selects another device as a fallback, or changes Windows defaults.

## Build from source

Use Visual Studio 2022 with the x64 C++ toolset, Windows SDK, and CMake 3.25 or later:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable is `build/Release/MicPatch.exe`. The separate [MicPatch virtual microphone driver](driver/README.md) is still experimental: this repository does not provide a signed release driver package. Use VB-CABLE for the verified route.

The application is covered by the [root MIT license](LICENSE). The SysVAD-derived driver retains its separate [Microsoft Public License](driver/LICENSE).
