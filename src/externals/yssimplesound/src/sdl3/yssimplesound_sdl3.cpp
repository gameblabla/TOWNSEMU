/*
 * YsSoundPlayer SDL3 backend.
 *
 * Added to replace the Linux ALSA backend (linux_alsa/yssimplesound_linux_alsa.cpp)
 * as the default on Linux, after a real bug hunt (see that file's own comments,
 * and TOWNSEMU's git history around FM TOWNS DAC streaming) traced audible
 * dropouts during continuous multi-second DAC streaming (TownsSound's FM_PCM
 * channel) back to that backend's architecture: audio buffer draining is only
 * ever driven synchronously from the single VM thread (KeepPlaying(), itself
 * only called once per outer VM loop iteration - see townsthread.cpp), through
 * a hard-coded 2-segment "playing"/"standBy" buffer that *silently drops* any
 * new segment the instant both are still full - no retry, no error surfaced.
 * One missed/late service window during a long stream was enough to lose
 * everything after it. Enlarging that buffer (tried first) didn't fix it,
 * because the actual draining still only happens when the VM thread calls in -
 * there's no independent thread pulling from it at the real 44.1kHz device
 * rate no matter how big the buffer is.
 *
 * SDL3's audio streams solve this structurally rather than by tuning buffer
 * sizes: SDL_PutAudioStreamData() feeds a real, dynamically-growable queue,
 * and a dedicated SDL-owned audio thread drains it into the actual device at
 * the real hardware rate, completely independent of how often (or
 * irregularly) this emulator's single VM thread gets around to calling in.
 * Each SoundData/Stream gets its own dedicated audio device (SDL supports
 * opening the same physical output multiple times - the OS/SDL mixes the
 * independent streams), which also gives correct per-sound Pause/Resume for
 * free, unlike a single shared device would.
 */
#include "yssimplesound.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>

#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdint>

static SDL_AudioFormat FormatFor(unsigned int bitPerSample,YSBOOL isSigned)
{
	if(16==bitPerSample)
	{
		// This codebase only ever creates 16-bit sound data as signed
		// (CreateFromSigned16bitStereo, WAV loading normalizes to
		// signed) - SDL3 doesn't even have an unsigned 16-bit format
		// (removed the deprecated one from SDL2), so there's nothing
		// sensible to do for isSigned==false here anyway.
		return SDL_AUDIO_S16;
	}
	return (YSTRUE==isSigned) ? SDL_AUDIO_S8 : SDL_AUDIO_U8;
}

// ------------------------------------------------------------------
// Type definitions up front (in dependency order) so every function
// body below can see complete types regardless of which class it
// belongs to - SoundData::APISpecificDataPerSoundData and
// Stream::APISpecificData are private nested types only forward-
// declared in yssimplesound.h.
// ------------------------------------------------------------------

// Each SoundData/Stream below owns its own actual SDL audio
// device+stream, so there isn't much device-wide state needed here
// beyond SDL_InitSubSystem bookkeeping and the list of currently-
// looping SoundData for KeepPlayingAPISpecific to re-feed.
class YsSoundPlayer::APISpecificData
{
public:
	bool initializedSDL=false;
	std::vector <SoundData *> looping;
};

// One-shot/background (PlayOneShot/PlayBackground) playback: CDDA, the
// debug "beep" channel, and any other non-streaming use.
class YsSoundPlayer::SoundData::APISpecificDataPerSoundData
{
public:
	SDL_AudioStream *stream=nullptr;
	long long int totalBytesPut=0;
};

// Continuous streaming playback (TownsSound's FM_PCM channel:
// RF5C68/YM2612-DAC/CD-DA-adjacent raw PCM generated a chunk at a time
// and fed in as it's produced). This is the path the whole SDL3 backend
// was added to fix.
class YsSoundPlayer::Stream::APISpecificData
{
public:
	SDL_AudioStream *stream=nullptr;
	int maxQueuedBytes=0;
};

YsSoundPlayer::APISpecificData *YsSoundPlayer::CreateAPISpecificData(void)
{
	return new APISpecificData;
}
void YsSoundPlayer::DeleteAPISpecificData(APISpecificData *ptr)
{
	delete ptr;
}

YSRESULT YsSoundPlayer::StartAPISpecific(void)
{
	if(true!=SDL_WasInit(SDL_INIT_AUDIO))
	{
		if(true!=SDL_InitSubSystem(SDL_INIT_AUDIO))
		{
			return YSERR;
		}
		api->initializedSDL=true;
	}
	return YSOK;
}
YSRESULT YsSoundPlayer::EndAPISpecific(void)
{
	api->looping.clear();
	if(true==api->initializedSDL)
	{
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		api->initializedSDL=false;
	}
	return YSOK;
}

void YsSoundPlayer::KeepPlayingAPISpecific(void)
{
	// Re-arm any PlayBackground (looping) sound once SDL's queue for it
	// runs dry. Actual device draining/timing happens on SDL's own
	// thread regardless of how often this gets called - this call only
	// needs to happen often enough to notice a loop boundary before the
	// listener would, not to keep audio flowing at all (that's the
	// whole point of moving to SDL3 streams).
	for(auto *dat : api->looping)
	{
		if(nullptr!=dat && nullptr!=dat->api->stream &&
		   0==SDL_GetAudioStreamQueued(dat->api->stream))
		{
			SDL_PutAudioStreamData(dat->api->stream,dat->DataPointer(),(int)dat->SizeInByte());
		}
	}
}

YsSoundPlayer::SoundData::APISpecificDataPerSoundData *YsSoundPlayer::SoundData::CreateAPISpecificData(void)
{
	return new APISpecificDataPerSoundData;
}
void YsSoundPlayer::SoundData::DeleteAPISpecificData(APISpecificDataPerSoundData *ptr)
{
	if(nullptr!=ptr->stream)
	{
		SDL_DestroyAudioStream(ptr->stream); // Also closes the device this stream owns.
	}
	delete ptr;
}

bool YsSoundPlayer::SoundData::IsPrepared(YsSoundPlayer &)
{
	return prepared;
}

YSRESULT YsSoundPlayer::SoundData::PreparePlay(YsSoundPlayer &player)
{
	if(nullptr==api->stream)
	{
		SDL_AudioSpec spec;
		spec.format=FormatFor(BitPerSample(),IsSigned());
		spec.channels=(YSTRUE==Stereo()) ? 2 : 1;
		spec.freq=(int)PlayBackRate();

		api->stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr);
		if(nullptr==api->stream)
		{
			return YSERR;
		}
		SDL_ResumeAudioStreamDevice(api->stream);
	}
	return YSOK;
}
void YsSoundPlayer::SoundData::CleanUpAPISpecific(void)
{
	if(nullptr!=api->stream)
	{
		SDL_DestroyAudioStream(api->stream);
		api->stream=nullptr;
	}
	api->totalBytesPut=0;
}

YSRESULT YsSoundPlayer::PlayOneShotAPISpecific(SoundData &dat)
{
	if(nullptr==dat.api->stream && YSOK!=dat.PreparePlay(*this))
	{
		return YSERR;
	}
	api->looping.erase(std::remove(api->looping.begin(),api->looping.end(),&dat),api->looping.end());
	SDL_ClearAudioStream(dat.api->stream);
	if(true!=SDL_PutAudioStreamData(dat.api->stream,dat.DataPointer(),(int)dat.SizeInByte()))
	{
		return YSERR;
	}
	dat.api->totalBytesPut=dat.SizeInByte();
	return YSOK;
}
YSRESULT YsSoundPlayer::PlayBackgroundAPISpecific(SoundData &dat)
{
	if(nullptr==dat.api->stream && YSOK!=dat.PreparePlay(*this))
	{
		return YSERR;
	}
	SDL_ClearAudioStream(dat.api->stream);
	if(true!=SDL_PutAudioStreamData(dat.api->stream,dat.DataPointer(),(int)dat.SizeInByte()))
	{
		return YSERR;
	}
	dat.api->totalBytesPut=dat.SizeInByte();
	if(api->looping.end()==std::find(api->looping.begin(),api->looping.end(),&dat))
	{
		api->looping.push_back(&dat);
	}
	return YSOK;
}
void YsSoundPlayer::StopAPISpecific(SoundData &dat)
{
	api->looping.erase(std::remove(api->looping.begin(),api->looping.end(),&dat),api->looping.end());
	if(nullptr!=dat.api->stream)
	{
		SDL_ClearAudioStream(dat.api->stream);
	}
}
YSBOOL YsSoundPlayer::IsPlayingAPISpecific(const SoundData &dat) const
{
	if(nullptr==dat.api->stream)
	{
		return YSFALSE;
	}
	return (0<SDL_GetAudioStreamQueued(dat.api->stream)) ? YSTRUE : YSFALSE;
}
double YsSoundPlayer::GetCurrentPositionAPISpecific(const SoundData &dat) const
{
	if(nullptr==dat.api->stream || 0==dat.PlayBackRate())
	{
		return 0.0;
	}
	long long int played=dat.api->totalBytesPut-SDL_GetAudioStreamQueued(dat.api->stream);
	if(played<0)
	{
		played=0;
	}
	return (double)played/(double)dat.BytePerTimeStep()/(double)dat.PlayBackRate();
}
void YsSoundPlayer::SetVolumeAPISpecific(SoundData &dat,float leftVol,float rightVol)
{
	if(nullptr!=dat.api->stream)
	{
		// SDL3 audio streams take a single scalar gain, not independent
		// per-channel gain - average the two. Strictly more than the
		// ALSA backend did here ("has no effect... in Linux-ALSA
		// environment", per this function's doc comment in
		// yssimplesound.h).
		SDL_SetAudioStreamGain(dat.api->stream,0.5f*(leftVol+rightVol));
	}
}
void YsSoundPlayer::PauseAPISpecific(SoundData &dat)
{
	if(nullptr!=dat.api->stream)
	{
		SDL_PauseAudioStreamDevice(dat.api->stream);
	}
}
void YsSoundPlayer::ResumeAPISpecific(SoundData &dat)
{
	if(nullptr!=dat.api->stream)
	{
		SDL_ResumeAudioStreamDevice(dat.api->stream);
	}
}

YsSoundPlayer::Stream::APISpecificData *YsSoundPlayer::Stream::CreateAPISpecificData(void)
{
	return new APISpecificData;
}
void YsSoundPlayer::Stream::DeleteAPISpecificData(APISpecificData *ptr)
{
	if(nullptr!=ptr->stream)
	{
		SDL_DestroyAudioStream(ptr->stream);
	}
	delete ptr;
}

YSRESULT YsSoundPlayer::StartStreamingAPISpecific(Stream &stream,StreamingOption opt)
{
	if(nullptr==stream.api->stream)
	{
		// TownsSound always creates streaming segments via
		// CreateFromSigned16bitStereo(YM2612::WAVE_SAMPLING_RATE,...)
		// - fixed 44100Hz signed 16-bit stereo - see sound.cpp.
		SDL_AudioSpec spec;
		spec.format=SDL_AUDIO_S16;
		spec.channels=2;
		spec.freq=44100;

		stream.api->stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr);
		if(nullptr==stream.api->stream)
		{
			return YSERR;
		}
		SDL_ResumeAudioStreamDevice(stream.api->stream);
	}

	// requestedRingBufferLengthMillisec is "just a preference and may
	// be ignored" per StreamingOption's own doc comment - honor it as
	// an upper bound on how far ahead the producer (this emulator) is
	// allowed to queue, mainly so a runaway producer can't grow memory
	// without bound, not because SDL needs help keeping up (it doesn't -
	// its own thread drains this queue at the real device rate
	// regardless of how bursty/irregular our feeding is). Default to a
	// full second of slack if unset, which is already vastly more
	// forgiving than the old ALSA backend's fixed ~80ms.
	uint32_t ms=(0!=opt.ringBufferLengthMillisec) ? opt.ringBufferLengthMillisec : 1000;
	stream.api->maxQueuedBytes=(int)((uint64_t)ms*44100u*2u*2u/1000u);

	return YSOK;
}
void YsSoundPlayer::StopStreamingAPISpecific(Stream &stream)
{
	if(nullptr!=stream.api->stream)
	{
		SDL_ClearAudioStream(stream.api->stream);
	}
}
YSBOOL YsSoundPlayer::StreamPlayerReadyToAcceptNextNumSampleAPISpecific(const Stream &stream,unsigned int numSamples) const
{
	if(nullptr==stream.api->stream)
	{
		return YSFALSE;
	}
	int wouldBeBytes=SDL_GetAudioStreamQueued(stream.api->stream)+(int)numSamples*2*2;
	return (wouldBeBytes<=stream.api->maxQueuedBytes) ? YSTRUE : YSFALSE;
}
YSRESULT YsSoundPlayer::AddNextStreamingSegmentAPISpecific(Stream &stream,const SoundData &dat)
{
	if(nullptr==stream.api->stream)
	{
		return YSERR;
	}
	if(true!=SDL_PutAudioStreamData(stream.api->stream,dat.DataPointer(),(int)dat.SizeInByte()))
	{
		return YSERR;
	}
	return YSOK;
}
