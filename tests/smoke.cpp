#include <cmath>
#include <cstdio>

#include "audiosource/sfxr/soloud_sfxr.hpp"
#include "soloud.hpp"

namespace
{
	constexpr unsigned int kSampleRate = 44100;
	constexpr unsigned int kBufferSize = 2048;
	constexpr unsigned int kChannels = 2;
	constexpr int kBlocks = 8;
	constexpr int kSfxrPreset = SoLoud::Sfxr::COIN;
	constexpr int kSfxrSeed = 1234;
	constexpr double kMinimumEnergy = 0.001;

	float gBuffer[kBufferSize * kChannels];
}

int main()
{
	SoLoud::Soloud soloud;
	if (soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::NULLDRIVER, kSampleRate, kBufferSize, kChannels) != SoLoud::SO_NO_ERROR)
	{
		std::printf("soloud init failed\n");
		return 1;
	}

	SoLoud::Sfxr sfxr;
	if (sfxr.loadPreset(kSfxrPreset, kSfxrSeed) != SoLoud::SO_NO_ERROR)
	{
		std::printf("sfxr preset failed\n");
		soloud.deinit();
		return 1;
	}

	soloud.play(sfxr);

	double energy = 0.0;
	for (int block = 0; block < kBlocks; block++)
	{
		soloud.mix(gBuffer, kBufferSize);
		for (unsigned int i = 0; i < kBufferSize * kChannels; i++)
		{
			energy += std::fabs(gBuffer[i]);
		}
	}

	if (energy <= kMinimumEnergy)
	{
		soloud.deinit();
		std::printf("soloud mixed silence\n");
		return 1;
	}

	std::printf("soloud smoke ok: backend=%s energy=%.3f\n", soloud.getBackendString(), energy);
	soloud.deinit();
	return 0;
}
