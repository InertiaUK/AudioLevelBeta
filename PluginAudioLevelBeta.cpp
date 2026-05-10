/* Copyright (C) 2014 Rainmeter Project Developers
*
* This Source Code Form is subject to the terms of the GNU General Public
* License; either version 2 of the License, or (at your option) any later
* version. If a copy of the GPL was not distributed with this file, You can
* obtain one at <https://www.gnu.org/licenses/gpl-2.0.html>. */

#include <Windows.h>
#include <cstdio>
#include <AudioClient.h>
#include <AudioPolicy.h>
#include <MMDeviceApi.h>
#include <FunctionDiscoveryKeys_devpkey.h>
#include <VersionHelpers.h>

#include <cmath>
#include <cassert>
#include <vector>
#include <algorithm>
#include <atomic>
#include <mutex>

#include <thread>
#include <chrono>
#include <avrt.h>
#pragma comment(lib, "Avrt.lib")

#include "../../API/RainmeterAPI.h"

#include "pffft/pffft.h"

// Overview: Audio level measurement from the Window Core Audio API
// See: http://msdn.microsoft.com/en-us/library/windows/desktop/dd370800%28v=vs.85%29.aspx

// Sample skin:
/*
[mAudio_Raw]
Measure=Plugin
Plugin=AudioLevel.dll
Port=Output

[mAudio_RMS_L]
Measure=Plugin
Plugin=AudioLevel.dll
Parent=mAudio_Raw
Type=RMS
Channel=L

[mAudio_RMS_R]
Measure=Plugin
Plugin=AudioLevel.dll
Parent=mAudio_Raw
Type=RMS
Channel=R
*/

#define WINDOWS_BUG_WORKAROUND	1
#define TWOPI					(2 * 3.14159265358979323846)
#define EXIT_ON_ERROR(hres)		if (FAILED(hres)) { goto Exit; }
#define SAFE_RELEASE(p)			if ((p) != NULL) { (p)->Release(); (p) = NULL; }
#define CLAMP01(x)				max(0.0, min(1.0, (x)))

struct Measure;

class DefaultDeviceNotificationClient : public IMMNotificationClient
{
public:
	DefaultDeviceNotificationClient(Measure* measure) :
		m_refCount(1),
		m_measure(measure)
	{
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++m_refCount;
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG refCount = --m_refCount;
		if (refCount == 0)
		{
			delete this;
		}
		return refCount;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
	{
		if (!ppvObject)
		{
			return E_POINTER;
		}

		if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
		{
			*ppvObject = static_cast<IMMNotificationClient*>(this);
			AddRef();
			return S_OK;
		}

		*ppvObject = NULL;
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override;
	HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override { return S_OK; }

private:
	std::atomic<ULONG> m_refCount;
	Measure* m_measure;
};

struct Measure
{
	enum Port
	{
		PORT_OUTPUT,
		PORT_INPUT,
	};

	enum Channel
	{
		CHANNEL_FL,
		CHANNEL_FR,
		CHANNEL_C,
		CHANNEL_LFE,
		CHANNEL_BL,
		CHANNEL_BR,
		CHANNEL_SL,
		CHANNEL_SR,
		CHANNEL_SUM,
		MAX_CHANNELS
	};

	enum Type
	{
		TYPE_RMS,
		TYPE_PEAK,
		TYPE_FFT,
		TYPE_WAVE,
		TYPE_BAND,
		TYPE_WAVEBAND,
		TYPE_FFTFREQ,
		TYPE_BANDFREQ,
		TYPE_FORMAT,
		TYPE_DEV_STATUS,
		TYPE_DEV_NAME,
		TYPE_DEV_ID,
		TYPE_DEV_LIST,
		TYPE_BUFFERSTATUS,
		// ... //
		NUM_TYPES
	};

	enum Format
	{
		FMT_INVALID,
		FMT_PCM_S16,
		FMT_PCM_F32,
		// ... //
		NUM_FORMATS
	};

	struct BandInfo
	{
		float freq;
		float x;
	};

	Port					m_port;						// port specifier (parsed from options)
	Channel					m_channel;					// channel specifier (parsed from options)
	Type					m_type;						// data type specifier (parsed from options)
	Format					m_format;					// format specifier (detected in init)
	int						m_envRMS[2];				// RMS attack/decay times in ms (parsed from options)
	int						m_envPeak[2];				// peak attack/decay times in ms (parsed from options)
	int						m_envFFT[2];				// FFT attack/decay times in ms (parsed from options)
	int						m_fftSize;					// size of FFT (parsed from options)
	int						m_fftBufferSize;			// size of FFT with zero-padding (parsed from options)
	int						m_fftIdx;					// FFT index to retrieve (parsed from options)
	int						m_waveIdx;					// WAVE index to retrieve (parsed from options)
	int						m_nBands;					// number of frequency bands (parsed from options)
	int						m_bandIdx;					// band index to retrieve (parsed from options)
	int						m_smoothing;				// smoothing level (parsed from options)
	int						m_smoothingMode;			// smoothing mode (parsed from options)
	int						m_waveSize;					// size of WAVE (parsed from options)
	int						m_ringBufferSize;			// size of the ring buffer for FFT and WAVE
	int						m_dynamicVolume;			// enable dynamic volume (parsed from options)
	UINT32					m_nFramesNext;				// number of frames obtained on the UpdateParent call
	UINT32					m_nSilentFrames;			// number of silent frames, used to calculate when to stop updating
	UINT32					m_nEmptyPacketCycles;		// number of update cycles with no available packets
	double					m_gainRMS;					// RMS gain (parsed from options)
	double					m_gainPeak;					// peak gain (parsed from options)
	double					m_freqMin;					// min freq for band measurement
	double					m_freqMax;					// max freq for band measurement
	double					m_sensitivity;				// dB range for FFT/Band return values (parsed from options)
	Measure*				m_parent;					// parent measure, if any
	void*					m_skin;						// skin pointer
	void*					m_rm;						// rainmeter pointer
	LPCWSTR					m_rmName;					// measure name
	IMMDeviceEnumerator*	m_enum;						// audio endpoint enumerator
	IMMDevice*				m_dev;						// audio endpoint device
	WAVEFORMATEX			m_wfxR;						// audio format request info
	WAVEFORMATEX*			m_wfx;						// audio format info
	IAudioClient*			m_clAudio;					// audio client instance
	IAudioCaptureClient*	m_clCapture;				// capture client instance
	IAudioClient*			m_clBugAudio;				// audio client for loopback events
#if (WINDOWS_BUG_WORKAROUND)
	IAudioRenderClient*		m_clBugRender;				// render client for dummy silent channel
#endif
	HANDLE					m_hReadyEvent;				// buffer-event handle to receive notifications
	HANDLE					m_hStopEvent;				// skin closed handle to receive notifications
	HANDLE					m_hTask;					// Multimedia Class Scheduler Service task
	std::thread*			m_updateLoopThread;			// thread for running the update loop
	std::mutex				m_mutex;					// protects device state and output buffers
	std::chrono::system_clock::time_point
							m_lastUpdate;				// time of last rainmeter update
	std::chrono::duration<double>
							m_waitUpdate;				// time to wait between updates
	std::chrono::duration<double>
							m_overheadUpdate;			// time that has been waited too long since last update
	double					m_updatesPerSecond;			// updates per second
	WCHAR					m_reqID[64];				// requested device ID (parsed from options)
	WCHAR					m_devName[64];				// device friendly name (detected in init)
	WCHAR					m_msgUpdate[256];			// rainmeter update command
	float					m_kRMS[2];					// RMS attack/decay filter constants
	float					m_kPeak[2];					// peak attack/decay filter constants
	float					m_kFFT[2];					// FFT attack/decay filter constants
	float*					m_bufChunk;					// buffer for latest data chunk copy
	float					m_rms[MAX_CHANNELS];		// current RMS levels
	float					m_peak[MAX_CHANNELS];		// current peak levels
	float					m_fftMeanSquare;			// used for dynamic volume
	PFFFT_Setup*			m_fftCfg;					// FFT states for each channel
	float*					m_ringBuffer;				// ring buffer for audio data
	float*					m_fftOut;					// buffer for FFT output
	float*					m_fftKWdw;					// window function coefficients
	float*					m_ringBufOut;				// buffer for audio data from the ring buffer
	float*					m_fftTmpOut;				// temp FFT processing buffer
	int						m_ringBufW;					// write index for input ring buffers
	float*					m_bandFreq;					// buffer of band max frequencies
	float*					m_bandOut;					// buffer of band values
	float*					m_bandTmpOut;               // temp buffer of band values
	float*					m_waveBandOut;				// buffer of wave values
	float*					m_waveOut;					// temp buffer of wave values
	float*					m_waveBandTmpOut;			// 2nd temp buffer of wave values
	float					m_df;						// delta freqency between two bins
	float					m_dw;						// delta waveform values between two bands
	float					m_fftScalar;				// FFT scalar
	float					m_bandScalar;				// band scalar
	float					m_waveScalar;				// wave scalar
	float					m_smoothingScalar;			// smoothing scalar
	bool					m_wfxAllocated;				// true when m_wfx must be freed with CoTaskMemFree
	bool					m_usingDefaultDevice;		// true when blank ID or requested ID fallback uses default device
	bool					m_outputsSilenced;			// true after stale output buffers have been cleared
	DefaultDeviceNotificationClient*
							m_notificationClient;		// default endpoint change callback

	Measure() :
		m_port(PORT_OUTPUT),
		m_channel(CHANNEL_SUM),
		m_type(TYPE_RMS),
		m_format(FMT_INVALID),
		m_fftSize(0),
		m_fftBufferSize(0),
		m_fftIdx(-1),
		m_fftScalar(0),
		m_nBands(0),
		m_bandIdx(-1),
		m_bandScalar(0),
		m_df(0),
		m_dw(0),
		m_smoothing(0),
		m_smoothingMode(0),
		m_smoothingScalar(0),
		m_waveSize(0),
		m_waveIdx(0),
		m_waveScalar(0),
		m_ringBufferSize(0),
		m_nFramesNext(0),
		m_nSilentFrames(0),
		m_nEmptyPacketCycles(0),
		m_dynamicVolume(0),
		m_gainRMS(1.0),
		m_gainPeak(1.0),
		m_freqMin(20.0),
		m_freqMax(20000.0),
		m_sensitivity(0.0),
		m_parent(NULL),
		m_skin(NULL),
		m_rm(NULL),
		m_rmName(NULL),
		m_enum(NULL),
		m_dev(NULL),
		m_wfxR({ 0 }),
		m_wfx(NULL),
		m_clAudio(NULL),
		m_clCapture(NULL),
		m_clBugAudio(NULL),
#if (WINDOWS_BUG_WORKAROUND)
		m_clBugRender(NULL),
#endif
		m_hReadyEvent(NULL),
		m_hStopEvent(NULL),
		m_hTask(NULL),
		m_updateLoopThread(NULL),
		m_waitUpdate(NULL),
		m_overheadUpdate(NULL),
		m_updatesPerSecond(-1.0),
		m_fftKWdw(NULL),
		m_ringBufOut(NULL),
		m_fftTmpOut(NULL),
		m_ringBufW(0),
		m_bandFreq(NULL),
		m_wfxAllocated(false),
		m_usingDefaultDevice(false),
		m_outputsSilenced(true),
		m_notificationClient(NULL)
	{
		m_envRMS[0] = 300;
		m_envRMS[1] = 300;
		m_envPeak[0] = 50;
		m_envPeak[1] = 2500;
		m_envFFT[0] = 300;
		m_envFFT[1] = 300;
		m_reqID[0] = '\0';
		m_devName[0] = '\0';
		m_msgUpdate[0] = '\0';
		m_kRMS[0] = 0.0f;
		m_kRMS[1] = 0.0f;
		m_kPeak[0] = 0.0f;
		m_kPeak[1] = 0.0f;
		m_kFFT[0] = 0.0f;
		m_kFFT[1] = 0.0f;
		m_fftMeanSquare = 0.0f;
		m_fftCfg = NULL;
		m_bufChunk = NULL;
		m_ringBuffer = NULL;
		m_fftOut = NULL;
		m_bandOut = NULL;
		m_bandTmpOut = NULL;
		m_waveBandOut = NULL;
		m_waveOut = NULL;
		m_waveBandTmpOut = NULL;
	}

	HRESULT DeviceInit();
	void DeviceRelease(bool stopThread = true, bool releaseProcessing = true);
	HRESULT SelectDevice();
	HRESULT InitializeAnalysisBuffers();
	void ReleaseAnalysisBuffers();
	void StartCaptureThread();
	void OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR defaultDeviceId);
	HRESULT ReinitializeDevice();
	UINT32 EmptyPacketSilenceThreshold() const;
	void ZeroOutputBuffers();
	HRESULT UpdateParent();

	void DoCaptureLoop();
};

float pcmScalar = 1.0f / 0x7fff;

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
//const IID IID_IAudioClient3 = __uuidof(IAudioClient3);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);
const IID IID_IAudioRenderClient = __uuidof(IAudioRenderClient);

std::vector<Measure*> s_parents;

void Measure::DoCaptureLoop()
{
	// register thread with MMCSS
	DWORD nTaskIndex = 0;
	m_hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &nTaskIndex);
	if (!(m_hTask && AvSetMmThreadPriority(m_hTask, AVRT_PRIORITY_CRITICAL)))
	{
		DWORD dwErr = GetLastError();
		RmLog(m_rm, LOG_WARNING, L"Failed to start multimedia task.");
		m_hTask = NULL;
		return;
	}

	while (1)
	{
		HANDLE waitArray[2] = { m_hReadyEvent, m_hStopEvent };
		DWORD waitTimeout = 50;
		if (m_updatesPerSecond > 0)
		{
			waitTimeout = (DWORD)max(1.0, ceil(1000.0 / m_updatesPerSecond));
		}

		DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waitArray), waitArray, FALSE, waitTimeout);
		if (waitResult == WAIT_OBJECT_0 + 1)
		{
			break;
		}
		if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_TIMEOUT)
		{
			break;
		}

		// update parent seperated from measure update function
		HRESULT hr = UpdateParent();

		switch (hr)
		{
		case S_OK:
			// everything is fine, update measures

			// wait specified time (to not spam update calls)
			if (m_updatesPerSecond > 0) 
			{
				auto now = std::chrono::system_clock::now();
				auto elapsed = now - m_lastUpdate;
				auto waitTime = m_waitUpdate;// - m_overheadUpdate;
				if (elapsed >= waitTime)
				{
					// overheadUpdate should correct the overhead time making the update rate more accurate
					// but i cant tell the difference and i dont know if it helps or does the opposite
					// so i leave it disabled until i investigated further
					//m_overheadUpdate = elapsed - waitTime;
					//if (m_overheadUpdate > m_waitUpdate) {
					//	m_overheadUpdate = std::chrono::duration<double>(0);
					//}
					m_lastUpdate = now;
					RmExecute(m_skin, m_msgUpdate);
				}
			}
			// update as fast as possible
			else if (m_updatesPerSecond < 0 && m_updatesPerSecond >= -1)
			{
				RmExecute(m_skin, m_msgUpdate);
			}
			break;
		case S_FALSE:
			// silence detected, no need to update
			break;
		case AUDCLNT_E_BUFFER_ERROR:
		case AUDCLNT_E_DEVICE_INVALIDATED:
		case AUDCLNT_E_SERVICE_NOT_RUNNING:
			ReinitializeDevice();
			break;
		}
	}

	if (m_hTask)
	{
		AvRevertMmThreadCharacteristics(m_hTask);
		m_hTask = NULL;
	}
}

/**
* Create and initialize a measure instance.  Creates WASAPI loopback
* device if not a child measure.
*
* @param[out]	data			Pointer address in which to return measure instance.
* @param[in]	rm				Rainmeter context.
*/
PLUGIN_EXPORT void Initialize(void** data, void* rm)
{
	Measure* m = new Measure;
	m->m_skin = RmGetSkin(rm);
	m->m_rm = rm;
	m->m_rmName = RmGetMeasureName(rm);
	*data = m;

	// parse parent specifier, if appropriate
	LPCWSTR parentName = RmReadString(rm, L"Parent", L"");
	if (*parentName)
	{
		// match parent using measure name and skin handle
		std::vector<Measure*>::const_iterator iter = s_parents.begin();
		for (; iter != s_parents.end(); ++iter)
		{
			if (_wcsicmp((*iter)->m_rmName, parentName) == 0 &&
				(*iter)->m_skin == m->m_skin &&
				!(*iter)->m_parent)
			{
				m->m_parent = (*iter);

				return;
			}
		}

		RmLogF(rm, LOG_ERROR, L"Couldn't find Parent measure '%s'.", parentName);
	}

	// this is a parent measure - add it to the global list
	s_parents.push_back(m);

	// parse port specifier
	LPCWSTR port = RmReadString(rm, L"Port", L"");
	if (port && *port)
	{
		if (_wcsicmp(port, L"Output") == 0)
		{
			m->m_port = Measure::PORT_OUTPUT;
		}
		else if (_wcsicmp(port, L"Input") == 0)
		{
			m->m_port = Measure::PORT_INPUT;
		}
		else
		{
			RmLogF(rm, LOG_ERROR, L"Invalid Port '%s', must be one of: Output or Input.", port);
		}
	}

	_snwprintf_s(m->m_msgUpdate, _TRUNCATE, L"!UpdateMeasure %s", m->m_rmName);

	// create the enumerator
	EXIT_ON_ERROR(CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)& m->m_enum));

	// parse requested device ID (optional)
	LPCWSTR reqID = RmReadString(rm, L"ID", L"");
	if (reqID)
	{
		_snwprintf_s(m->m_reqID, _TRUNCATE, L"%s", reqID);
	}

	EXIT_ON_ERROR(m->SelectDevice());

	m->m_notificationClient = new DefaultDeviceNotificationClient(m);
	EXIT_ON_ERROR(m->m_enum->RegisterEndpointNotificationCallback(m->m_notificationClient));

	// init the device (if it fails, log debug message and quit)
	if (m->DeviceInit() == S_OK)
	{
		return;
	}

Exit:

	if (m->m_notificationClient)
	{
		if (m->m_enum)
		{
			m->m_enum->UnregisterEndpointNotificationCallback(m->m_notificationClient);
		}
		SAFE_RELEASE(m->m_notificationClient);
	}
	SAFE_RELEASE(m->m_enum);
}


/**
* Destroy the measure instance.
*
* @param[in]	data			Measure instance pointer.
*/
PLUGIN_EXPORT void Finalize(void* data)
{
	Measure* m = (Measure*)data;

	if (!m->m_parent)
	{
		m->DeviceRelease();
		if (m->m_notificationClient)
		{
			if (m->m_enum)
			{
				m->m_enum->UnregisterEndpointNotificationCallback(m->m_notificationClient);
			}
			SAFE_RELEASE(m->m_notificationClient);
		}
		SAFE_RELEASE(m->m_enum);

		std::vector<Measure*>::iterator iter = std::find(s_parents.begin(), s_parents.end(), m);
		if (iter != s_parents.end())
		{
			s_parents.erase(iter);
		}
	}

	delete m;
}


/**
* (Re-)parse parameters from .ini file.
*
* @param[in]	data			Measure instance pointer.
* @param[in]	rm				Rainmeter context.
* @param[out]	maxValue		?
*/
PLUGIN_EXPORT void Reload(void* data, void* rm, double* maxValue)
{
	static const LPCWSTR s_typeName[Measure::NUM_TYPES] =
	{
		L"RMS",								// TYPE_RMS
		L"Peak",							// TYPE_PEAK
		L"FFT",								// TYPE_FFT
		L"Wave",							// TYPE_WAVE
		L"Band",							// TYPE_BAND
		L"WaveBand",						// TYPE_WAVEBAND
		L"FFTFreq",							// TYPE_FFTFREQ
		L"BandFreq",						// TYPE_BANDFREQ
		L"Format",							// TYPE_FORMAT
		L"DeviceStatus",					// TYPE_DEV_STATUS
		L"DeviceName",						// TYPE_DEV_NAME
		L"DeviceID",						// TYPE_DEV_ID
		L"DeviceList",						// TYPE_DEV_LIST
		L"BufferStatus"						// TYPE_BUFFERSTATUS
	};

	static const LPCWSTR s_chanName[Measure::MAX_CHANNELS][3] =
	{
		{ L"L",		L"FL",		L"0", },	// CHANNEL_FL
		{ L"R",		L"FR",		L"1", },	// CHANNEL_FR
		{ L"C",		L"",		L"2", },	// CHANNEL_C
		{ L"LFE",	L"Sub",		L"3", },	// CHANNEL_LFE
		{ L"BL",	L"",		L"4", },	// CHANNEL_BL
		{ L"BR",	L"",		L"5", },	// CHANNEL_BR
		{ L"SL",	L"",		L"6", },	// CHANNEL_SL
		{ L"SR",	L"",		L"7", },	// CHANNEL_SR
		{ L"Sum",	L"Avg",		L"", },		// CHANNEL_SUM
	};

	Measure* m = (Measure*)data;

	// parse data type
	LPCWSTR type = RmReadString(rm, L"Type", L"");
	if (*type)
	{
		int iType;
		for (iType = 0; iType < Measure::NUM_TYPES; ++iType)
		{
			if (_wcsicmp(type, s_typeName[iType]) == 0)
			{
				m->m_type = (Measure::Type)iType;
				break;
			}
		}

		if (iType >= Measure::NUM_TYPES)
		{
			WCHAR msg[512];
			WCHAR* d = msg;
			d += _snwprintf_s(d, _countof(msg) - (d - msg), _TRUNCATE,
				L"Invalid Type '%s', must be one of:", type);

			for (int i = 0; i < Measure::NUM_TYPES; ++i)
			{
				d += _snwprintf_s(d, _countof(msg) - (d - msg), _TRUNCATE,
					L"%s%s%s", i ? L", " : L" ", i == (Measure::NUM_TYPES - 1) ? L"or " : L"", s_typeName[i]);
			}

			d += _snwprintf_s(d, _countof(msg) - (d - msg), _TRUNCATE, L".");
			RmLogF(rm, LOG_ERROR, msg);
		}
	}

	// parse channel specifier
	LPCWSTR channel = RmReadString(rm, L"Channel", L"");
	if (*channel)
	{
		bool found = false;
		for (int iChan = 0; iChan <= Measure::CHANNEL_SUM && !found; ++iChan)
		{
			for (int j = 0; j < 3; ++j)
			{
				if (_wcsicmp(channel, s_chanName[iChan][j]) == 0)
				{
					m->m_channel = (Measure::Channel)iChan;
					found = true;
					break;
				}
			}
		}

		if (!found)
		{
			WCHAR msg[512];
			WCHAR* d = msg;
			d += _snwprintf_s(d, _countof(msg) - (d - msg), _TRUNCATE,
				L"Invalid Channel '%s', must be an integer between 0 and %d, or one of:", channel, Measure::MAX_CHANNELS - 2);

			for (int i = 0; i <= Measure::CHANNEL_SUM; ++i)
			{
				d += _snwprintf_s(d, _countof(msg) - (d - msg), _TRUNCATE,
					L"%s%s%s", i ? L", " : L" ", i == Measure::CHANNEL_SUM ? L"or " : L"", s_chanName[i][0]);
			}

			d += _snwprintf_s(d, _countof(msg) - (d - msg), _TRUNCATE, L".");
			RmLogF(rm, LOG_ERROR, msg);
		}
	}

	// parse envelope, fft and band values on parents only
	if (!m->m_parent)
	{
		int fftSize = RmReadInt(rm, L"FFTSize", m->m_fftSize);
		int fftBufferSize = max(m->m_fftSize, RmReadInt(rm, L"FFTBufferSize", m->m_fftBufferSize));
		int nBands = RmReadInt(rm, L"Bands", m->m_nBands);
		int smoothing = max(0, RmReadInt(rm, L"Smoothing", m->m_smoothing));
		double freqMin = max(0.0, RmReadDouble(rm, L"FreqMin", m->m_freqMin));
		double freqMax = max(0.0, RmReadDouble(rm, L"FreqMax", m->m_freqMax));
		int waveSize = RmReadInt(rm, L"WAVESize", m->m_waveSize);

		// if one of these values changed, reinitialize
		if (m->m_fftSize		!= fftSize ||
			m->m_fftBufferSize	!= fftBufferSize ||
			m->m_freqMin		!= freqMin ||
			m->m_freqMax		!= freqMax ||
			m->m_waveSize		!= waveSize ||
			m->m_nBands			!= nBands ||
			m->m_smoothing		!= smoothing)
		{
			// initialize FFT data
			if (fftSize < 0 || fftSize & 1)
			{
				RmLogF(rm, LOG_ERROR, L"Invalid FFTSize %ld: must be an even integer >= 0. (powers of 2 work best)", fftSize);
				fftSize = 0;
			}
			m->m_fftSize = fftSize;
			m->m_fftBufferSize = fftBufferSize;

			// initialize WAVE data
			if (waveSize < 0 || waveSize & 1)
			{
				RmLogF(rm, LOG_ERROR, L"Invalid WAVESize %ld: must be an even integer >= 0.", waveSize);
				waveSize = 0;
			}
			m->m_waveSize = waveSize;

			m->m_ringBufferSize = max(fftSize, waveSize);

			// initialize frequency bands
			if (nBands < 0)
			{
				RmLogF(rm, LOG_ERROR, L"Invalid Bands %ld: must be an integer >= 0.", nBands);
				nBands = 0;
			}
			m->m_nBands = nBands;

			// initialize smoothing
			m->m_smoothing = smoothing;
			if (m->m_smoothing)
			{
				m->m_smoothingScalar = 1.0f / ((float)m->m_smoothing * 2.0f + 1.0f);
			}

			// initialize min/max frequency
			m->m_freqMin = freqMin;
			m->m_freqMax = freqMax;

			m->DeviceRelease(true, true);
			if (m->SelectDevice() != S_OK || m->DeviceInit() != S_OK || m->InitializeAnalysisBuffers() != S_OK)
			{
				RmLog(rm, LOG_ERROR, L"Failed to initialize audio analysis buffers.");
			}
		}

		// values that dont need fft/band reinitialization
		m->m_dynamicVolume = max(0, RmReadInt(rm, L"DynamicVolume", m->m_dynamicVolume));
		m->m_smoothingMode = min(max(0, RmReadInt(rm, L"SmoothingMode", m->m_smoothingMode)), 2);

		// update wait time
		m->m_updatesPerSecond = min(240, RmReadDouble(rm, L"UpdatesPerSecond", -1));
		if (m->m_updatesPerSecond > 0) {
			m->m_waitUpdate = std::chrono::duration<double>(1 / m->m_updatesPerSecond);
		}

		// (re)parse envelope values
		m->m_envRMS[0] = max(0, RmReadInt(rm, L"RMSAttack", m->m_envRMS[0]));
		m->m_envRMS[1] = max(0, RmReadInt(rm, L"RMSDecay", m->m_envRMS[1]));
		m->m_envPeak[0] = max(0, RmReadInt(rm, L"PeakAttack", m->m_envPeak[0]));
		m->m_envPeak[1] = max(0, RmReadInt(rm, L"PeakDecay", m->m_envPeak[1]));
		m->m_envFFT[0] = max(0, RmReadInt(rm, L"FFTAttack", m->m_envFFT[0]));
		m->m_envFFT[1] = max(0, RmReadInt(rm, L"FFTDecay", m->m_envFFT[1]));

		// (re)parse gain constants
		m->m_gainRMS = max(0.0, RmReadDouble(rm, L"RMSGain", m->m_gainRMS));
		m->m_gainPeak = max(0.0, RmReadDouble(rm, L"PeakGain", m->m_gainPeak));

		m->m_sensitivity = 10 * log10(m->m_fftSize);	// default dynamic range/noise floor
		m->m_sensitivity = 10 / max(1.0, RmReadDouble(rm, L"Sensitivity", m->m_sensitivity));

		// regenerate filter constants
		if (m->m_wfx)
		{
			const double freq = m->m_wfx->nSamplesPerSec;
			m->m_kRMS[0] = (float)exp(log10(0.01) / (freq * (double)m->m_envRMS[0] * 0.001));
			m->m_kRMS[1] = (float)exp(log10(0.01) / (freq * (double)m->m_envRMS[1] * 0.001));
			m->m_kPeak[0] = (float)exp(log10(0.01) / (freq * (double)m->m_envPeak[0] * 0.001));
			m->m_kPeak[1] = (float)exp(log10(0.01) / (freq * (double)m->m_envPeak[1] * 0.001));

			if (m->m_fftSize)
			{
				m->m_kFFT[0] = (float)exp(log10(0.01) / (freq * 0.001 * (double)m->m_envFFT[0] * 0.001));
				m->m_kFFT[1] = (float)exp(log10(0.01) / (freq * 0.001 * (double)m->m_envFFT[1] * 0.001));
			}
		}

		if (!m->m_updateLoopThread && m->m_updatesPerSecond != -2)
		{
			m->StartCaptureThread();
		}
		else if (m->m_updateLoopThread && m->m_updatesPerSecond == -2) 
		{
			if (m->m_hStopEvent)
			{
				SetEvent(m->m_hStopEvent);
			}
			if (m->m_updateLoopThread->joinable())
			{
				m->m_updateLoopThread->join();
			}
			delete m->m_updateLoopThread;
			m->m_updateLoopThread = NULL;
		}
	}

	// parse FFT index request
	m->m_fftIdx = max(0, RmReadInt(rm, L"FFTIdx", m->m_fftIdx));
	m->m_fftIdx = m->m_parent ?
		min(m->m_parent->m_fftBufferSize / 2, m->m_fftIdx) :
		min(m->m_fftBufferSize / 2, m->m_fftIdx);

	// parse WAVE index request
	m->m_waveIdx = max(0, RmReadInt(rm, L"WaveIdx", m->m_waveIdx));
	m->m_waveIdx = m->m_parent ?
		min(m->m_parent->m_waveSize, m->m_waveIdx) :
		min(m->m_waveSize, m->m_waveIdx);

	// parse band index request
	m->m_bandIdx = max(0, RmReadInt(rm, L"BandIdx", m->m_bandIdx));
	m->m_bandIdx = m->m_parent ?
		min(m->m_parent->m_nBands, m->m_bandIdx) :
		min(m->m_nBands, m->m_bandIdx);
}


/**
* Update the measure.
*
* @param[in]	data			Measure instance pointer.
* @return		Latest value - typically an audio level between 0.0 and 1.0.
*/
PLUGIN_EXPORT double Update(void* data)
{
	Measure* m = (Measure*)data;
	Measure* parent = m->m_parent ? m->m_parent : m;

	// rainmeter style update loop - not recommended
	if (!m->m_parent && m->m_updatesPerSecond == -2)
	{
		HRESULT hr = m->UpdateParent();

		switch (hr)
		{
		case S_OK:
			// everything is fine, update measures
			break;
		case S_FALSE:
			// silence detected, but ignore to still update other types of measures like FFTFREQ
			break;
		case AUDCLNT_E_BUFFER_ERROR:
		case AUDCLNT_E_DEVICE_INVALIDATED:
		case AUDCLNT_E_SERVICE_NOT_RUNNING:
			m->ReinitializeDevice();
			return 0.0;
		}
	}

	std::lock_guard<std::mutex> lock(parent->m_mutex);

	switch (m->m_type)
	{
	case Measure::TYPE_BAND:
		if (parent->m_clCapture && parent->m_nBands && m->m_bandIdx < parent->m_nBands)
		{
			return parent->m_bandOut[m->m_bandIdx];
		}
		break;
	case Measure::TYPE_WAVEBAND:
		if (parent->m_clCapture && parent->m_nBands && parent->m_waveSize && m->m_bandIdx < parent->m_nBands)
		{
			return parent->m_waveBandOut[m->m_bandIdx];
		}
		break;
	case Measure::TYPE_FFT:
		if (parent->m_clCapture && parent->m_fftBufferSize && m->m_fftIdx < parent->m_fftBufferSize)
		{
			return max(0, parent->m_sensitivity * log10(CLAMP01(parent->m_fftOut[m->m_fftIdx])) + 1.0);
		}
		break;
	case Measure::TYPE_FFTFREQ:
		if (parent->m_clCapture && parent->m_fftBufferSize && m->m_fftIdx <= (parent->m_fftBufferSize * 0.5))
		{
			return ((float)m->m_fftIdx) * parent->m_df;
		}
		break;

	case Measure::TYPE_BANDFREQ:
		if (parent->m_clCapture && parent->m_nBands && m->m_bandIdx < parent->m_nBands)
		{
			return parent->m_bandFreq[m->m_bandIdx];
		}
		break;
	case Measure::TYPE_WAVE:
		if (parent->m_clCapture && parent->m_waveSize && m->m_waveIdx < parent->m_waveSize)
		{
			return parent->m_waveOut[m->m_waveIdx];
		}
		break;
	case Measure::TYPE_RMS:
		if (parent->m_clCapture && parent->m_rms)
		{
			return CLAMP01(sqrt(parent->m_rms[m->m_channel]) * parent->m_gainRMS);
		}
		break;
	case Measure::TYPE_PEAK:
		if (parent->m_clCapture && parent->m_peak)
		{
			return CLAMP01(parent->m_peak[m->m_channel] * parent->m_gainPeak);
		}
		break;
	case Measure::TYPE_DEV_STATUS:
		if (parent->m_dev)
		{
			DWORD state;
			if (parent->m_dev->GetState(&state) == S_OK && state == DEVICE_STATE_ACTIVE)
			{
				return 1.0;
			}
		}
		break;
	case Measure::TYPE_BUFFERSTATUS:
		if (parent->m_nFramesNext > 0)
		{
			return parent->m_nFramesNext;
		}
		break;
	}

	return 0.0;
}


/**
* Indicates that the application working directory will not be reset by the plugin.
*/
PLUGIN_EXPORT void OverrideDirectory()
{
}


/**
* Get a string value from the measure.
*
* @param[in]	data			Measure instance pointer.
* @return		String value - must be copied out by the caller.
*/
PLUGIN_EXPORT LPCWSTR GetString(void* data)
{
	Measure* m = (Measure*)data;
	Measure* parent = m->m_parent ? m->m_parent : m;

	static WCHAR buffer[4096];
	const WCHAR* s_fmtName[Measure::NUM_FORMATS] =
	{
		L"<invalid>",	// FMT_INVALID
		L"PCM 16b",		// FMT_PCM_S16
		L"PCM 32b",		// FMT_PCM_F32
	};

	buffer[0] = '\0';
	std::lock_guard<std::mutex> lock(parent->m_mutex);

	switch (m->m_type)
	{
	default:
		// return NULL for any numeric values, so Rainmeter can auto-convert them.
		return NULL;

	case Measure::TYPE_FORMAT:
		if (parent->m_wfx)
		{
			_snwprintf_s(buffer, _TRUNCATE, L"%dHz %s %dch", parent->m_wfx->nSamplesPerSec,
				s_fmtName[parent->m_format], parent->m_wfx->nChannels);
		}
		break;

	case Measure::TYPE_DEV_NAME:
		return parent->m_devName;

	case Measure::TYPE_DEV_ID:
		if (parent->m_dev)
		{
			LPWSTR pwszID = NULL;
			if (parent->m_dev->GetId(&pwszID) == S_OK)
			{
				_snwprintf_s(buffer, _TRUNCATE, L"%s", pwszID);
				CoTaskMemFree(pwszID);
			}
		}
		break;

	case Measure::TYPE_DEV_LIST:
		if (parent->m_enum)
		{
			IMMDeviceCollection* collection = NULL;
			if (parent->m_enum->EnumAudioEndpoints(parent->m_port == Measure::PORT_OUTPUT ? eRender : eCapture,
				DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &collection) == S_OK)
			{
				WCHAR* d = &buffer[0];
				UINT nDevices;
				collection->GetCount(&nDevices);

				for (ULONG iDevice = 0; iDevice < nDevices; ++iDevice)
				{
					IMMDevice* device = NULL;
					IPropertyStore* props = NULL;
					if (collection->Item(iDevice, &device) == S_OK && device->OpenPropertyStore(STGM_READ, &props) == S_OK)
					{
						LPWSTR id = NULL;
						PROPVARIANT	varName;
						PropVariantInit(&varName);

						if (device->GetId(&id) == S_OK && props->GetValue(PKEY_Device_FriendlyName, &varName) == S_OK)
						{
							d += _snwprintf_s(d, _countof(buffer) - (d - buffer), _TRUNCATE,
								L"%s%s: %s", iDevice > 0 ? L"\n" : L"", id, varName.pwszVal);
						}

						if (id) CoTaskMemFree(id);

						PropVariantClear(&varName);
					}

					SAFE_RELEASE(props);
					SAFE_RELEASE(device);
				}
			}

			SAFE_RELEASE(collection);
		}
		break;
	}

	return buffer;
}

UINT32 Measure::EmptyPacketSilenceThreshold() const
{
	if (m_updatesPerSecond > 0)
	{
		return (UINT32)max(1.0, ceil(m_updatesPerSecond * 0.3));
	}

	if (m_updatesPerSecond == -2)
	{
		return 1;
	}

	return 6;
}

void Measure::ZeroOutputBuffers()
{
	if (m_bandOut && m_nBands)
	{
		memset(m_bandOut, 0, m_nBands * sizeof(float));
	}
	if (m_fftOut && m_fftBufferSize)
	{
		memset(m_fftOut, 0, m_fftBufferSize * sizeof(float));
	}
	if (m_waveOut && m_waveSize)
	{
		memset(m_waveOut, 0, m_waveSize * sizeof(float));
	}
	if (m_waveBandOut && m_nBands)
	{
		memset(m_waveBandOut, 0, m_nBands * sizeof(float));
	}

	for (int iChan = 0; iChan < MAX_CHANNELS; ++iChan)
	{
		m_rms[iChan] = 0.0f;
		m_peak[iChan] = 0.0f;
	}

	m_outputsSilenced = true;
}

HRESULT Measure::UpdateParent()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_clCapture || !m_wfx || !m_bufChunk)
	{
		return E_FAIL;
	}

	BYTE* buffer;
	UINT32 nFrames, nFramesNext;
	DWORD  flags;

	HRESULT hr = m_clCapture->GetNextPacketSize(&nFramesNext);
	m_nFramesNext = nFramesNext;
	if (hr == S_OK)
	{
		if (nFramesNext <= 0)
		{
			++m_nEmptyPacketCycles;
			if (m_nEmptyPacketCycles >= EmptyPacketSilenceThreshold())
			{
				if (!m_outputsSilenced)
				{
					ZeroOutputBuffers();
					return S_OK;
				}

				return S_FALSE;
			}

			return S_FALSE;
		}

		m_nEmptyPacketCycles = 0;
		m_outputsSilenced = false;
		
		while (m_clCapture->GetBuffer(&buffer, &nFrames, &flags, NULL, NULL) == S_OK)
		{
			// if not F32, convert to F32
			if (m_format == Measure::FMT_PCM_F32)
			{
				memcpy(&m_bufChunk[0], &buffer[0], nFrames * m_wfx->nBlockAlign);
			}
			else if (m_format == Measure::FMT_PCM_S16)
			{
				INT16* buf = (INT16*)buffer;
				for (int iPcm = 0; iPcm < nFrames * m_wfx->nChannels; ++iPcm)
				{
					m_bufChunk[iPcm] = (float)buf[iPcm] * pcmScalar;
				}
			}

			// release buffer immediately to resume capture
			m_clCapture->ReleaseBuffer(nFrames);

			// first silent check result (to process in the second silent check)
			bool firstSilentCheckPassed = false;

			// first test for discontinuity or silence (using audioclient flags)
			if (flags & AUDCLNT_BUFFERFLAGS_SILENT) 
			{
				// is the ring buffer filled with silence? then stop updating
				if (m_nSilentFrames > m_ringBufferSize) 
				{
					return S_FALSE;
				}
				else 
				{
					// reset rms/peak because its silent
					for (int iChan = 0; iChan < MAX_CHANNELS; ++iChan)
					{
						m_rms[iChan] = 0.0;
						m_peak[iChan] = 0.0;
					}

					m_nSilentFrames += nFrames;
				}
			}
			else if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
			{
				// not sure what to do with those frames... ignore them? use them? treat as silent frames?
				// for now, ignore.
				//continue;
				firstSilentCheckPassed = true;
			}
			else 
			{
				// audio data is not silent, reset silent frames counter
				firstSilentCheckPassed = true;
			}

			if (m_ringBufferSize)
			{
				// store data in ring buffers, demux streams, and measure RMS and peak levels
				const int channelsToProcess = min((int)m_wfx->nChannels, (int)Measure::CHANNEL_SUM);
				for (int iFrame = 0; iFrame < (int)(nFrames * m_wfx->nChannels);)
				{
					for (int iChan = 0; iChan < (int)m_wfx->nChannels; ++iChan)
					{
						// store data in ring buffers, and demux streams
						if (m_channel == Measure::CHANNEL_SUM)
						{
							if (iChan == Measure::CHANNEL_FL)
							{
								// cannot increment before evaluation
								const float L = m_bufChunk[iFrame];

								// stereo to mono: (L + R) / 2; mono devices use the first channel.
								m_ringBuffer[m_ringBufW] = m_wfx->nChannels > 1 ? 0.5f * (L + m_bufChunk[iFrame + 1]) : L;
							}
						}
						else if (iChan == m_channel)
						{
							m_ringBuffer[m_ringBufW] = m_bufChunk[iFrame];
						}
						else { }	// move along the raw data buffer

						// measure RMS and peak levels
						float x = (float)m_bufChunk[iFrame++];
						float sqrX = x * x;
						float absX = abs(x);
						if (iChan < channelsToProcess)
						{
							m_rms[iChan] = sqrX + m_kRMS[(sqrX < m_rms[iChan])] * (m_rms[iChan] - sqrX);
							m_peak[iChan] = absX + m_kPeak[(absX < m_peak[iChan])] * (m_peak[iChan] - absX);
						}
					}
					m_ringBufW = (m_ringBufW + 1) % m_ringBufferSize;	// move along the data-to-process buffer
				}
			}
			else if (m_type == Measure::TYPE_RMS || m_type == Measure::TYPE_PEAK) 
			{
				// measure RMS and peak levels
				// loops unrolled for float, 16b and mono, stereo
				const int channelsToProcess = min((int)m_wfx->nChannels, (int)Measure::CHANNEL_SUM);
				for (int iFrame = 0; iFrame < (int)(nFrames * m_wfx->nChannels);)
				{
					for (int iChan = 0; iChan < (int)m_wfx->nChannels; ++iChan)
					{
						float x = (float)m_bufChunk[iFrame++];
						float sqrX = x * x;
						float absX = abs(x);
						if (iChan < channelsToProcess)
						{
							m_rms[iChan] = sqrX + m_kRMS[(sqrX < m_rms[iChan])] * (m_rms[iChan] - sqrX);
							m_peak[iChan] = absX + m_kPeak[(absX < m_peak[iChan])] * (m_peak[iChan] - absX);
						}
					}
				}
			}

			// rms and peak values for sum channel
			if (m_wfx->nChannels >= 2)
			{
				m_rms[Measure::CHANNEL_SUM] = (m_rms[Measure::CHANNEL_FL] + m_rms[Measure::CHANNEL_FR]) * 0.5f;
				m_peak[Measure::CHANNEL_SUM] = (m_peak[Measure::CHANNEL_FL] + m_peak[Measure::CHANNEL_FR]) * 0.5f;
			}
			else
			{
				m_rms[Measure::CHANNEL_SUM] = m_rms[Measure::CHANNEL_FL];
				m_peak[Measure::CHANNEL_SUM] = m_peak[Measure::CHANNEL_FL];
			}

			if (firstSilentCheckPassed)
			{
				// second silent check (using rms)
				if ((m_rms[Measure::CHANNEL_SUM]) <= 0.0000001F)
				{
					if (m_nSilentFrames > m_ringBufferSize)
					{
						return S_FALSE;
					}
					else 
					{
						m_nSilentFrames += nFrames;
					}
				}
				else
				{
					m_nSilentFrames = 0;
				}
			}
		}

		// process FFTs
		if (m_ringBufferSize)
		{
			// copy from the circular ring buffer to temp space
			memcpy(&m_ringBufOut[0], &m_ringBuffer[m_ringBufW], (m_ringBufferSize - m_ringBufW) * sizeof(float));
			memcpy(&m_ringBufOut[m_ringBufferSize - m_ringBufW], &m_ringBuffer[0], m_ringBufW * sizeof(float));

			if (m_waveSize)
			{
				// copy waveform into wave output buffer
				memcpy(&m_waveOut[0], &m_ringBufOut[m_ringBufferSize - m_waveSize], m_waveSize * sizeof(float));
			}

			if (m_fftSize)
			{
				if (m_dynamicVolume) 
				{
					m_fftMeanSquare = 0.0f;
					// apply the windowing function and calculate fft sized mean square
					for (int iBin = 0; iBin < m_fftSize; ++iBin)
					{
						int src = m_ringBufferSize - m_fftSize + iBin;
						m_fftMeanSquare += m_ringBufOut[src] * m_ringBufOut[src];
						m_ringBufOut[src] *= m_fftKWdw[iBin];
					}
					m_fftMeanSquare = m_fftMeanSquare / m_fftSize;
					m_fftMeanSquare *= 10.0F;
				}
				else 
				{
					// apply the windowing function
					for (int iBin = 0; iBin < m_fftSize; ++iBin)
					{
						int src = m_ringBufferSize - m_fftSize + iBin;
						m_ringBufOut[src] *= m_fftKWdw[iBin];
					}
				}

				pffft_transform_ordered(m_fftCfg, &m_ringBufOut[m_ringBufferSize - m_fftSize], m_fftTmpOut, NULL, pffft_direction_t::PFFFT_FORWARD);

				int ifftBin;
				for (int iBin = 0; iBin < m_fftBufferSize; ++iBin)
				{
					ifftBin = iBin * 2;

					// old and new values
					float x0 = m_fftOut[iBin];
					const float x1 = (m_fftTmpOut[ifftBin] * m_fftTmpOut[ifftBin] + m_fftTmpOut[ifftBin + 1] * m_fftTmpOut[ifftBin + 1]) * m_fftScalar;

					x0 = x1 + m_kFFT[(x1 < x0)] * (x0 - x1);		// attack/decay filter
					m_fftOut[iBin] = x0;
				}
			}
		}

		if (m_nBands)
		{
			// dynamic volume: same band values for low- and high-volume music
			// if there are silent frames in the buffer, dont regulate the volume to allow a smooth fading into silence
			float volumeScalar = 1;
			float volumeScalar2 = 1;
			if (m_dynamicVolume && m_nSilentFrames <= 0)
			{
				//volumeScalar = m_rms[m_channel] > 0 ? (1 / (min(1, m_rms[m_channel] * 10))) : 1;
				volumeScalar = m_fftMeanSquare > 0 ? (1 / (min(1, m_fftMeanSquare))) : 1;
				//volumeScalar2 = m_fftMeanSquare > 0 ? 1 / min(1, sqrt(m_fftMeanSquare)) : 1; // WIP
			}

			// integrate waveform into lin-scale frequency bands
			if (m_waveSize)
			{
				int iBin = 0;
				int iBand = 0;
				float w0 = 0.0f;

				// use a temp buffer if smoothing is enabled, otherwise skip temp buffer
				float* ptrWaveBuffer = m_smoothing ? m_waveBandTmpOut : m_waveBandOut;
				memset(ptrWaveBuffer, 0, m_nBands * sizeof(float));

				while (iBin < m_waveSize && iBand < m_nBands)
				{
					const float wLin1 = iBin;
					const float bLin1 = m_dw * (iBand + 1);
					float& y = ptrWaveBuffer[iBand];

					if (wLin1 < bLin1)
					{
						y += (wLin1 - w0) * (m_waveOut[iBin]);
						w0 = wLin1;
						iBin += 1;
					}
					else
					{
						y += (bLin1 - w0) * (m_waveOut[iBin]);
						y *= m_waveScalar * volumeScalar2 * 0.5f;
						y += 0.5f;
						w0 = bLin1;
						iBand += 1;
					}
				}

				// smoothing
				// calculate the average of the band indexes iBand-n to iBand+n (n = m_smoothing)
				if (m_smoothing)
				{
					for (iBand = 0; iBand < m_nBands; iBand++)
					{
						float x = 0;
						for (int s = -m_smoothing; s <= m_smoothing; s++)
						{
							x += (iBand + s < 0) || (iBand + s >= m_nBands) ? 0.5f : ptrWaveBuffer[iBand + s];
						}
						m_waveBandOut[iBand] = x * m_smoothingScalar;
					}
				}
			}

			// integrate FFT results into log-scale frequency bands
			if (m_fftSize)
			{
				int iBin = (int)ceilf(m_freqMin / m_df);
				int iBand = 0;
				float f0 = m_freqMin;

				// use a temp buffer if smoothing is enabled, otherwise skip temp buffer
				float* ptrBandBuffer = m_smoothing ? m_bandTmpOut : m_bandOut;
				memset(ptrBandBuffer, 0, m_nBands * sizeof(float));

				while (iBin <= (m_fftBufferSize * 0.5f) && iBand < m_nBands)
				{
					const float fLin1 = ((float)iBin) * m_df;
					const float fLog1 = m_bandFreq[iBand];
					float& y = ptrBandBuffer[iBand];

					if (fLin1 <= fLog1)
					{
						y += (fLin1 - f0) * m_fftOut[iBin];
						f0 = fLin1;
						iBin += 1;
					}
					else
					{
						y += (fLog1 - f0) * m_fftOut[iBin];
						y *= m_bandScalar; // scaling
						y *= volumeScalar; // dynamic volume
						y = max(0, m_sensitivity * log10(CLAMP01(y)) + 1.0); // sensitivity
						f0 = fLog1;
						iBand += 1;
					}
				}

				// smoothing
				// calculate the average of the band indexes iBand-n to iBand+n (n = m_smoothing)
				if (m_smoothing)
				{
					// smoothingMode decides what to do at values bigger than nBands or smaller than 0
					if (m_smoothingMode == 0) 
					{
						// clamp: 1,2,3 at ends 3,3,3
						for (iBand = 0; iBand < m_nBands; iBand++)
						{
							float x = 0;
							for (int s = -m_smoothing; s <= m_smoothing; s++)
							{
								x += m_bandTmpOut[(iBand + s < 0) || (iBand + s >= m_nBands) ? iBand : iBand + s];
							}
							m_bandOut[iBand] = x * m_smoothingScalar;
						}
					}
					else if (m_smoothingMode == 1)
					{
						// repeat: 1,2,3 at ends 1,2,3
						for (iBand = 0; iBand < m_nBands; iBand++)
						{
							float x = 0;
							for (int s = -m_smoothing; s <= m_smoothing; s++)
							{
								int i = iBand + s < 0 ? m_nBands + s : iBand + s;
								i = i >= m_nBands ? s : i;
								x += m_bandTmpOut[i];
							}
							m_bandOut[iBand] = x * m_smoothingScalar;
						}
					}
					else // if (m_smoothingMode == 2)
					{
						// repeat reverse: 1,2,3 at ends: 3,2,1
						for (iBand = 0; iBand < m_nBands; iBand++)
						{
							float x = 0;
							for (int s = -m_smoothing; s <= m_smoothing; s++)
							{
								int i = iBand + s < 0 ? -s : iBand + s;
								i = i >= m_nBands ? m_nBands - s : i;
								x += m_bandTmpOut[i];
							}
							m_bandOut[iBand] = x * m_smoothingScalar;
						}
					}
				}
			}
		}
	}

	return hr;
}


/**
* Try to initialize the default device for the specified port.
*
* @return		Result value, S_OK on success.
*/
HRESULT DefaultDeviceNotificationClient::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId)
{
	if (m_measure)
	{
		m_measure->OnDefaultDeviceChanged(flow, role, pwstrDefaultDeviceId);
	}
	return S_OK;
}

HRESULT Measure::SelectDevice()
{
	if (!m_enum)
	{
		m_dev = NULL;
		return E_FAIL;
	}

	SAFE_RELEASE(m_dev);
	m_usingDefaultDevice = false;

	if (*m_reqID)
	{
		HRESULT hr = m_enum->GetDevice(m_reqID, &m_dev);
		if (hr == S_OK && m_dev)
		{
			return S_OK;
		}

		m_dev = NULL;
		RmLogF(m_rm, LOG_WARNING, L"Audio %s device '%s' not found (error 0x%08x). Falling back to the default device.",
			m_port == Measure::PORT_OUTPUT ? L"output" : L"input", m_reqID, hr);
	}

	m_usingDefaultDevice = true;
	return m_enum->GetDefaultAudioEndpoint(m_port == Measure::PORT_OUTPUT ? eRender : eCapture, eConsole, &m_dev);
}

void Measure::ReleaseAnalysisBuffers()
{
	if (m_fftCfg) pffft_destroy_setup(m_fftCfg);
	m_fftCfg = NULL;

	if (m_ringBuffer) free(m_ringBuffer);
	m_ringBuffer = NULL;

	if (m_fftOut) free(m_fftOut);
	m_fftOut = NULL;

	if (m_fftKWdw) free(m_fftKWdw);
	m_fftKWdw = NULL;

	if (m_ringBufOut) free(m_ringBufOut);
	m_ringBufOut = NULL;

	if (m_fftTmpOut) free(m_fftTmpOut);
	m_fftTmpOut = NULL;

	if (m_bandFreq) free(m_bandFreq);
	m_bandFreq = NULL;

	if (m_bandOut) free(m_bandOut);
	m_bandOut = NULL;

	if (m_bandTmpOut) free(m_bandTmpOut);
	m_bandTmpOut = NULL;

	if (m_waveBandOut) free(m_waveBandOut);
	m_waveBandOut = NULL;

	if (m_waveOut) free(m_waveOut);
	m_waveOut = NULL;

	if (m_waveBandTmpOut) free(m_waveBandTmpOut);
	m_waveBandTmpOut = NULL;

	m_ringBufW = 0;
	m_fftMeanSquare = 0.0f;
	m_nEmptyPacketCycles = 0;
	m_outputsSilenced = true;
}

HRESULT Measure::InitializeAnalysisBuffers()
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (!m_wfx)
	{
		return S_FALSE;
	}

	m_nEmptyPacketCycles = 0;
	m_outputsSilenced = true;

	if (m_ringBufferSize)
	{
		m_ringBuffer = (float*)calloc(m_ringBufferSize, sizeof(float));
		m_ringBufOut = (float*)calloc(max(m_ringBufferSize, m_fftBufferSize), sizeof(float));
		if (!m_ringBuffer || !m_ringBufOut)
		{
			return E_OUTOFMEMORY;
		}
	}

	if (m_fftSize)
	{
		m_fftKWdw = (float*)calloc(m_fftSize, sizeof(float));
		m_fftCfg = pffft_new_setup(m_fftBufferSize, pffft_transform_t::PFFFT_REAL);
		m_fftTmpOut = (float*)calloc(m_fftBufferSize * 2, sizeof(float));
		m_fftOut = (float*)calloc(m_fftBufferSize, sizeof(float));
		if (!m_fftKWdw || !m_fftCfg || !m_fftTmpOut || !m_fftOut)
		{
			return E_OUTOFMEMORY;
		}

		m_fftScalar = (float)(1.0 / sqrt(m_fftSize));
		m_df = (float)m_wfx->nSamplesPerSec / m_fftBufferSize;

		for (int iBin = 1; iBin < m_fftSize; ++iBin)
		{
			m_fftKWdw[iBin] = (float)(0.5 * (1.0 - cos(TWOPI * iBin / (m_fftSize + 1))));
		}
		m_fftKWdw[0] = 0.0f;

		if (m_nBands)
		{
			if (m_freqMin <= 0.0 || m_freqMax <= m_freqMin)
			{
				RmLog(m_rm, LOG_ERROR, L"Invalid frequency range: FreqMin must be > 0 and FreqMax must be greater than FreqMin.");
				return E_INVALIDARG;
			}

			m_bandFreq = (float*)malloc(m_nBands * sizeof(float));
			m_bandOut = (float*)calloc(m_nBands, sizeof(float));
			if (!m_bandFreq || !m_bandOut)
			{
				return E_OUTOFMEMORY;
			}

			const double step = pow(2.0, (log(m_freqMax / m_freqMin) / m_nBands) / log(2.0));
			m_bandFreq[0] = (float)(m_freqMin * step);
			m_bandScalar = 2.0f / (float)m_wfx->nSamplesPerSec;
			for (int iBand = 1; iBand < m_nBands; ++iBand)
			{
				m_bandFreq[iBand] = (float)(m_bandFreq[iBand - 1] * step);
			}

			if (m_smoothing)
			{
				m_bandTmpOut = (float*)calloc(m_nBands, sizeof(float));
				if (!m_bandTmpOut)
				{
					return E_OUTOFMEMORY;
				}
			}
		}
	}

	if (m_waveSize)
	{
		m_waveOut = (float*)calloc(m_waveSize, sizeof(float));
		if (!m_waveOut)
		{
			return E_OUTOFMEMORY;
		}

		if (m_nBands)
		{
			m_dw = (float)m_waveSize / (float)m_nBands;
			m_waveScalar = (float)(1.0f / m_dw);
			m_waveBandOut = (float*)calloc(m_nBands, sizeof(float));
			if (!m_waveBandOut)
			{
				return E_OUTOFMEMORY;
			}

			if (m_smoothing)
			{
				m_waveBandTmpOut = (float*)calloc(m_nBands, sizeof(float));
				if (!m_waveBandTmpOut)
				{
					return E_OUTOFMEMORY;
				}
			}
		}
	}

	return S_OK;
}

void Measure::StartCaptureThread()
{
	if (!m_updateLoopThread && m_hReadyEvent && m_hStopEvent && m_updatesPerSecond != -2)
	{
		m_updateLoopThread = new std::thread(&Measure::DoCaptureLoop, this);
	}
}

void Measure::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR defaultDeviceId)
{
	if (!m_usingDefaultDevice || role != eConsole)
	{
		return;
	}

	if ((m_port == PORT_OUTPUT && flow != eRender) || (m_port == PORT_INPUT && flow != eCapture))
	{
		return;
	}

	RmLog(m_rm, LOG_DEBUG, L"Default audio device changed; reinitializing AudioLevelBeta.");
	ReinitializeDevice();
}

HRESULT Measure::ReinitializeDevice()
{
	bool currentThread = m_updateLoopThread && m_updateLoopThread->get_id() == std::this_thread::get_id();
	DeviceRelease(!currentThread, true);

	HRESULT hr = SelectDevice();
	if (FAILED(hr))
	{
		RmLogF(m_rm, LOG_ERROR, L"AudioLevel: Failed to select audio device after reinitialization. HRESULT %d", (int)hr);
		return hr;
	}

	hr = DeviceInit();
	if (FAILED(hr))
	{
		return hr;
	}

	hr = InitializeAnalysisBuffers();
	if (FAILED(hr))
	{
		RmLogF(m_rm, LOG_ERROR, L"AudioLevel: Failed to initialize analysis buffers after device change. HRESULT %d", (int)hr);
		return hr;
	}

	if (!currentThread)
	{
		StartCaptureThread();
	}

	return S_OK;
}

HRESULT	Measure::DeviceInit()
{
	HRESULT hr;

	// get the device handle
	if (!m_enum || !m_dev)
	{
		RmLog(m_rm, LOG_WARNING, L"AudioLevel: Cannot initialize audio device because no endpoint is selected.");
		return E_FAIL;
	}

	// store device name
	IPropertyStore* props = NULL;
	if (m_dev->OpenPropertyStore(STGM_READ, &props) == S_OK)
	{
		PROPVARIANT	varName;
		PropVariantInit(&varName);

		if (props->GetValue(PKEY_Device_FriendlyName, &varName) == S_OK)
		{
			_snwprintf_s(m_devName, _TRUNCATE, L"%s", varName.pwszVal);
		}

		PropVariantClear(&varName);
	}

	SAFE_RELEASE(props);

	// get an extra audio client for loopback events
	hr = m_dev->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&m_clBugAudio);
	if (hr != S_OK)
	{
		RmLog(m_rm, LOG_WARNING, L"Failed to create audio client for loopback events.");
		goto Exit;
	}

	// get the main audio client
	//if (m_dev->Activate(IID_IAudioClient3, CLSCTX_ALL, NULL, (void**)&m_clAudio) != S_OK)
	//{
	if (m_dev->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&m_clAudio) != S_OK)
	{
		RmLog(m_rm, LOG_WARNING, L"Failed to create audio client.");
		goto Exit;
	}
	//}

	// parse audio format - Note: not all formats are supported.
	hr = m_clAudio->GetMixFormat(&m_wfx);
	EXIT_ON_ERROR(hr);

	m_wfxR.nChannels = m_wfx->nChannels;
	m_wfxR.nSamplesPerSec = m_wfx->nSamplesPerSec;
	m_wfxR.cbSize = 0;

	CoTaskMemFree(m_wfx);
	m_wfx = NULL;
	m_wfxAllocated = false;

	m_wfxR.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
	m_wfxR.wBitsPerSample = 32;
	m_wfxR.nBlockAlign = m_wfxR.nChannels * m_wfxR.wBitsPerSample / 8;
	m_wfxR.nAvgBytesPerSec = m_wfxR.nSamplesPerSec * m_wfxR.nBlockAlign;

	WAVEFORMATEX* closestMatch = NULL;
	hr = m_clAudio->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &m_wfxR, &closestMatch);
	if (hr == S_OK)
	{
		m_format = FMT_PCM_F32;
		m_wfx = &m_wfxR;
		m_wfxAllocated = false;
	}
	else
	{
		if (closestMatch)
		{
			CoTaskMemFree(closestMatch);
			closestMatch = NULL;
		}

		m_wfxR.wFormatTag = WAVE_FORMAT_PCM;
		m_wfxR.wBitsPerSample = 16;
		m_wfxR.nBlockAlign = m_wfxR.nChannels * m_wfxR.wBitsPerSample / 8;
		m_wfxR.nAvgBytesPerSec = m_wfxR.nSamplesPerSec * m_wfxR.nBlockAlign;

		hr = m_clAudio->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &m_wfxR, &closestMatch);
		if (hr == S_OK)
		{
			m_format = FMT_PCM_S16;
			m_wfx = &m_wfxR;
			m_wfxAllocated = false;
		}
		else
		{
			if (closestMatch)
			{
				CoTaskMemFree(closestMatch);
				closestMatch = NULL;
			}

			// try a standard format
			m_wfxR.nChannels = 2;
			m_wfxR.nSamplesPerSec = 48000;
			m_wfxR.nBlockAlign = m_wfxR.nChannels * m_wfxR.wBitsPerSample / 8;
			m_wfxR.nAvgBytesPerSec = m_wfxR.nSamplesPerSec * m_wfxR.nBlockAlign;

			hr = m_clAudio->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &m_wfxR, &closestMatch);
			if (hr == S_OK)
			{
				m_format = FMT_PCM_S16;
				m_wfx = &m_wfxR;
				m_wfxAllocated = false;
			}
			else
			{
				if (closestMatch)
				{
					CoTaskMemFree(closestMatch);
					closestMatch = NULL;
				}
				RmLog(m_rm, LOG_WARNING, L"Invalid sample format.  Only PCM 16b integer or PCM 32b float are supported.");
				hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
				goto Exit;
			}
		}
	}

	hr = m_clBugAudio->Initialize(
		AUDCLNT_SHAREMODE_SHARED,
		(m_updatesPerSecond != -2 ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0),		// "Each time the client receives an event for the render stream, it must signal the capture client to run"
		0,
		0,
		m_wfx,
		NULL);
	if (hr != S_OK)
	{
		RmLog(m_rm, LOG_WARNING, L"Failed to initialize audio client for loopback events.");
	}
	EXIT_ON_ERROR(hr);

	if (m_updatesPerSecond != -2)
	{
		m_hReadyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
		if (m_hReadyEvent == NULL)
		{
			RmLog(m_rm, LOG_WARNING, L"Failed to create buffer-event handle.");
			hr = E_FAIL;
			goto Exit;
		}

		m_hStopEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
		if (m_hStopEvent == NULL)
		{
			RmLog(m_rm, LOG_WARNING, L"Failed to create stop-event handle.");
			hr = E_FAIL;
			goto Exit;
		}

		hr = m_clBugAudio->SetEventHandle(m_hReadyEvent);

		EXIT_ON_ERROR(hr);
	}

#if (WINDOWS_BUG_WORKAROUND)
	// ---------------------------------------------------------------------------------------
	// Windows bug workaround: create a silent render client before initializing loopback mode
	// see: http://social.msdn.microsoft.com/Forums/windowsdesktop/en-US/c7ba0a04-46ce-43ff-ad15-ce8932c00171/loopback-recording-causes-digital-stuttering?forum=windowspro-audiodevelopment
	if (m_port == PORT_OUTPUT)
	{
		hr = m_clBugAudio->GetService(IID_IAudioRenderClient, (void**)&m_clBugRender);
		EXIT_ON_ERROR(hr);

		UINT32 nFrames;
		hr = m_clBugAudio->GetBufferSize(&nFrames);
		EXIT_ON_ERROR(hr);

		BYTE* buffer;
		hr = m_clBugRender->GetBuffer(nFrames, &buffer);
		EXIT_ON_ERROR(hr);

		hr = m_clBugRender->ReleaseBuffer(nFrames, AUDCLNT_BUFFERFLAGS_SILENT);
		EXIT_ON_ERROR(hr);
	}
	// ---------------------------------------------------------------------------------------
#endif

	hr = m_clBugAudio->Start();
	if (hr != S_OK)
	{
		RmLog(m_rm, LOG_WARNING, L"Failed to start the stream for loopback events.");
	}
	EXIT_ON_ERROR(hr);

	// initialize the audio client

	/* void* ptr = NULL;
	if (m_clAudio->QueryInterface(IID_IAudioClient3, (void**)&ptr) == S_OK)
	{
	AudioClientProperties props = { 0 };
	props.cbSize = sizeof(AudioClientProperties);
	props.bIsOffload = FALSE;
	props.eCategory = AudioCategory_Other;
	//props.Options = AUDCLNT_STREAMOPTIONS_RAW | AUDCLNT_STREAMOPTIONS_MATCH_FORMAT;

	if (((IAudioClient3*)m_clAudio)->SetClientProperties(&props) != S_OK)
	{
	RmLog(m_rm, LOG_WARNING, L"Failed to set audio client properties.");
	goto Exit;
	}

	UINT32 defFrames, funFrames, minFrames, maxFrames;
	hr = ((IAudioClient3*)m_clAudio)->GetSharedModeEnginePeriod(m_wfx, &defFrames, &funFrames, &minFrames, &maxFrames);
	EXIT_ON_ERROR(hr);

	// 0x88890021 AUDCLNT_E_INVALID_STREAM_FLAG - Loopback not supported?
	hr = ((IAudioClient3*)m_clAudio)->InitializeSharedAudioStream((m_port == PORT_OUTPUT ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0)
	, minFrames, m_wfx, NULL);
	if (hr != S_OK)
	{
	RmLog(m_rm LOG_WARNING, L"Failed to initialize audio client (3).");
	goto Exit;
	}
	} else */

	if (m_clAudio->Initialize(
		AUDCLNT_SHAREMODE_SHARED,
		(m_port == PORT_OUTPUT ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0),
		0,
		0,
		m_wfx,
		NULL) != S_OK)
	{
		RmLog(m_rm, LOG_WARNING, L"Failed to initialize loopback audio client.");
		goto Exit;
	}

	// initialize the audio capture client
	hr = m_clAudio->GetService(IID_IAudioCaptureClient, (void**)&m_clCapture);
	if (hr != S_OK)
	{
		RmLog(m_rm, LOG_WARNING, L"Failed to create audio capture client.");
	}
	EXIT_ON_ERROR(hr);

	// start the stream
	hr = m_clAudio->Start();
	if (hr != S_OK)
	{
		RmLog(m_rm, LOG_WARNING, L"Failed to start the stream.");
	}
	EXIT_ON_ERROR(hr);

	// allocate buffer for latest data chunk copy
	UINT32 nMaxFrames;
	hr = m_clAudio->GetBufferSize(&nMaxFrames);
	if (hr != S_OK)
	{
		RmLog(m_rm, LOG_WARNING, L"Failed to determine max buffer size.");
	}
	EXIT_ON_ERROR(hr);

	m_bufChunk = (float*)calloc(nMaxFrames * m_wfx->nChannels, sizeof(float));
	if (!m_bufChunk)
	{
		hr = E_OUTOFMEMORY;
		goto Exit;
	}

	return S_OK;

Exit:
	DeviceRelease();
	RmLogF(m_rm, LOG_ERROR, L"AudioLevel: Failed with HRESULT  %d", (int)hr);
	return hr;
}


/**
* Release handles to audio resources.  (except the enumerator)
*/
void Measure::DeviceRelease(bool stopThread, bool releaseProcessing)
{
	bool currentThread = m_updateLoopThread && m_updateLoopThread->get_id() == std::this_thread::get_id();
	if (stopThread && m_updateLoopThread && !currentThread)
	{
		if (m_hStopEvent)
		{
			SetEvent(m_hStopEvent);
		}
		if (m_updateLoopThread->joinable())
		{
			m_updateLoopThread->join();
		}
		delete m_updateLoopThread;
		m_updateLoopThread = NULL;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	RmLog(m_rm, LOG_DEBUG, L"Releasing dummy stream audio device.");
	if (m_clBugAudio)
	{
		m_clBugAudio->Stop();
	}
#if (WINDOWS_BUG_WORKAROUND)
	SAFE_RELEASE(m_clBugRender);
#endif
	SAFE_RELEASE(m_clBugAudio);

	RmLog(m_rm, LOG_DEBUG, L"Releasing audio device.");

	if (m_clAudio)
	{
		m_clAudio->Stop();
	}

	SAFE_RELEASE(m_clCapture);
	SAFE_RELEASE(m_clAudio);
	SAFE_RELEASE(m_dev);

	if (m_hReadyEvent != NULL) { CloseHandle(m_hReadyEvent); m_hReadyEvent = NULL; }
	if (m_hStopEvent != NULL) { CloseHandle(m_hStopEvent); m_hStopEvent = NULL; }

	if (m_bufChunk) free(m_bufChunk);
	m_bufChunk = NULL;
	m_nFramesNext = 0;
	m_nSilentFrames = 0;
	m_nEmptyPacketCycles = 0;
	m_outputsSilenced = true;

	for (int iChan = 0; iChan < Measure::MAX_CHANNELS; ++iChan)
	{
		m_rms[iChan] = 0.0;
		m_peak[iChan] = 0.0;
	}

	if (m_wfxAllocated && m_wfx)
	{
		CoTaskMemFree(m_wfx);
	}
	m_wfx = NULL;
	m_wfxAllocated = false;

	if (releaseProcessing)
	{
		ReleaseAnalysisBuffers();
	}

	m_devName[0] = '\0';
	m_format = FMT_INVALID;
}
