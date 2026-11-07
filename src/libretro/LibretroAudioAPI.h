#pragma once

#include "audio/IAudioAPI.h"
#include "libretro/LibretroAudioRing.h"

#include <atomic>

class LibretroAudioAPI : public IAudioAPI
{
public:
	// Callback type matches retro_audio_sample_batch_t: returns frames actually accepted.
	using AudioCallback = size_t(*)(const int16_t* data, size_t frames);
	// RetroArch's multi-channel batch: frames of `channels` samples, the
	// speakers in `layout` (RETRO_AUDIO_SPEAKER_*)
	using MultiAudioCallback = size_t(*)(const int16_t* data, size_t frames,
		unsigned channels, unsigned layout);

	LibretroAudioAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample);
	// The ring is a static that outlive any one instance,
	// and until this they were only ever put back by the next run's
	// constructor - so a closed title left its last few milliseconds of audio
	// sitting there for the next one to start with. Emptied here instead,
	// which is the end of the run that produced them: ShutdownTitle destroys
	// this object through snd_core::reset, after the scheduler has stopped, so
	// nothing is still feeding it.
	~LibretroAudioAPI() override;

	AudioAPI GetType() const override { return Cubeb; } // pretend to be Cubeb for config compatibility

	bool NeedAdditionalBlocks() const override;
	bool FeedBlock(sint16* data) override;
	bool Play() override;
	bool Stop() override;

	// Register the frontend's audio_batch_cb (called from retro_init).
	static void SetAudioCallback(AudioCallback cb);
	// What the frontend is given: 2 (stereo through the batch callback) or 6
	// (5.1 through the multi-channel one). Set at load, before AX starts.
	static void SetOutput(unsigned channels, MultiAudioCallback multi);

	// Push one frame's worth (frames, at the rate the core reports) to the
	// frontend, more only to work off a backlog. Call once per retro_run.
	static void FlushAudio(size_t frames);

	// Audio Thread (cemu_audio_thread): RetroArch pulls the audio on its own
	// thread instead of retro_run handing it over, and AX runs by the wall
	// clock as in standalone Cemu, so a late retro_run (loading, shader
	// compilation) does not stop the audio.
	static void SetThreadMode(bool enabled) { s_thread_mode.store(enabled, std::memory_order_relaxed); }
	static bool IsThreadMode() { return s_thread_mode.load(std::memory_order_relaxed); }
	// Called from RetroArch's audio thread: hands over up to maxFrames of
	// what is buffered and returns how many frames went out
	static size_t PullAudio(size_t maxFrames);
	// Drops what is buffered (after a pause, so it is not played late)
	static void DropBuffered();

	static void Reset();

	// Once a second, report what AX produced, what the ring had to drop and
	// what the frontend took (cemu_log_audio). Audio going missing has three
	// possible ends - AX not producing, the ring overflowing, the frontend
	// refusing - and a report that only says "too few samples" cannot tell
	// them apart.
	static void SetStatsLogging(bool enabled);
	static bool IsStatsLogging() { return s_log_stats; }

private:
	static size_t SendFromRing(size_t sampleCount, size_t& sent);
	static void AccountWrite(size_t offered, size_t written);
	static void AccountFlush(size_t read, size_t sent);
	static void ReportStats();

	static AudioCallback s_audio_callback;
	static MultiAudioCallback s_multi_callback;
	static unsigned s_out_channels;
	static LibretroAudioRingBuffer s_ring_buffer;
	static std::atomic_bool s_thread_mode;

	static bool s_log_stats;
	static uint64_t s_stat_offered;
	static uint64_t s_stat_written;
	static uint64_t s_stat_read;
	static uint64_t s_stat_sent;
	static uint64_t s_stat_flushes;
	static uint64_t s_stat_empty_flushes;
};
