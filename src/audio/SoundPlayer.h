#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

// Plays Free Touch's own sounds (content/sounds/*.wav) through Windows' XAudio2: menu clicks and training cues.
// Not part of the game: no SDK calls, nothing online. Any thread (calls are serialised). If audio cannot start,
// every call is a quiet no-op.
class SoundPlayer
{
public:
	SoundPlayer() = default;
	~SoundPlayer();
	SoundPlayer(const SoundPlayer&) = delete;
	SoundPlayer& operator=(const SoundPlayer&) = delete;

	// Starts XAudio2 and loads every .wav in `folder` (PCM 16-bit, any rate and channel count; stored as 48 kHz
	// mono). Returns how many sounds loaded. Calling it again reloads the folder.
	int load(const std::filesystem::path& folder);
	void shutdown();

	// `name` is the file name without ".wav". `volume` 0..1, `pitch` a playback rate (1 = as recorded, 2 = an octave
	// up). Unknown names and a zero volume do nothing.
	void play(const std::string& name, float volume, float pitch = 1.f);

	bool ready() const { return master_ != nullptr; }

private:
	struct Voice
	{
		IXAudio2SourceVoice* voice = nullptr;
		unsigned long long startedAt = 0; // play counter, to reuse the oldest when all are busy
	};

	bool start();

	std::mutex mutex_;
	IXAudio2* xaudio_ = nullptr;
	IXAudio2MasteringVoice* master_ = nullptr;
	std::vector<Voice> voices_;
	std::map<std::string, std::vector<short>> clips_; // 48 kHz mono samples
	std::vector<std::map<std::string, std::vector<short>>> retired_; // clips from before a reload
	unsigned long long plays_ = 0;
	bool comInitialized_ = false;
};
