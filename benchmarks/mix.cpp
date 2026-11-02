#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "audiosource/wav/soloud_wav.hpp"
#include "audiosource/wav/soloud_wavstream.hpp"
#include "core/soloud_audiosource.hpp"
#include "filter/soloud_biquadresonantfilter.hpp"
#include "soloud.hpp"

namespace
{
	constexpr unsigned int kSampleRate = 44100;
	constexpr unsigned int kMaxVoices = 128;
	constexpr unsigned int kBackendBufferFrames = 2048;
	constexpr unsigned int kSourceFrames = 220500;
	constexpr unsigned int kWarmupBlocks = 16;
	constexpr unsigned int kMeasuredBlocks = 64;
	constexpr unsigned int kTrials = 3;
	constexpr unsigned int kChannelsMono = 1;
	constexpr unsigned int kChannelsStereo = 2;
	constexpr unsigned int kWavHeaderBytes = 44;
	constexpr unsigned int kChunkIdBytes = 4;
	constexpr unsigned int kWavPcmFormat = 1;
	constexpr unsigned int kPcm16Bits = 16;
	constexpr unsigned int kPcm16Bytes = kPcm16Bits / 8;
	constexpr unsigned int kWavFormatChunkBytes = 16;
	constexpr unsigned int kRiffSizeOffset = 4;
	constexpr unsigned int kWaveFormatOffset = 16;
	constexpr unsigned int kAudioFormatOffset = 20;
	constexpr unsigned int kChannelsOffset = 22;
	constexpr unsigned int kSampleRateOffset = 24;
	constexpr unsigned int kByteRateOffset = 28;
	constexpr unsigned int kBlockAlignOffset = 32;
	constexpr unsigned int kBitsPerSampleOffset = 34;
	constexpr unsigned int kDataSizeOffset = 40;
	constexpr float kPcm16Maximum = 32767.0f;
	constexpr float kTwoPi = 6.28318530718f;
	constexpr unsigned int kVoiceCountCases = 4;
	constexpr unsigned int kBlockSizeCases = 3;
	constexpr unsigned int kScenarioCount = 8;
	constexpr std::array<unsigned int, kVoiceCountCases> kVoiceCounts = {1, 16, 64, 128};
	constexpr std::array<unsigned int, kBlockSizeCases> kBlockSizes = {256, 512, 2048};
	constexpr float kFrequencyHz = 440.0f;
	constexpr float kFilterFrequencyHz = 4000.0f;
	constexpr float kFilterResonance = 0.707f;
	constexpr double kNanosecondsPerSecond = 1000000000.0;
	constexpr char kCsvDelimiter = ',';
	constexpr const char *kCsvHeader = "case,voices,block,ns_per_frame";

	struct Scenario
	{
		const char *name;
		unsigned int channels;
		bool stream;
		bool perVoiceFilter;
		bool globalFilter;
	};

	constexpr std::array<Scenario, kScenarioCount> kScenarios = {{
		{"pcm-mono", kChannelsMono, false, false, false},
		{"pcm-stereo", kChannelsStereo, false, false, false},
		{"wav-stream-mono", kChannelsMono, true, false, false},
		{"wav-stream-stereo", kChannelsStereo, true, false, false},
		{"pcm-per-voice-filter", kChannelsMono, false, true, false},
		{"pcm-stereo-per-voice-filter", kChannelsStereo, false, true, false},
		{"pcm-global-filter", kChannelsMono, false, false, true},
		{"pcm-stereo-global-filter", kChannelsStereo, false, false, true},
	}};

	struct Measurement
	{
		const char *scenario;
		unsigned int voices;
		unsigned int blockSize;
		double nanosecondsPerFrame;
	};

	bool writeChunkId(std::vector<std::uint8_t> &wav, unsigned int offset, const char *chunkId)
	{
		if (wav.size() < static_cast<std::size_t>(offset + kChunkIdBytes))
		{
			return false;
		}
		std::memcpy(wav.data() + offset, chunkId, kChunkIdBytes);
		return true;
	}

	void write16(std::vector<std::uint8_t> &bytes, unsigned int offset, std::uint16_t value)
	{
		bytes[offset] = static_cast<std::uint8_t>(value);
		bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
	}

	void write32(std::vector<std::uint8_t> &bytes, unsigned int offset, std::uint32_t value)
	{
		for (unsigned int byte = 0; byte < sizeof(value); ++byte)
		{
			bytes[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
		}
	}

	void makePcm(std::vector<float> &pcm, unsigned int channels)
	{
		pcm.resize(static_cast<std::size_t>(kSourceFrames) * channels);
		for (unsigned int frame = 0; frame < kSourceFrames; ++frame)
		{
			const float time = static_cast<float>(frame) / static_cast<float>(kSampleRate);
			const float sample = std::sin(kTwoPi * kFrequencyHz * time);
			for (unsigned int channel = 0; channel < channels; ++channel)
			{
				pcm[static_cast<std::size_t>(frame) * channels + channel] = sample;
			}
		}
	}

	bool makeWaveBytes(const std::vector<float> &pcm, unsigned int channels,
					   std::vector<std::uint8_t> &waveBytes)
	{
		const unsigned int dataBytes = static_cast<unsigned int>(pcm.size()) * kPcm16Bytes;
		waveBytes.assign(kWavHeaderBytes + dataBytes, 0);
		if (!writeChunkId(waveBytes, 0, "RIFF") ||
			!writeChunkId(waveBytes, 8, "WAVE") ||
			!writeChunkId(waveBytes, 12, "fmt ") ||
			!writeChunkId(waveBytes, 36, "data"))
		{
			return false;
		}

		const unsigned int blockAlign = channels * kPcm16Bytes;
		write32(waveBytes, kRiffSizeOffset, static_cast<std::uint32_t>(waveBytes.size() - kChunkIdBytes));
		write32(waveBytes, kWaveFormatOffset, kWavFormatChunkBytes);
		write16(waveBytes, kAudioFormatOffset, kWavPcmFormat);
		write16(waveBytes, kChannelsOffset, static_cast<std::uint16_t>(channels));
		write32(waveBytes, kSampleRateOffset, kSampleRate);
		write32(waveBytes, kByteRateOffset, kSampleRate * blockAlign);
		write16(waveBytes, kBlockAlignOffset, static_cast<std::uint16_t>(blockAlign));
		write16(waveBytes, kBitsPerSampleOffset, kPcm16Bits);
		write32(waveBytes, kDataSizeOffset, dataBytes);

		for (std::size_t sampleIndex = 0; sampleIndex < pcm.size(); ++sampleIndex)
		{
			const std::int16_t sample = static_cast<std::int16_t>(pcm[sampleIndex] * kPcm16Maximum);
			write16(waveBytes, kWavHeaderBytes + static_cast<unsigned int>(sampleIndex) * kPcm16Bytes,
					static_cast<std::uint16_t>(sample));
		}
		return true;
	}

	bool prepareSource(const Scenario &scenario, SoLoud::Wav &wav, SoLoud::WavStream &stream,
					SoLoud::BiquadResonantFilter &filter, SoLoud::AudioSource *&source,
					std::vector<float> &pcm, std::vector<std::uint8_t> &waveBytes)
	{
		makePcm(pcm, scenario.channels);
		if (scenario.stream)
		{
			if (!makeWaveBytes(pcm, scenario.channels, waveBytes) ||
				stream.loadMem(waveBytes.data(), static_cast<unsigned int>(waveBytes.size()), false, false) != SoLoud::SO_NO_ERROR)
			{
				return false;
			}
			source = &stream;
		}
		else
		{
			if (wav.loadRawWave(pcm.data(), static_cast<unsigned int>(pcm.size()),
							static_cast<float>(kSampleRate), scenario.channels, true, false) != SoLoud::SO_NO_ERROR)
			{
				return false;
			}
			source = &wav;
		}

		source->setLooping(true);
		if (scenario.perVoiceFilter || scenario.globalFilter)
		{
			if (filter.setParams(SoLoud::BiquadResonantFilter::LOWPASS,
								 kFilterFrequencyHz, kFilterResonance) != SoLoud::SO_NO_ERROR)
			{
				return false;
			}
		}
		if (scenario.perVoiceFilter)
		{
			source->setFilter(0, &filter);
		}
		return true;
	}

	bool startVoices(SoLoud::Soloud &soloud, SoLoud::AudioSource &source,
					 unsigned int voiceCount, std::vector<SoLoud::handle> &handles)
	{
		handles.clear();
		handles.reserve(voiceCount);
		for (unsigned int index = 0; index < voiceCount; ++index)
		{
			const SoLoud::handle handle = soloud.play(source);
			if (handle == 0)
			{
				return false;
			}
			handles.push_back(handle);
		}
		return true;
	}

	void stopVoices(SoLoud::Soloud &soloud, const std::vector<SoLoud::handle> &handles)
	{
		for (SoLoud::handle handle : handles)
		{
			soloud.stop(handle);
		}
	}

	double median(std::array<double, kTrials> &values)
	{
		std::sort(values.begin(), values.end());
		return values[values.size() / 2];
	}

	std::string makeKey(const Measurement &measurement)
	{
		return std::string(measurement.scenario) + kCsvDelimiter + std::to_string(measurement.voices) +
			kCsvDelimiter + std::to_string(measurement.blockSize);
	}

	void removeCarriageReturn(std::string &line)
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
	}

	bool parseUnsigned(const std::string &field, unsigned int &value)
	{
		if (field.empty() || field.front() == '-')
			return false;
		char *end = nullptr;
		const unsigned long parsed = std::strtoul(field.c_str(), &end, 10);
		if (end == field.c_str() || *end != '\0' || parsed > std::numeric_limits<unsigned int>::max())
			return false;
		value = static_cast<unsigned int>(parsed);
		return true;
	}

	bool readBaseline(const std::string &path, std::unordered_map<std::string, double> &baseline)
	{
		std::ifstream file(path);
		std::string line;
		if (!file || !std::getline(file, line))
			return false;
		removeCarriageReturn(line);
		if (line != kCsvHeader)
			return false;

		while (std::getline(file, line))
		{
			removeCarriageReturn(line);
			std::istringstream row(line);
			std::string scenario;
			std::string voicesField;
			std::string blockField;
			std::string elapsedField;
			if (!std::getline(row, scenario, kCsvDelimiter) ||
				!std::getline(row, voicesField, kCsvDelimiter) ||
				!std::getline(row, blockField, kCsvDelimiter) ||
				!std::getline(row, elapsedField) || scenario.empty() ||
				elapsedField.find(kCsvDelimiter) != std::string::npos)
			{
				return false;
			}
			Measurement measurement = {scenario.c_str(), 0, 0, 0.0};
			if (!parseUnsigned(voicesField, measurement.voices) ||
				!parseUnsigned(blockField, measurement.blockSize))
			{
				return false;
			}
			char *end = nullptr;
			const double elapsed = std::strtod(elapsedField.c_str(), &end);
			if (end == elapsedField.c_str() || *end != '\0' || !std::isfinite(elapsed) || elapsed <= 0.0 ||
				!baseline.emplace(makeKey(measurement), elapsed).second)
			{
				return false;
			}
		}
		return file.eof();
	}

	bool writeCsv(const std::string &path, const std::vector<Measurement> &measurements)
	{
	std::ofstream file(path);
	if (!file)
		return false;
	file << std::setprecision(17);
	file << kCsvHeader << '\n';
		for (const Measurement &measurement : measurements)
		{
			file << measurement.scenario << kCsvDelimiter << measurement.voices << kCsvDelimiter
				 << measurement.blockSize << kCsvDelimiter << measurement.nanosecondsPerFrame << '\n';
		}
		return file.good();
	}

	bool compareBaseline(const std::unordered_map<std::string, double> &baseline,
					 const std::vector<Measurement> &measurements, double maxRegressionPercent)
	{
		bool passed = true;
		for (const Measurement &measurement : measurements)
		{
			const std::string key = makeKey(measurement);
			const auto baselineResult = baseline.find(key);
			if (baselineResult == baseline.end())
			{
				std::printf("baseline missing: %s\n", key.c_str());
				passed = false;
				continue;
			}
			const double regressionPercent =
				(measurement.nanosecondsPerFrame - baselineResult->second) / baselineResult->second * 100.0;
			if (!std::isfinite(regressionPercent))
			{
				std::printf("invalid regression value: %s\n", key.c_str());
				passed = false;
				continue;
			}
			std::printf("regression case=%s change=%+.2f%% limit=%.2f%%\n",
					key.c_str(), regressionPercent, maxRegressionPercent);
			if (regressionPercent > maxRegressionPercent)
				passed = false;
		}
		if (baseline.size() != measurements.size())
		{
			std::printf("baseline case count mismatch: expected=%zu actual=%zu\n",
					measurements.size(), baseline.size());
			passed = false;
		}
		return passed;
	}

	bool pathsReferToSameFile(const std::string &left, const std::string &right, bool &succeeded)
	{
		std::error_code error;
		const bool leftExists = std::filesystem::exists(left, error);
		if (error)
		{
			succeeded = false;
			return false;
		}
		const bool rightExists = std::filesystem::exists(right, error);
		if (error)
		{
			succeeded = false;
			return false;
		}
		if (leftExists && rightExists)
		{
			const bool equivalent = std::filesystem::equivalent(left, right, error);
			if (error)
			{
				succeeded = false;
				return false;
			}
			if (equivalent)
			{
				succeeded = true;
				return true;
			}
		}
		const std::filesystem::path normalizedLeft = std::filesystem::weakly_canonical(left, error);
		if (error)
		{
			succeeded = false;
			return false;
		}
		const std::filesystem::path normalizedRight = std::filesystem::weakly_canonical(right, error);
		if (error)
		{
			succeeded = false;
			return false;
		}
		succeeded = true;
		return normalizedLeft == normalizedRight;
	}

	bool benchmark(SoLoud::Soloud &soloud, const Scenario &scenario, SoLoud::AudioSource &source,
				unsigned int voiceCount, unsigned int blockSize, std::vector<float> &buffer,
				std::vector<Measurement> &measurements)
	{
		buffer.resize(static_cast<std::size_t>(blockSize) * kChannelsStereo);
		std::array<double, kTrials> nanosecondsPerFrame = {};
		std::vector<SoLoud::handle> handles;

		for (double &trialResult : nanosecondsPerFrame)
		{
			if (!startVoices(soloud, source, voiceCount, handles))
			{
				stopVoices(soloud, handles);
				return false;
			}
			for (unsigned int block = 0; block < kWarmupBlocks; ++block)
			{
				soloud.mix(buffer.data(), blockSize);
			}
			if (soloud.getActiveVoiceCount() != voiceCount)
			{
				stopVoices(soloud, handles);
				return false;
			}

			const auto start = std::chrono::steady_clock::now();
			for (unsigned int block = 0; block < kMeasuredBlocks; ++block)
			{
				soloud.mix(buffer.data(), blockSize);
			}
			const auto end = std::chrono::steady_clock::now();
			const double elapsed =
				static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
			trialResult = elapsed / static_cast<double>(kMeasuredBlocks * blockSize);
			stopVoices(soloud, handles);
		}

		const double frameBudget = kNanosecondsPerSecond / static_cast<double>(kSampleRate);
		const double cost = median(nanosecondsPerFrame);
		const double realtimePercent = cost / frameBudget * 100.0;
		std::printf("case=%-21s voices=%3u block=%4u ns/frame=%8.2f realtime=%6.2f%%\n",
					scenario.name, voiceCount, blockSize, cost, realtimePercent);
		measurements.push_back({scenario.name, voiceCount, blockSize, cost});
		return true;
	}

	bool runScenario(SoLoud::Soloud &soloud, const Scenario &scenario, std::vector<Measurement> &measurements)
	{
		std::vector<float> pcm;
		std::vector<std::uint8_t> waveBytes;
		SoLoud::BiquadResonantFilter filter;
		SoLoud::Wav wav;
		SoLoud::WavStream stream;
		SoLoud::AudioSource *source = nullptr;
		if (!prepareSource(scenario, wav, stream, filter, source, pcm, waveBytes))
			return false;
		if (scenario.globalFilter)
		{
			soloud.setGlobalFilter(0, &filter);
		}

		std::vector<float> buffer;
		bool succeeded = true;
		for (unsigned int voices : kVoiceCounts)
		{
			for (unsigned int blockSize : kBlockSizes)
			{
				if (!benchmark(soloud, scenario, *source, voices, blockSize, buffer, measurements))
				{
					succeeded = false;
					break;
				}
			}
			if (!succeeded)
			{
				break;
			}
		}
		if (scenario.globalFilter)
		{
			soloud.setGlobalFilter(0, nullptr);
		}
		return succeeded;
	}
}

int main(int argc, char *argv[])
{
	std::string csvPath;
	std::string baselinePath;
	double maxRegressionPercent = 0.0;
	bool hasRegressionLimit = false;
	for (int argument = 1; argument < argc; ++argument)
	{
		const std::string option = argv[argument];
		if ((option == "--csv" || option == "--baseline" || option == "--max-regression-percent") &&
			argument + 1 < argc)
		{
			const std::string value = argv[++argument];
			if (option == "--csv")
				csvPath = value;
			else if (option == "--baseline")
				baselinePath = value;
			else
			{
				char *end = nullptr;
				maxRegressionPercent = std::strtod(value.c_str(), &end);
				if (end == value.c_str() || *end != '\0' || !std::isfinite(maxRegressionPercent) ||
					maxRegressionPercent < 0.0)
				{
					std::printf("invalid regression limit\n");
					return 2;
				}
				hasRegressionLimit = true;
			}
		}
		else
		{
			std::printf("usage: soloud_mix_benchmark [--csv path] [--baseline path --max-regression-percent value]\n");
			return 2;
		}
	}
	if (baselinePath.empty() != !hasRegressionLimit)
	{
		std::printf("baseline and regression limit must be provided together\n");
		return 2;
	}
	if (!csvPath.empty() && !baselinePath.empty())
	{
		bool pathComparisonSucceeded = false;
		if (pathsReferToSameFile(csvPath, baselinePath, pathComparisonSucceeded))
		{
			std::printf("CSV output path must differ from baseline path\n");
			return 2;
		}
		if (!pathComparisonSucceeded)
		{
			std::printf("could not compare CSV and baseline paths\n");
			return 2;
		}
	}
	std::unordered_map<std::string, double> baseline;
	if (!baselinePath.empty() && !readBaseline(baselinePath, baseline))
	{
		std::printf("could not read benchmark baseline: %s\n", baselinePath.c_str());
		return 1;
	}

	SoLoud::Soloud soloud;
	if (soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::NULLDRIVER,
				kSampleRate, kBackendBufferFrames, kChannelsStereo) != SoLoud::SO_NO_ERROR)
	{
		std::printf("benchmark init failed\n");
		return 1;
	}
	if (soloud.setMaxActiveVoiceCount(kMaxVoices) != SoLoud::SO_NO_ERROR)
	{
		std::printf("benchmark voice limit setup failed\n");
		soloud.deinit();
		return 1;
	}

	std::vector<Measurement> measurements;
	for (const Scenario &scenario : kScenarios)
	{
		if (!runScenario(soloud, scenario, measurements))
		{
			std::printf("benchmark scenario failed: %s\n", scenario.name);
			soloud.deinit();
			return 1;
		}
	}

	soloud.deinit();
	if (!csvPath.empty() && !writeCsv(csvPath, measurements))
	{
		std::printf("could not write benchmark CSV: %s\n", csvPath.c_str());
		return 1;
	}
	if (!baselinePath.empty())
	{
		if (!compareBaseline(baseline, measurements, maxRegressionPercent))
			return 1;
	}
	return 0;
}
