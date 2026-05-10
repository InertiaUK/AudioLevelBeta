# AudioLevelBeta

A Rainmeter plugin for real-time audio visualisation. Captures the Windows audio loopback and exposes FFT spectrum, RMS, peak, wave, and band data to Rainmeter skins.

This is a maintained fork of [thesn10/AudioLevelBeta](https://github.com/thesn10/AudioLevelBeta), which itself builds on the original [AudioLevel plugin](https://docs.rainmeter.net/manual/plugins/audiolevel/) by David Grace. The 1.2.0 release fixes a crash that made the plugin unusable on current Windows 11 builds, adds automatic default device tracking, and clears up a number of other bugs found during the investigation.

Maintained by [Mark Andrews](https://keepcomputing.co.uk).

## Installation

Drop `AudioLevelBeta.dll` into `%APPDATA%\Rainmeter\Plugins\`. Grab the latest build from the [releases page](https://github.com/InertiaUK/AudioLevelBeta/releases). Restart Rainmeter.

## Usage

### Parent measure

```ini
[Audio]
Measure=Plugin
Plugin=AudioLevelBeta
Port=Output
ID=
FFTSize=2048
FFTBufferSize=16384
Bands=90
FreqMin=20
FreqMax=20000
Sensitivity=50
UpdatesPerSecond=60
OnUpdateAction=[!UpdateMeasureGroup Audio][!UpdateMeterGroup Bars][!Redraw]
```

Leave `ID=` blank to use the Windows default output device. The plugin will follow device changes automatically — no restart needed if you switch output in Sound settings.

To target a specific device, set `ID=` to the device endpoint ID (use `Type=DeviceList` to enumerate available devices).

### Child measures

```ini
[Band1]
Measure=Plugin
Plugin=AudioLevelBeta
Parent=Audio
Type=Band
BandIdx=0

[RMS]
Measure=Plugin
Plugin=AudioLevelBeta
Parent=Audio
Type=RMS
Channel=Sum
```

### Types

| Type | Description |
|------|-------------|
| `Band` | FFT frequency band value. Use `BandIdx=N`. |
| `FFT` | Raw FFT bin value. Use `FFTIdx=N`. |
| `FFTFreq` | Centre frequency of FFT bin N. |
| `RMS` | RMS level. |
| `Peak` | Peak level. |
| `Wave` | Raw waveform sample. Use `WaveIdx=N`. |
| `WaveBand` | Smoothed waveform band. Use `BandIdx=N`. |
| `BandFreq` | Centre frequency of band N. |
| `BufferStatus` | Returns 1 when new audio data is available. Use with `OnUpdateAction`. |
| `DeviceList` | Returns a newline-separated list of available audio devices. |

### Options (parent measure)

| Option | Default | Description |
|--------|---------|-------------|
| `Port` | `Output` | `Output` for loopback capture, `Input` for microphone. |
| `ID` | *(blank)* | Device endpoint ID. Blank = Windows default output. |
| `Channel` | `Sum` | `L`, `R`, `C`, `Sum`, `Avg`, or channel number. |
| `FFTSize` | `256` | FFT window size. Higher = more frequency resolution. |
| `FFTBufferSize` | `0` | Ring buffer size for FFT. |
| `FFTAttack` / `FFTDecay` | `300` | Smoothing time constants in ms. |
| `Bands` | `0` | Number of frequency bands. |
| `FreqMin` / `FreqMax` | `20` / `20000` | Frequency range in Hz. |
| `Sensitivity` | `35` | dB sensitivity for FFT output. |
| `AverageSize` | `1` | Number of FFT frames to average. |
| `UpdatesPerSecond` | `-1` | Max update rate. `60` recommended. `-1` = unlimited. `-2` = legacy mode. |
| `DynamicVolume` | `0` | Normalises output across volume levels. |
| `WaveSize` | `256` | Wave/WaveBand buffer size. |
| `Smoothing` | `0` | Band/WaveBand neighbour averaging. |

## Building

Requires Visual Studio 2022 and the [Rainmeter Plugin SDK](https://github.com/rainmeter/rainmeter-plugin-sdk). The SDK's `API` folder must sit two directories above the project file.

```
E:\
  API\
    RainmeterAPI.h
    x64\Rainmeter.lib
    x32\Rainmeter.lib
  Source Code\
    AudioLevelBeta\
      PluginAudioLevelBeta.vcxproj
```

Build x64 Release:

```
MSBuild PluginAudioLevelBeta.vcxproj /p:Configuration=Release /p:Platform=x64
```

The DLL is written directly to `%APPDATA%\Rainmeter\Plugins\`.

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## Licence

GPL-2.0. See [LICENSE](LICENSE).

## Credits

- Original AudioLevel plugin: [David Grace](https://docs.rainmeter.net/manual/plugins/audiolevel/)
- AudioLevelBeta: [thesn10](https://github.com/thesn10/AudioLevelBeta), [SnGmng](https://github.com/SnGmng), [alatsombath](https://github.com/alatsombath)
- 1.2.0 fixes: [Mark Andrews](https://keepcomputing.co.uk)
