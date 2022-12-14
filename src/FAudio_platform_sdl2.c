/* FAudio - XAudio Reimplementation for FNA
 *
 * Copyright (c) 2011-2022 Ethan Lee, Luigi Auriemma, and the MonoGame Team
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from
 * the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software in a
 * product, an acknowledgment in the product documentation would be
 * appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Ethan "flibitijibibo" Lee <flibitijibibo@flibitijibibo.com>
 *
 */

#ifndef FAUDIO_WIN32_PLATFORM

#include "FAudio_internal.h"

#include <SDL.h>

#ifdef FNA_USE_CUBEB_FOR_AUDIO
#include <cubeb/cubeb.h>
#endif

#if !SDL_VERSION_ATLEAST(2, 24, 0)
#error "SDL version older than 2.24.0"
#endif /* !SDL_VERSION_ATLEAST */

/* Mixer Thread */

void FAudio_UTF8_To_UTF16(const char* src, uint16_t* dst, size_t len);

#ifdef FNA_USE_CUBEB_FOR_AUDIO
typedef struct FRingBuffer {
	uint8_t* buffer;
	size_t pointer_push;
	size_t pointer_pop;
	size_t max;
	size_t available;
} FRingBuffer;

static uint8_t FRingBuffer_Init(FRingBuffer* ring, size_t size) {
	if (!ring) {
		return 0;
	}

	ring->buffer = (uint8_t*)malloc(size);
	ring->pointer_push = 0;
	ring->pointer_pop = 0;
	ring->available = 0;
	ring->max = size;

	if (!ring->buffer) {
		SDL_Log("No memory to allocate ring buffer!");
		return 0;
	}

	return 1;
}

static void FRingBuffer_Free(FRingBuffer* ring) {
	free(ring->buffer);
}

static void FRingBuffer_Push(FRingBuffer* ring, const void* data, size_t size) {
	if (ring->max < size) {
		SDL_Log("Pushing too much for one ring buffer push session!");
		return;
	}

	ring->available += size;

	size_t firstPassPushSize = size;
	if (ring->pointer_push + size > ring->max) {
		firstPassPushSize = ring->max - ring->pointer_push;
	}

	SDL_memcpy(ring->buffer + ring->pointer_push, data, firstPassPushSize);
	if (firstPassPushSize == size) {
		ring->pointer_push += size;
		return;
	}

	size_t secondPassPushSize = size - firstPassPushSize;
	SDL_memcpy(ring->buffer, (uint8_t*)data + firstPassPushSize, secondPassPushSize);
	ring->pointer_push = secondPassPushSize;
}

static size_t FRingBuffer_UnreadByteCount(FRingBuffer* ring) {
	return ring->available;
}

static void FRingBuffer_Pop(FRingBuffer* ring, void* data, size_t* count) {
	if (!ring || !count) {
		SDL_Log("One of the argument in pop is null!");
		return;
	}

	size_t actualPopCount = FAudio_min(FAudio_min(*count, ring->available), ring->max);
	size_t popCountPass1 = FAudio_min(actualPopCount, ring->max - ring->pointer_pop);

	SDL_memcpy(data, ring->buffer + ring->pointer_pop, popCountPass1);

	if (popCountPass1 != actualPopCount) {
		size_t popCountPass2 = actualPopCount - popCountPass1;

		SDL_memcpy((uint8_t*)data + popCountPass1, ring->buffer, popCountPass2);
		ring->pointer_pop = popCountPass2;
	}
	else {
		ring->pointer_pop += actualPopCount;
	}

	ring->available -= actualPopCount;
	*count = actualPopCount;
}

static cubeb* CubebContext = NULL;
static cubeb_device_collection CubebDeviceCollection;
static int CubebContextRefCount = 0;

typedef struct FCubebAudioStream {
	cubeb_stream* stream;
	FRingBuffer ringBuffer;
	int channelCount;
	float* tempBuffer;
} FCubebAudioStream;

static void FNA_Internal_StateCallback(cubeb_stream* stream, void* user_data, cubeb_state state) {
}

static long FAudio_INTERNAL_MixCallback(cubeb_stream* stm, void* user,
	const void* input_buffer, void* output_buffer, long nframes)
{
	FAudio* audio = (FAudio*)user;
	FCubebAudioStream* fcubeb = (FCubebAudioStream*)audio->platform;

	FAudio_zero(output_buffer, nframes * fcubeb->channelCount * sizeof(float));

	if (audio->active)
	{
		const size_t goalFrames = (size_t)nframes;
		size_t accumulatedFrames = 0;

		float* outPointer = (float*)output_buffer;

		if (FRingBuffer_UnreadByteCount(&fcubeb->ringBuffer) != 0) {
			accumulatedFrames = goalFrames * fcubeb->channelCount * sizeof(float);
			FRingBuffer_Pop(&fcubeb->ringBuffer, outPointer, &accumulatedFrames);

			outPointer += accumulatedFrames / sizeof(float);
			accumulatedFrames /= (fcubeb->channelCount * sizeof(float));
		}

		while (accumulatedFrames < goalFrames) {
			if (accumulatedFrames + audio->updateSize > goalFrames) {
				if (!fcubeb->tempBuffer) {
					fcubeb->tempBuffer = (float*)malloc(audio->updateSize * fcubeb->channelCount * sizeof(float));
				}

				FAudio_zero(fcubeb->tempBuffer, audio->updateSize * fcubeb->channelCount * sizeof(float));

				FAudio_INTERNAL_UpdateEngine(
					audio,
					fcubeb->tempBuffer
				);

				size_t framesToUse = goalFrames - accumulatedFrames;
				SDL_memcpy(outPointer, fcubeb->tempBuffer, framesToUse * fcubeb->channelCount * sizeof(float));

				FRingBuffer_Push(&fcubeb->ringBuffer, fcubeb->tempBuffer + framesToUse * fcubeb->channelCount,
					(audio->updateSize - framesToUse) * fcubeb->channelCount * sizeof(float));
			} else {
				FAudio_INTERNAL_UpdateEngine(
					audio,
					outPointer
				);
			}

			outPointer += audio->updateSize * fcubeb->channelCount;
			accumulatedFrames += audio->updateSize;
		}
	}

	return nframes;
}
static void FAudio_InitCubebInstance()
{
	if (CubebContext) {
		return;
	}

	cubeb_init(&CubebContext, "FAudio", NULL);
	if (cubeb_enumerate_devices(CubebContext, CUBEB_DEVICE_TYPE_OUTPUT, &CubebDeviceCollection) != CUBEB_OK){
#if __ANDROID__ || TARGET_OS_IPHONE
		CubebDeviceCollection.count = 1;
		CubebDeviceCollection.device = NULL;
#endif
	}
}

void FAudio_PlatformAddRef()
{
	if (CubebContextRefCount == 0) {
		FAudio_InitCubebInstance();

		FAudio_INTERNAL_InitSIMDFunctions(
			SDL_HasSSE2(),
			SDL_HasNEON()
		);
	}

	CubebContextRefCount++;
}

void FAudio_PlatformRelease()
{
	CubebContextRefCount--;
	if (CubebContextRefCount == 0) {
		if (CubebDeviceCollection.device != NULL) {
			cubeb_device_collection_destroy(CubebContext, &CubebDeviceCollection);
		}
		cubeb_destroy(CubebContext);
	}
}

void FAudio_PlatformInit(
	FAudio *audio,
	uint32_t flags,
	uint32_t deviceIndex,
	FAudioWaveFormatExtensible *mixFormat,
	uint32_t *updateSize,
	void** platformDevice
) {
	*platformDevice = NULL;

	FCubebAudioStream* streamCubeb = (FCubebAudioStream*)calloc(1, sizeof(FCubebAudioStream));
	streamCubeb->channelCount = mixFormat->Format.nChannels;

	cubeb_stream_params outParams;
	outParams.format = CUBEB_SAMPLE_FLOAT32NE;
	outParams.rate = mixFormat->Format.nSamplesPerSec;
	outParams.channels = mixFormat->Format.nChannels;
	outParams.layout = (mixFormat->Format.nChannels == 1) ? CUBEB_LAYOUT_MONO : CUBEB_LAYOUT_STEREO;
	outParams.prefs = CUBEB_STREAM_PREF_NONE;

	FAudio_PlatformAddRef();

	uint32_t latencyFrames;

	int result = cubeb_get_min_latency(CubebContext, &outParams, &latencyFrames);
	if (result != CUBEB_OK) {
		SDL_Log("Could not get minimum latency, use default");
		latencyFrames = 256;
	}

	cubeb_devid outDeviceId = NULL;
	if ((deviceIndex != 0) && CubebDeviceCollection.device) {
		if (deviceIndex > CubebDeviceCollection.count) {
			SDL_Log("Out-of-range device index given to platform init!");
			return;
		}

		outDeviceId = CubebDeviceCollection.device[deviceIndex - 1].device_id;
	}

	result = cubeb_stream_init(CubebContext, &streamCubeb->stream, "FAudio Stream \"Device\"",
		NULL, NULL, outDeviceId, &outParams, latencyFrames, FAudio_INTERNAL_MixCallback, FNA_Internal_StateCallback,
		audio);

	if ((result != CUBEB_OK) || (streamCubeb->stream == NULL)) {
		FAudio_PlatformRelease();

		SDL_Log("Failed to create Cubeb stream! Freq=%d channels=%d err=%d", mixFormat->Format.nSamplesPerSec, mixFormat->Format.nChannels,
			result);
		return;
	}

	/* Write up the received format for the engine */
	WriteWaveFormatExtensible(
		mixFormat,
		streamCubeb->channelCount,
		mixFormat->Format.nSamplesPerSec,
		&DATAFORMAT_SUBTYPE_IEEE_FLOAT
	);

	if (flags & FAUDIO_1024_QUANTUM)
	{
		/* Get the sample count for a 21.33ms frame.
		 * For 48KHz this should be 1024.
		 */
		*updateSize = (int)(mixFormat->Format.nSamplesPerSec / (1000.0 / (64.0 / 3.0)));
	}
	else
	{
		*updateSize = mixFormat->Format.nSamplesPerSec / 100;
	}

	FRingBuffer_Init(&streamCubeb->ringBuffer, *updateSize * 4 * streamCubeb->channelCount * sizeof(float));

	/* SDL_AudioDeviceID is a Uint32, anybody using a 16-bit PC still? */
	*platformDevice = (void*)streamCubeb;

	/* Start the thread! */
	cubeb_stream_set_volume(streamCubeb->stream, 1.0f);
	cubeb_stream_start(streamCubeb->stream);
}

void FAudio_PlatformQuit(void* platformDevice)
{
	FCubebAudioStream* stream = (FCubebAudioStream*)platformDevice;
	cubeb_stream_stop(stream->stream);
	cubeb_stream_destroy(stream->stream);
	if (stream->tempBuffer) {
		free(stream->tempBuffer);
	}

	FRingBuffer_Free(&stream->ringBuffer);
	free(stream);

	FAudio_PlatformRelease();
}

uint32_t FAudio_PlatformGetDeviceCount()
{
	FAudio_InitCubebInstance();
	return (uint32_t)CubebDeviceCollection.count;
}

uint32_t FAudio_PlatformGetDeviceDetails(
	uint32_t index,
	FAudioDeviceDetails* details
) {
	FAudio_InitCubebInstance();

	if (index >= CubebDeviceCollection.count) {
		SDL_Log("Out-of-range device index given to platform get device details!");
		return FAUDIO_E_INVALID_CALL;
	}

	const char* name, * envvar;
	int channels, rate;

	details->DeviceID[0] = L'0' + index;

	name = (CubebDeviceCollection.device != NULL) ? CubebDeviceCollection.device[index].friendly_name : "Default Device";
	details->Role = (CubebDeviceCollection.device == NULL) ? FAudioGlobalDefaultDevice : FAudioNotDefaultDevice;

	FAudio_UTF8_To_UTF16(
		name,
		(uint16_t*)details->DisplayName,
		sizeof(details->DisplayName)
	);

	/* Environment variables take precedence over all possible values */
	envvar = SDL_getenv("SDL_AUDIO_FREQUENCY");
	if (envvar != NULL)
	{
		rate = SDL_atoi(envvar);
	}
	else
	{
		rate = 0;
	}
	envvar = SDL_getenv("SDL_AUDIO_CHANNELS");
	if (envvar != NULL)
	{
		channels = SDL_atoi(envvar);
	}
	else
	{
		channels = 0;
	}

	if (CubebDeviceCollection.device != NULL) {
		if (rate <= 0)
		{
			rate = (int)CubebDeviceCollection.device[index].default_rate;
		}
		if (channels <= 0)
		{
			channels = (int)CubebDeviceCollection.device[index].max_channels;
		}
	}

	/* If we make it all the way here with no format, hardcode a sane one */
	if (rate <= 0)
	{
		rate = 48000;
	}
	if (channels <= 0)
	{
		channels = 2;
	}

	/* Write the format, finally. */
	WriteWaveFormatExtensible(
		&details->OutputFormat,
		channels,
		rate,
		&DATAFORMAT_SUBTYPE_PCM
	);

	return 0;
}
#else
static void FAudio_INTERNAL_MixCallback(void* userdata, Uint8* stream, int len)
{
	FAudio* audio = (FAudio*)userdata;

	FAudio_zero(stream, len);
	if (audio->active)
	{
		FAudio_INTERNAL_UpdateEngine(
			audio,
			(float*)stream
		);
	}
}

/* Platform Functions */

static void FAudio_INTERNAL_PrioritizeDirectSound()
{
	int numdrivers, i, wasapi, directsound;

	if (SDL_GetHint("SDL_AUDIODRIVER") != NULL)
	{
		/* Already forced to something, ignore */
		return;
	}

	/* Check to see if we have both Windows drivers in the list */
	numdrivers = SDL_GetNumAudioDrivers();
	wasapi = -1;
	directsound = -1;
	for (i = 0; i < numdrivers; i += 1)
	{
		const char* driver = SDL_GetAudioDriver(i);
		if (SDL_strcmp(driver, "wasapi") == 0)
		{
			wasapi = i;
		}
		else if (SDL_strcmp(driver, "directsound") == 0)
		{
			directsound = i;
		}
	}

	/* We force if and only if both drivers exist and wasapi is earlier */
	if ((wasapi > -1) && (directsound > -1))
	{
		if (wasapi < directsound)
		{
			SDL_SetHint("SDL_AUDIODRIVER", "directsound");
		}
	}
}

void FAudio_PlatformAddRef()
{
	FAudio_INTERNAL_PrioritizeDirectSound();

	/* SDL tracks ref counts for each subsystem */
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
	{
		SDL_Log("SDL_INIT_AUDIO failed: %s", SDL_GetError());
	}
	FAudio_INTERNAL_InitSIMDFunctions(
		SDL_HasSSE2(),
		SDL_HasNEON()
	);
}

void FAudio_PlatformRelease()
{
	/* SDL tracks ref counts for each subsystem */
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void FAudio_PlatformInit(
	FAudio* audio,
	uint32_t flags,
	uint32_t deviceIndex,
	FAudioWaveFormatExtensible* mixFormat,
	uint32_t* updateSize,
	void** platformDevice
) {
	SDL_AudioDeviceID device;
	SDL_AudioSpec want, have;

	FAudio_assert(mixFormat != NULL);
	FAudio_assert(updateSize != NULL);

	/* Build the device spec */
	want.freq = mixFormat->Format.nSamplesPerSec;
	want.format = AUDIO_F32;
	want.channels = mixFormat->Format.nChannels;
	want.silence = 0;
	want.callback = FAudio_INTERNAL_MixCallback;
	want.userdata = audio;
	if (flags & FAUDIO_1024_QUANTUM)
	{
		/* Get the sample count for a 21.33ms frame.
		 * For 48KHz this should be 1024.
		 */
		want.samples = (int)(
			want.freq / (1000.0 / (64.0 / 3.0))
			);
	}
	else
	{
		want.samples = want.freq / 100;
	}

	/* Open the device (or at least try to) */
iosretry:
	device = SDL_OpenAudioDevice(
		deviceIndex > 0 ? SDL_GetAudioDeviceName(deviceIndex - 1, 0) : NULL,
		0,
		&want,
		&have,
		0
	);
	if (device == 0)
	{
		const char* err = SDL_GetError();
		SDL_Log("OpenAudioDevice failed: %s", err);

		/* iOS has a weird thing where you can't open a stream when the
		 * app is in the background, even though the program is meant
		 * to be suspended and thus not trip this in the first place.
		 *
		 * Startup suspend behavior when an app is opened then closed
		 * is a big pile of crap, basically.
		 *
		 * Google the error code and you'll find that this has been a
		 * long-standing issue that nobody seems to care about.
		 * -flibit
		 */
		if (SDL_strstr(err, "Code=561015905") != NULL)
		{
			goto iosretry;
		}

		FAudio_assert(0 && "Failed to open audio device!");
		return;
	}

	/* Write up the received format for the engine */
	WriteWaveFormatExtensible(
		mixFormat,
		have.channels,
		have.freq,
		&DATAFORMAT_SUBTYPE_IEEE_FLOAT
	);
	*updateSize = have.samples;

	/* SDL_AudioDeviceID is a Uint32, anybody using a 16-bit PC still? */
	*platformDevice = (void*)((size_t)device);

	/* Start the thread! */
	SDL_PauseAudioDevice(device, 0);
}

void FAudio_PlatformQuit(void* platformDevice)
{
	SDL_CloseAudioDevice((SDL_AudioDeviceID)((size_t)platformDevice));
}

uint32_t FAudio_PlatformGetDeviceCount()
{
	uint32_t devCount = SDL_GetNumAudioDevices(0);
	if (devCount == 0)
	{
		return 0;
	}
	return devCount + 1; /* Add one for "Default Device" */
}

uint32_t FAudio_PlatformGetDeviceDetails(
	uint32_t index,
	FAudioDeviceDetails* details
) {
	const char* name, * envvar;
	int channels, rate;
	SDL_AudioSpec spec;
	uint32_t devcount;

	FAudio_zero(details, sizeof(FAudioDeviceDetails));

	devcount = FAudio_PlatformGetDeviceCount();
	if (index >= devcount)
	{
		return FAUDIO_E_INVALID_CALL;
	}

	details->DeviceID[0] = L'0' + index;
	if (index == 0)
	{
		name = "Default Device";
		details->Role = FAudioGlobalDefaultDevice;

		/* This variable will look like a DSound GUID or WASAPI ID, i.e.
		 * "{0.0.0.00000000}.{FD47D9CC-4218-4135-9CE2-0C195C87405B}"
		 */
		envvar = SDL_getenv("FAUDIO_FORCE_DEFAULT_DEVICEID");
		if (envvar != NULL)
		{
			FAudio_UTF8_To_UTF16(
				envvar,
				(uint16_t*)details->DeviceID,
				sizeof(details->DeviceID)
			);
		}
	}
	else
	{
		name = SDL_GetAudioDeviceName(index - 1, 0);
		details->Role = FAudioNotDefaultDevice;
	}
	FAudio_UTF8_To_UTF16(
		name,
		(uint16_t*)details->DisplayName,
		sizeof(details->DisplayName)
	);

	/* Environment variables take precedence over all possible values */
	envvar = SDL_getenv("SDL_AUDIO_FREQUENCY");
	if (envvar != NULL)
	{
		rate = SDL_atoi(envvar);
	}
	else
	{
		rate = 0;
	}
	envvar = SDL_getenv("SDL_AUDIO_CHANNELS");
	if (envvar != NULL)
	{
		channels = SDL_atoi(envvar);
	}
	else
	{
		channels = 0;
	}

	/* Get the device format from the OS */
	if (index == 0)
	{
		if (SDL_GetDefaultAudioInfo(NULL, &spec, 0) < 0)
		{
			SDL_zero(spec);
		}
	}
	else
	{
		SDL_GetAudioDeviceSpec(index - 1, 0, &spec);
	}
	if ((spec.freq > 0) && (rate <= 0))
	{
		rate = spec.freq;
	}
	if ((spec.channels > 0) && (channels <= 0))
	{
		channels = spec.channels;
	}

	/* If we make it all the way here with no format, hardcode a sane one */
	if (rate <= 0)
	{
		rate = 48000;
	}
	if (channels <= 0)
	{
		channels = 2;
	}

	/* Write the format, finally. */
	WriteWaveFormatExtensible(
		&details->OutputFormat,
		channels,
		rate,
		&DATAFORMAT_SUBTYPE_PCM
	);
	return 0;
}

#endif
/* Threading */

FAudioThread FAudio_PlatformCreateThread(
	FAudioThreadFunc func,
	const char *name,
	void* data
) {
	return (FAudioThread) SDL_CreateThread(
		(SDL_ThreadFunction) func,
		name,
		data
	);
}

void FAudio_PlatformWaitThread(FAudioThread thread, int32_t *retval)
{
	SDL_WaitThread((SDL_Thread*) thread, retval);
}

void FAudio_PlatformThreadPriority(FAudioThreadPriority priority)
{
	SDL_SetThreadPriority((SDL_ThreadPriority) priority);
}

uint64_t FAudio_PlatformGetThreadID(void)
{
	return (uint64_t) SDL_ThreadID();
}

FAudioMutex FAudio_PlatformCreateMutex()
{
	return (FAudioMutex) SDL_CreateMutex();
}

void FAudio_PlatformDestroyMutex(FAudioMutex mutex)
{
	SDL_DestroyMutex((SDL_mutex*) mutex);
}

void FAudio_PlatformLockMutex(FAudioMutex mutex)
{
	SDL_LockMutex((SDL_mutex*) mutex);
}

void FAudio_PlatformUnlockMutex(FAudioMutex mutex)
{
	SDL_UnlockMutex((SDL_mutex*) mutex);
}

void FAudio_sleep(uint32_t ms)
{
	SDL_Delay(ms);
}

/* Time */

uint32_t FAudio_timems()
{
	return SDL_GetTicks();
}

/* FAudio I/O */

FAudioIOStream* FAudio_fopen(const char *path)
{
	FAudioIOStream *io = (FAudioIOStream*) FAudio_malloc(
		sizeof(FAudioIOStream)
	);
	SDL_RWops *rwops = SDL_RWFromFile(path, "rb");
	io->data = rwops;
	io->read = (FAudio_readfunc) rwops->read;
	io->seek = (FAudio_seekfunc) rwops->seek;
	io->close = (FAudio_closefunc) rwops->close;
	io->lock = FAudio_PlatformCreateMutex();
	return io;
}

FAudioIOStream* FAudio_memopen(void *mem, int len)
{
	FAudioIOStream *io = (FAudioIOStream*) FAudio_malloc(
		sizeof(FAudioIOStream)
	);
	SDL_RWops *rwops = SDL_RWFromMem(mem, len);
	io->data = rwops;
	io->read = (FAudio_readfunc) rwops->read;
	io->seek = (FAudio_seekfunc) rwops->seek;
	io->close = (FAudio_closefunc) rwops->close;
	io->lock = FAudio_PlatformCreateMutex();
	return io;
}

uint8_t* FAudio_memptr(FAudioIOStream *io, size_t offset)
{
	SDL_RWops *rwops = (SDL_RWops*) io->data;
	FAudio_assert(rwops->type == SDL_RWOPS_MEMORY);
	return rwops->hidden.mem.base + offset;
}

void FAudio_close(FAudioIOStream *io)
{
	io->close(io->data);
	FAudio_PlatformDestroyMutex((FAudioMutex) io->lock);
	FAudio_free(io);
}

#ifdef FAUDIO_DUMP_VOICES
FAudioIOStreamOut* FAudio_fopen_out(const char *path, const char *mode)
{
	FAudioIOStreamOut *io = (FAudioIOStreamOut*) FAudio_malloc(
		sizeof(FAudioIOStreamOut)
	);
	SDL_RWops *rwops = SDL_RWFromFile(path, mode);
	io->data = rwops;
	io->read = (FAudio_readfunc) rwops->read;
	io->write = (FAudio_writefunc) rwops->write;
	io->seek = (FAudio_seekfunc) rwops->seek;
	io->size = (FAudio_sizefunc) rwops->size;
	io->close = (FAudio_closefunc) rwops->close;
	io->lock = FAudio_PlatformCreateMutex();
	return io;
}

void FAudio_close_out(FAudioIOStreamOut *io)
{
	io->close(io->data);
	FAudio_PlatformDestroyMutex((FAudioMutex) io->lock);
	FAudio_free(io);
}
#endif /* FAUDIO_DUMP_VOICES */

/* UTF8->UTF16 Conversion, taken from PhysicsFS */

#define UNICODE_BOGUS_CHAR_VALUE 0xFFFFFFFF
#define UNICODE_BOGUS_CHAR_CODEPOINT '?'

static uint32_t FAudio_UTF8_CodePoint(const char **_str)
{
    const char *str = *_str;
    uint32_t retval = 0;
    uint32_t octet = (uint32_t) ((uint8_t) *str);
    uint32_t octet2, octet3, octet4;

    if (octet == 0)  /* null terminator, end of string. */
        return 0;

    else if (octet < 128)  /* one octet char: 0 to 127 */
    {
        (*_str)++;  /* skip to next possible start of codepoint. */
        return octet;
    } /* else if */

    else if ((octet > 127) && (octet < 192))  /* bad (starts with 10xxxxxx). */
    {
        /*
         * Apparently each of these is supposed to be flagged as a bogus
         *  char, instead of just resyncing to the next valid codepoint.
         */
        (*_str)++;  /* skip to next possible start of codepoint. */
        return UNICODE_BOGUS_CHAR_VALUE;
    } /* else if */

    else if (octet < 224)  /* two octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet -= (128+64);
        octet2 = (uint32_t) ((uint8_t) *(++str));
        if ((octet2 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 1;  /* skip to next possible start of codepoint. */
        retval = ((octet << 6) | (octet2 - 128));
        if ((retval >= 0x80) && (retval <= 0x7FF))
            return retval;
    } /* else if */

    else if (octet < 240)  /* three octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet -= (128+64+32);
        octet2 = (uint32_t) ((uint8_t) *(++str));
        if ((octet2 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet3 = (uint32_t) ((uint8_t) *(++str));
        if ((octet3 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 2;  /* skip to next possible start of codepoint. */
        retval = ( ((octet << 12)) | ((octet2-128) << 6) | ((octet3-128)) );

        /* There are seven "UTF-16 surrogates" that are illegal in UTF-8. */
        switch (retval)
        {
            case 0xD800:
            case 0xDB7F:
            case 0xDB80:
            case 0xDBFF:
            case 0xDC00:
            case 0xDF80:
            case 0xDFFF:
                return UNICODE_BOGUS_CHAR_VALUE;
        } /* switch */

        /* 0xFFFE and 0xFFFF are illegal, too, so we check them at the edge. */
        if ((retval >= 0x800) && (retval <= 0xFFFD))
            return retval;
    } /* else if */

    else if (octet < 248)  /* four octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet -= (128+64+32+16);
        octet2 = (uint32_t) ((uint8_t) *(++str));
        if ((octet2 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet3 = (uint32_t) ((uint8_t) *(++str));
        if ((octet3 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet4 = (uint32_t) ((uint8_t) *(++str));
        if ((octet4 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 3;  /* skip to next possible start of codepoint. */
        retval = ( ((octet << 18)) | ((octet2 - 128) << 12) |
                   ((octet3 - 128) << 6) | ((octet4 - 128)) );
        if ((retval >= 0x10000) && (retval <= 0x10FFFF))
            return retval;
    } /* else if */

    /*
     * Five and six octet sequences became illegal in rfc3629.
     *  We throw the codepoint away, but parse them to make sure we move
     *  ahead the right number of bytes and don't overflow the buffer.
     */

    else if (octet < 252)  /* five octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 4;  /* skip to next possible start of codepoint. */
        return UNICODE_BOGUS_CHAR_VALUE;
    } /* else if */

    else  /* six octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 6;  /* skip to next possible start of codepoint. */
        return UNICODE_BOGUS_CHAR_VALUE;
    } /* else if */

    return UNICODE_BOGUS_CHAR_VALUE;
}

void FAudio_UTF8_To_UTF16(const char *src, uint16_t *dst, size_t len)
{
    len -= sizeof (uint16_t);   /* save room for null char. */
    while (len >= sizeof (uint16_t))
    {
        uint32_t cp = FAudio_UTF8_CodePoint(&src);
        if (cp == 0)
            break;
        else if (cp == UNICODE_BOGUS_CHAR_VALUE)
            cp = UNICODE_BOGUS_CHAR_CODEPOINT;

        if (cp > 0xFFFF)  /* encode as surrogate pair */
        {
            if (len < (sizeof (uint16_t) * 2))
                break;  /* not enough room for the pair, stop now. */

            cp -= 0x10000;  /* Make this a 20-bit value */

            *(dst++) = 0xD800 + ((cp >> 10) & 0x3FF);
            len -= sizeof (uint16_t);

            cp = 0xDC00 + (cp & 0x3FF);
        } /* if */

        *(dst++) = cp;
        len -= sizeof (uint16_t);
    } /* while */

    *dst = 0;
}

/* vim: set noexpandtab shiftwidth=8 tabstop=8: */

#else

extern int this_tu_is_empty;

#endif /* FAUDIO_WIN32_PLATFORM */
