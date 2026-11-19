# TC Spectrum Analyzer

Spectrum analyzer VST3 for macOS (Apple Silicon) with the graphic behaviour of the Waves PAZ frequency display, plus a K-System meter.

## Features

- Steep filter bank like the PAZ (14th-order Chebyshev band-pass per band, peak / RMS detection of the band output): linear bands below 250 Hz (LF Res 10/20/40 Hz), 1/7-octave bands (Q ≈ 10) above.
- Detect Peak/RMS, Response 10 ms – 3 s (default 160 ms), Weighting Flat/A/B/C.
- Optional peak hold with adjustable fall time (fall = time to drop 20 dB, up to ∞).
- Multichannel: reads the bus layout automatically (stereo up to 7.1.4 / 16 channels), one coloured curve per channel, channels can be hidden. Show Channels / Sum / Channels + Sum.
- Channel colours are global (saved in `~/Library/Application Support/TC Spectrum Analyzer`).
- K-Meter (Bob Katz K-12 / K-14 / K-20): AES17 RMS, 600 ms window, 4x true peak (dBTP), True Peak Max / RMS Max (click to reset). Can be hidden.
- Zoom: mouse wheel = dB zoom, Cmd + wheel = frequency zoom, drag = pan, double-click = reset spectrum peaks.
- Light on CPU: the audio thread only copies samples; analysis runs on a background thread with a multirate filter bank (hidden or silent channels are skipped); only the curves are redrawn each frame.
- The window scales uniformly with a fixed aspect ratio.

## Install

```sh
cd /tmp && curl -L -o TCSA.zip https://github.com/Hymmoth/TC-Spectrum-Analyzer/releases/download/latest/TC-Spectrum-Analyzer-macOS.zip \
  && rm -rf "$HOME/Library/Audio/Plug-Ins/VST3/TC Spectrum Analyzer.vst3" \
  && ditto -x -k TCSA.zip "$HOME/Library/Audio/Plug-Ins/VST3/" && rm TCSA.zip
```

Downloading with `curl` avoids the macOS quarantine flag, so no Gatekeeper warning appears.

## Build

```sh
cmake -B build -G Xcode
cmake --build build --config Release --target TCSpectrumAnalyzer_VST3
```

## License

AGPL-3.0 (JUCE is used under the AGPLv3).
