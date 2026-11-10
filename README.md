# FormantSlide — JUCE VST3 prototype

A minimal Windows VST3 synthesizer prototype for FL Studio MIDI color-group workflows.

## What works
- WAV loading by drag-and-drop or file chooser (up to 10 minutes at 44.1 kHz, maximum 2 channels)
- Single-voice sample playback with linear interpolation
- MIDI channels 1–2 trigger voice notes
- MIDI channel 5 or channels 3–16 control pitch slides
- Configurable *temporary* glide time (250 ms default) to validate MIDI routing
- Sample-accurate MIDI event positions within processing blocks

## What does NOT work yet
- **No formant preservation.** Resampling changes pitch and playback speed.
- No automatic chromatic slicing / sample mapping.
- No exact glide duration inferred from MIDI note length (normal MIDI Note On does not supply Note Off time).
- Slide Note Off does not stop a running glide in this first test.
- No project-persistent embedded sample. Reload the WAV after reopening the project.
- No voice release envelope, advanced overlapping-note handling or polished anti-aliasing.

## Build on GitHub (no local Visual Studio)
1. Create a GitHub repository and upload **the contents** of this folder, including `.github/workflows/build-windows.yml`.
2. Use branch `main`, then go to **Actions** > **Build FormantSlide VST3 (Windows)** > **Run workflow**.
3. When the build finishes, download the `FormantSlide-Windows-VST3` artifact.
4. Extract the artifact and copy the `.vst3` bundle folder into `C:\Program Files\Common Files\VST3\`.
5. In FL Studio, run Plugin Manager's plugin scan. Load FormantSlide as an instrument.
6. Drag a WAV into the plugin, put a long note on MIDI channel 1, then a higher overlapping note on MIDI channel 5. The slide begins on the second note's Note On.

## Dependencies and licensing
CMake downloads JUCE 8.0.10 from its official GitHub repository during CI. A GitHub-hosted `windows-2022` runner supplies MSVC and the Windows SDK. Check JUCE licensing before distributing commercial or closed-source binaries. No Rubber Band or other formant DSP is included in this prototype.

## Next engineering milestones
1. Verify FL Studio MIDI channel routing and plugin state restore.
2. Implement sample slicing and MIDI note-to-slice mapping.
3. Replace resampling with a suitable real-time pitch-shifter that preserves formants; assess CPU and latency.
4. Add a pitch glide state machine for multiple slide events and a clearly documented Note Off policy.
