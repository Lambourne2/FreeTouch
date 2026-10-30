#include "pch.h"
#include "audio/SoundPlayer.h"

#include <Windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <cstring>
#include <fstream>

#pragma comment(lib, "xaudio2.lib")

namespace
{
	constexpr unsigned kRate = 48000;
	constexpr size_t kVoices = 16;
	constexpr float kMaxPitch = 4.f;

	// RIFF/WAVE PCM 16-bit -> 48 kHz mono (channels averaged, linear resampling). Empty on anything else.
	std::vector<short> readWav(const std::filesystem::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
			return {};
		unsigned channels = 0, rate = 0, bits = 0, format = 0;
		const char* data = nullptr;
		size_t dataSize = 0;
		for (size_t pos = 12; pos + 8 <= bytes.size();)
		{
			uint32_t size = 0;
			std::memcpy(&size, bytes.data() + pos + 4, 4);
			const char* body = bytes.data() + pos + 8;
			if (pos + 8 + size > bytes.size())
				size = static_cast<uint32_t>(bytes.size() - pos - 8);
			if (std::memcmp(bytes.data() + pos, "fmt ", 4) == 0 && size >= 16)
			{
				uint16_t v16 = 0;
				std::memcpy(&v16, body, 2); format = v16;
				std::memcpy(&v16, body + 2, 2); channels = v16;
				std::memcpy(&rate, body + 4, 4);
				std::memcpy(&v16, body + 14, 2); bits = v16;
			}
			else if (std::memcmp(bytes.data() + pos, "data", 4) == 0)
			{
				data = body;
				dataSize = size;
			}
			pos += 8 + size + (size & 1);
		}
		if (!data || (format != 1 && format != 0xFFFE) || bits != 16 || channels == 0 || rate == 0)
			return {};
		const size_t frames = dataSize / (2 * channels);
		std::vector<float> mono(frames);
		for (size_t f = 0; f < frames; ++f)
		{
			int sum = 0;
			for (unsigned c = 0; c < channels; ++c)
			{
				int16_t s = 0;
				std::memcpy(&s, data + (f * channels + c) * 2, 2);
				sum += s;
			}
			mono[f] = static_cast<float>(sum) / channels;
		}
		if (frames == 0)
			return {};
		const size_t outFrames = static_cast<size_t>(static_cast<double>(frames) * kRate / rate);
		std::vector<short> out(outFrames);
		for (size_t i = 0; i < outFrames; ++i)
		{
			const double src = static_cast<double>(i) * rate / kRate;
			const size_t a = (std::min)(static_cast<size_t>(src), frames - 1);
			const size_t b = (std::min)(a + 1, frames - 1);
			const double t = src - a;
			out[i] = static_cast<short>(std::clamp(mono[a] + (mono[b] - mono[a]) * t, -32768.0, 32767.0));
		}
		return out;
	}
}

SoundPlayer::~SoundPlayer()
{
	shutdown();
}

bool SoundPlayer::start()
{
	if (master_)
		return true;
	const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	comInitialized_ = co == S_OK || co == S_FALSE; // a thread already set up differently is fine as it is
	if (FAILED(XAudio2Create(&xaudio_, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !xaudio_)
		return false;
	if (FAILED(xaudio_->CreateMasteringVoice(&master_)) || !master_)
	{
		xaudio_->Release();
		xaudio_ = nullptr;
		return false;
	}
	WAVEFORMATEX fmt{};
	fmt.wFormatTag = WAVE_FORMAT_PCM;
	fmt.nChannels = 1;
	fmt.nSamplesPerSec = kRate;
	fmt.wBitsPerSample = 16;
	fmt.nBlockAlign = 2;
	fmt.nAvgBytesPerSec = kRate * 2;
	for (size_t i = 0; i < kVoices; ++i)
	{
		IXAudio2SourceVoice* v = nullptr;
		if (SUCCEEDED(xaudio_->CreateSourceVoice(&v, &fmt, 0, kMaxPitch)) && v)
			voices_.push_back({v, 0});
	}
	return true;
}

int SoundPlayer::load(const std::filesystem::path& folder)
{
	std::lock_guard lock(mutex_);
	if (!start())
		return 0;
	// Nothing may still be reading the old clips.
	for (auto& v : voices_)
	{
		v.voice->Stop();
		v.voice->FlushSourceBuffers();
	}
	// The audio thread may still be finishing a flushed buffer: keep the old samples alive until shutdown.
	retired_.push_back(std::move(clips_));
	clips_.clear();
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
	{
		if (!entry.is_regular_file() || entry.path().extension() != ".wav")
			continue;
		auto samples = readWav(entry.path());
		if (!samples.empty())
			clips_[entry.path().stem().string()] = std::move(samples);
	}
	return static_cast<int>(clips_.size());
}

void SoundPlayer::shutdown()
{
	std::lock_guard lock(mutex_);
	for (auto& v : voices_)
		v.voice->DestroyVoice();
	voices_.clear();
	if (master_)
		master_->DestroyVoice();
	master_ = nullptr;
	if (xaudio_)
		xaudio_->Release();
	xaudio_ = nullptr;
	clips_.clear();
	retired_.clear();
	if (comInitialized_)
		CoUninitialize();
	comInitialized_ = false;
}

void SoundPlayer::play(const std::string& name, float volume, float pitch)
{
	if (volume <= 0.001f)
		return;
	std::lock_guard lock(mutex_);
	if (!master_ || voices_.empty())
		return;
	const auto clip = clips_.find(name);
	if (clip == clips_.end())
		return;

	// A free voice, else the one that started longest ago.
	Voice* pick = nullptr;
	for (auto& v : voices_)
	{
		XAUDIO2_VOICE_STATE state{};
		v.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
		if (state.BuffersQueued == 0)
		{
			pick = &v;
			break;
		}
	}
	if (!pick)
		pick = &*(std::min_element)(voices_.begin(), voices_.end(),
			[](const Voice& a, const Voice& b) { return a.startedAt < b.startedAt; });
	pick->voice->Stop();
	pick->voice->FlushSourceBuffers();

	XAUDIO2_BUFFER buffer{};
	buffer.AudioBytes = static_cast<UINT32>(clip->second.size() * sizeof(short));
	buffer.pAudioData = reinterpret_cast<const BYTE*>(clip->second.data());
	buffer.Flags = XAUDIO2_END_OF_STREAM;
	pick->voice->SetVolume(std::clamp(volume, 0.f, 1.f));
	pick->voice->SetFrequencyRatio(std::clamp(pitch, 0.25f, kMaxPitch));
	if (SUCCEEDED(pick->voice->SubmitSourceBuffer(&buffer)))
		pick->voice->Start();
	pick->startedAt = ++plays_;
}
