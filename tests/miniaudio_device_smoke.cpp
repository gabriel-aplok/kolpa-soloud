#include <cstdio>
#include <vector>

#include "backend/miniaudio/soloud_miniaudio.hpp"

namespace
{
	constexpr unsigned int kSampleRate = 44100;
	constexpr unsigned int kBufferSize = 2048;
	constexpr unsigned int kChannels = 2;
}

int main()
{
	std::vector<SoLoud::MiniaudioDevice> devices;
	if (SoLoud::getMiniaudioPlaybackDevices(devices) != SoLoud::SO_NO_ERROR)
	{
		std::printf("miniaudio device enumeration failed\n");
		return 1;
	}
	if (devices.empty())
	{
		std::printf("no miniaudio playback devices found\n");
		return 1;
	}

	const SoLoud::MiniaudioDevice *defaultDevice = nullptr;
	for (const SoLoud::MiniaudioDevice &device : devices)
	{
		std::printf("device=%s default=%s\n", device.name.c_str(), device.isDefault ? "yes" : "no");
		if (device.isDefault)
			defaultDevice = &device;
	}
	if (defaultDevice == nullptr || SoLoud::setPreferredMiniaudioDevice(defaultDevice) != SoLoud::SO_NO_ERROR)
	{
		std::printf("miniaudio default device selection failed\n");
		return 1;
	}

	SoLoud::Soloud soloud;
	if (soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::MINIAUDIO,
				kSampleRate, kBufferSize, kChannels) != SoLoud::SO_NO_ERROR)
	{
		std::printf("miniaudio device startup failed\n");
		return 1;
	}
	if (SoLoud::getMiniaudioRecoveryStatus() != SoLoud::MiniaudioRecoveryStatus::Idle)
	{
		soloud.deinit();
		std::printf("unexpected miniaudio recovery status after startup\n");
		return 1;
	}

	soloud.deinit();
	std::printf("miniaudio device smoke ok\n");
	return 0;
}
