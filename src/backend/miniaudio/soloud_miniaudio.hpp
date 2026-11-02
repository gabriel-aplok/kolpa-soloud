#ifndef SOLOUD_MINIAUDIO_BACKEND_H
#define SOLOUD_MINIAUDIO_BACKEND_H

#include <vector>
#include <string>

#include "miniaudio.h"
#include "soloud.hpp"

namespace SoLoud
{
	struct MiniaudioDeviceFormat
	{
		ma_format format;
		unsigned int channels;
		unsigned int sampleRate;
	};

	struct MiniaudioDevice
	{
		ma_device_id id;
		std::string name;
		bool isDefault;
		bool capabilitiesAvailable;
		std::vector<MiniaudioDeviceFormat> formats;
	};

	struct MiniaudioLatencyInfo
	{
		unsigned int periodSizeInFrames;
		unsigned int periods;
		unsigned int sampleRate;
		double estimatedMilliseconds;
	};

	enum class MiniaudioRecoveryStatus
	{
		Idle,
		Recovering,
		SelectedDeviceRestored,
		DefaultDeviceRestored,
		OutputDeviceSwitched,
		OutputDeviceSwitchFailed,
		Failed,
	};

	using MiniaudioRecoveryCallback = void (*)(MiniaudioRecoveryStatus status, void *userData);

	result getMiniaudioPlaybackDevices(std::vector<MiniaudioDevice> &devices);
	result setPreferredMiniaudioDevice(const MiniaudioDevice *device);
	result switchMiniaudioPlaybackDevice(const MiniaudioDevice *device);
	result recoverMiniaudioPlaybackDevice();
	result setMiniaudioLatencyConfig(unsigned int periodSizeInFrames, unsigned int periods);
	result getMiniaudioLatencyInfo(MiniaudioLatencyInfo &info);
	void setMiniaudioRecoveryCallback(MiniaudioRecoveryCallback callback, void *userData);
	MiniaudioRecoveryStatus getMiniaudioRecoveryStatus();
}

#endif
