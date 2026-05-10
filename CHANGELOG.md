# Changelog

## 1.2.0 — 2026-05-10

Fixes a crash that made the plugin unusable on Windows 11 after any audio device change, adds automatic default device tracking, and resolves 16 bugs found during the investigation.

### Bug fixes

**Crash on stale device GUID (the main one)**
When `ID=` referenced a device GUID that no longer existed, `GetDevice()` failed but left `m_dev` uninitialised. `DeviceInit()` then called `m_dev->OpenPropertyStore()` unconditionally — the `assert` was stripped in Release builds — causing an access violation (`c0000005`) that crashed Rainmeter on startup. Fixed by setting `m_dev = NULL` explicitly on failure and replacing the assert with a proper null check.

**Bars freezing when audio source exits abruptly**
When a playing process ended without sending `AUDCLNT_BUFFERFLAGS_SILENT` (browser tab switch, app crash, account change), `GetNextPacketSize()` returned zero but output arrays were never cleared. Bars stayed frozen at their last values. Fixed with a 300ms empty-packet counter: after no data for 300ms, all output arrays are zeroed. Capture thread `WaitForMultipleObjects()` timeout also changed from `INFINITE` to 50ms to prevent the thread stalling when packets stop.

**Other fixes**
- `m_rm` was never initialised before use in logging calls
- `IsFormatSupported()` left `m_wfx` pointing to freed memory
- OOM on chunk buffer allocation was not checked
- Shadowed `m_nFramesNext` broke `BufferStatus` calculations
- Multichannel audio wrote past the end of `m_rms`/`m_peak` arrays
- Mono sum channel read one frame past the buffer end
- FFT window indexing error; dynamic mean square not reset per frame
- Wave-band loop had an off-by-one read
- `FFTFreq` child used child `m_df` instead of parent `m_df`
- x64 pointer truncation in `_snwprintf_s` size arithmetic
- Capture thread was detached without a join; teardown could hang or crash on unload
- Config reload leaked FFT/wave/band allocations
- Missing mutex around capture/output state caused a data race

### New features

**Automatic default device tracking**
`DefaultDeviceNotificationClient` implements `IMMNotificationClient` and is registered at initialisation. When the system default render device changes, `OnDefaultDeviceChanged()` calls `ReinitializeDevice()` — the plugin follows device switches at runtime with no Rainmeter restart. Leaving `ID=` blank now safely uses the Windows default endpoint and stays in sync as it changes.

**Fallback to default device**
When `ID=` specifies a device that is not found, the plugin logs a warning and falls back to the default endpoint rather than crashing.

### Project

Updated `PlatformToolset` from `v142` to `v145` and `WindowsTargetPlatformVersion` from `8.1` to `10.0.26100.0`. The 8.1 SDK is absent from current VS2022 installations.

---

## 1.1.8.6 and earlier

See the original repository at [thesn10/AudioLevelBeta](https://github.com/thesn10/AudioLevelBeta).
