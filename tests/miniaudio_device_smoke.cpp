#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <vector>

#include "backend/miniaudio/soloud_miniaudio.hpp"

namespace SoLoud
{
	extern ma_device gDevice;
}

namespace
{
	constexpr unsigned int kSampleRate = 44100;
	constexpr unsigned int kBufferSize = 2048;
	constexpr unsigned int kChannels = 2;
	constexpr unsigned int kPeriodSizeInFrames = 512;
	constexpr unsigned int kPeriods = 3;
	constexpr unsigned int kExpectedStatusEvents = 16;
	std::atomic<unsigned int> gStatusEvents = 0;
	std::atomic<int> gLastCallbackStatus = static_cast<int>(SoLoud::MiniaudioRecoveryStatus::Idle);
	std::atomic<bool> gCallbackStatusMatches = true;
	std::atomic<bool> gCallbackDeinitialized = false;
	std::mutex gCallbackMutex;
	std::condition_variable gCallbackCondition;

	void recordRecoveryStatus(SoLoud::MiniaudioRecoveryStatus status, void *)
	{
		gLastCallbackStatus.store(static_cast<int>(status));
		if (SoLoud::getMiniaudioRecoveryStatus() != status)
			gCallbackStatusMatches.store(false);
		++gStatusEvents;
		gCallbackCondition.notify_all();
	}

	void deinitializeFromRecoveryCallback(SoLoud::MiniaudioRecoveryStatus status, void *userData)
	{
		if (status == SoLoud::MiniaudioRecoveryStatus::Recovering)
		{
			static_cast<SoLoud::Soloud *>(userData)->deinit();
			gCallbackDeinitialized.store(true);
			gCallbackCondition.notify_all();
		}
	}
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
		std::printf("device=%s default=%s capabilities=%s native-formats=%zu\n", device.name.c_str(),
				device.isDefault ? "yes" : "no", device.capabilitiesAvailable ? "reported" : "unavailable",
				device.formats.size());
		if (device.isDefault)
			defaultDevice = &device;
	}
	if (defaultDevice == nullptr || SoLoud::setPreferredMiniaudioDevice(defaultDevice) != SoLoud::SO_NO_ERROR)
	{
		std::printf("miniaudio default device selection failed\n");
		return 1;
	}
	if (SoLoud::setMiniaudioLatencyConfig(kPeriodSizeInFrames, 0) != SoLoud::INVALID_PARAMETER ||
		SoLoud::setMiniaudioLatencyConfig(kPeriodSizeInFrames, kPeriods) != SoLoud::SO_NO_ERROR)
	{
		std::printf("miniaudio latency configuration failed\n");
		return 1;
	}
	SoLoud::setMiniaudioRecoveryCallback(recordRecoveryStatus, nullptr);

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
	SoLoud::MiniaudioLatencyInfo latencyInfo;
	if (SoLoud::setMiniaudioLatencyConfig(kPeriodSizeInFrames, kPeriods) != SoLoud::INVALID_PARAMETER ||
		SoLoud::getMiniaudioLatencyInfo(latencyInfo) != SoLoud::SO_NO_ERROR ||
		latencyInfo.periodSizeInFrames == 0 || latencyInfo.periods == 0 || latencyInfo.estimatedMilliseconds <= 0.0)
	{
		soloud.deinit();
		std::printf("miniaudio latency query failed\n");
		return 1;
	}
	for (unsigned int iteration = 0; iteration < 2; ++iteration)
	{
		const SoLoud::result switchResult = SoLoud::switchMiniaudioPlaybackDevice(defaultDevice);
		const SoLoud::MiniaudioRecoveryStatus switchStatus = SoLoud::getMiniaudioRecoveryStatus();
		const SoLoud::result recoveryResult = SoLoud::recoverMiniaudioPlaybackDevice();
		const SoLoud::MiniaudioRecoveryStatus recoveryStatus = SoLoud::getMiniaudioRecoveryStatus();
		if (switchResult != SoLoud::SO_NO_ERROR ||
			switchStatus != SoLoud::MiniaudioRecoveryStatus::OutputDeviceSwitched ||
			recoveryResult != SoLoud::SO_NO_ERROR ||
			recoveryStatus != SoLoud::MiniaudioRecoveryStatus::SelectedDeviceRestored)
		{
			soloud.deinit();
			std::printf("miniaudio operation failed: switch=%d status=%d recovery=%d status=%d\n",
				switchResult, static_cast<int>(switchStatus), recoveryResult, static_cast<int>(recoveryStatus));
			return 1;
		}
	}
	for (unsigned int loss = 0; loss < 2; ++loss)
	{
		const unsigned int priorEvents = gStatusEvents.load();
		if (ma_device_stop(&SoLoud::gDevice) != MA_SUCCESS)
		{
			soloud.deinit();
			std::printf("miniaudio simulated loss failed to stop device\n");
			return 1;
		}
		std::unique_lock<std::mutex> callbackLock(gCallbackMutex);
		const bool recovered = gCallbackCondition.wait_for(callbackLock, std::chrono::seconds(5), [&]
		{
			return gStatusEvents.load() >= priorEvents + 2;
		});
		if (!recovered || SoLoud::getMiniaudioRecoveryStatus() != SoLoud::MiniaudioRecoveryStatus::SelectedDeviceRestored)
		{
			callbackLock.unlock();
			soloud.deinit();
			std::printf("miniaudio repeated loss recovery failed\n");
			return 1;
		}
	}
	if (SoLoud::switchMiniaudioPlaybackDevice(nullptr) != SoLoud::SO_NO_ERROR ||
		SoLoud::recoverMiniaudioPlaybackDevice() != SoLoud::SO_NO_ERROR ||
		SoLoud::getMiniaudioRecoveryStatus() != SoLoud::MiniaudioRecoveryStatus::DefaultDeviceRestored ||
		gStatusEvents.load() != kExpectedStatusEvents ||
		gLastCallbackStatus.load() != static_cast<int>(SoLoud::MiniaudioRecoveryStatus::DefaultDeviceRestored) ||
		!gCallbackStatusMatches.load())
	{
		soloud.deinit();
		std::printf("miniaudio default recovery or callback count failed\n");
		return 1;
	}
	SoLoud::setMiniaudioRecoveryCallback(deinitializeFromRecoveryCallback, &soloud);
	const SoLoud::result callbackRecoveryResult = SoLoud::recoverMiniaudioPlaybackDevice();
	std::unique_lock<std::mutex> callbackLock(gCallbackMutex);
	const bool callbackFinished = gCallbackCondition.wait_for(callbackLock, std::chrono::seconds(5), []
	{
		return gCallbackDeinitialized.load();
	});
	callbackLock.unlock();
	if (callbackRecoveryResult == SoLoud::SO_NO_ERROR || !callbackFinished ||
		SoLoud::getMiniaudioRecoveryStatus() != SoLoud::MiniaudioRecoveryStatus::Recovering)
	{
		std::printf("miniaudio callback shutdown failed: result=%d called=%d status=%d\n",
			callbackRecoveryResult, gCallbackDeinitialized.load(),
			static_cast<int>(SoLoud::getMiniaudioRecoveryStatus()));
		return 1;
	}
	if (soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::MINIAUDIO,
				kSampleRate, kBufferSize, kChannels) != SoLoud::SO_NO_ERROR)
	{
		std::printf("miniaudio restart after callback shutdown failed\n");
		return 1;
	}

	soloud.deinit();
	if (SoLoud::setMiniaudioLatencyConfig(0, 0) != SoLoud::SO_NO_ERROR ||
		SoLoud::getMiniaudioLatencyInfo(latencyInfo) != SoLoud::INVALID_PARAMETER)
	{
		std::printf("miniaudio shutdown state failed\n");
		return 1;
	}
	std::printf("miniaudio device smoke ok\n");
	return 0;
}
