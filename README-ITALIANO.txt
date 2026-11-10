FormantSlide Native v0.4 - JUCE EDITOR RESTORE
=============================================

This patch replaces the broken Win32 edit-box UI of Native v0.3 with a JUCE
editor styled and laid out after our working v0.2 VST3.

WHAT CHANGED
- Rebuilt dark JUCE UI with waveform, slice boundaries and selectable slices.
- Load WAV and Import MIDI via buttons, or drag and drop WAV/MID.
- Fixed native editor window lifecycle: publish only a valid HWND to FL Studio.
- Restored continuous JUCE sliders for First MIDI note, divisions, glide.
- Added FL note calibration (-24 to +24 semitones) for host-note mapping.
- Restored v0.2's Signalsmith window 1024 / interval 192 / formant base 200 Hz.
- UI changes to glide and note calibration no longer stop the current voice.
- Native FL Studio note slides still follow FinalLevels.Pitch from the SDK.
- Colour slide duration: SDK FPV_GetLength, MIDI import, glide fallback.
- Artifact is now named FormantSlide_x64.dll, matching FL native conventions.
- WAV silence detector unchanged from v0.2; if only one slice is detected,
  select Equal divisions and set the number of notes in the WAV.

INSTALL TO GITHUB (on the flnative branch)
1. Replace CMakeLists.txt.
2. Replace the full Source/ folder with Source/ from this patch.
3. Replace .github/workflows/build-windows.yml.
4. Keep the existing SDK/ directory in the repository root. No need to upload it again.
5. Commit and manually run the GitHub Action on branch flnative.
6. Copy the produced FormantSlide_x64.dll to
   D:\FL STUDIO 24\Plugins\Fruity\Generators\FormantSlide\
7. Close FL Studio fully before replacing the DLL, then start it again.

TESTING
- Open plugin, load a chromatic WAV; waveform and slice lines should appear.
- Status text reports detected slice count. If 1: select Equal divisions,
  set count to match WAV, then move First MIDI note to the note of slice 1.
- Slide mode controls channels 5 only vs channels 3-16.
- FL note calibration normally starts at 0. If pitch is one octave too low,
  try +12; if one octave too high, try -12. This does NOT change target
  note differences for the slide notes.
- For colored slide-note length, imported MIDI remains an optional fallback.
- FL-native triangle slides use the host-provided FinalLevels.Pitch curve.

IMPORTANT LIMITS
- We cannot test the FL Studio host/UI behavior or perform an MSVC Windows
  build on this runner; compilation via GitHub Actions is required.
- JUCE initialization and child-window embedding are based on JUCE's Windows
  addToDesktop(native parent HWND) API; this has not yet been tested in FL 2025.
- Audio formant quality is not guaranteed to be flawless. This patch restores
  the original GUI and pitch-shifting configuration, not a new DSP algorithm.
- A WAV with no detectable gaps may genuinely have only one auto-detected
  slice; manual equal divisions is intentionally not guessed.
- The SDK/ content is third-party/nonofficial; check its license and compatibility.
