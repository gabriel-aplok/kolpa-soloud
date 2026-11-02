#ifndef SOLOUD_MINIAUDIO_BACKEND_H
#define SOLOUD_MINIAUDIO_BACKEND_H

#include <vector>
#include <string>

#include "miniaudio.h"
#include "soloud.hpp"

namespace SoLoud
{
	struct MiniaudioDevice
	{
		ma_device_id id;
		std::string name;
		bool isDefault;
	};

	enum class MiniaudioRecoveryStatus
	{
		Idle,
		Recovering,
		SelectedDeviceRestored,
		DefaultDeviceRestored,
		Failed,
	};

	using MiniaudioRecoveryCallback = void (*)(MiniaudioRecoveryStatus status, void *userData);

	result getMiniaudioPlaybackDevices(std::vector<MiniaudioDevice> &devices);
	result setPreferredMiniaudioDevice(const MiniaudioDevice *device);
	void setMiniaudioRecoveryCallback(MiniaudioRecoveryCallback callback, void *userData);
	MiniaudioRecoveryStatus getMiniaudioRecoveryStatus();
}

#endif
