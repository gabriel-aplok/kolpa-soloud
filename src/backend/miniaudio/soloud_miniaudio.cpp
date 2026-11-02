/*
SoLoud audio engine
Copyright (c) 2013-2020 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/
#include <stdlib.h>

#include "soloud.hpp"
#include "soloud_miniaudio.hpp"
#include <mutex>
#include <condition_variable>
#include <thread>
#include <system_error>
#include <new>
#include <vector>
#include <utility>

#if !defined(WITH_MINIAUDIO)

namespace SoLoud
{
	result getMiniaudioPlaybackDevices(std::vector<MiniaudioDevice> &devices) { devices.clear(); return NOT_IMPLEMENTED; }
	result setPreferredMiniaudioDevice(const MiniaudioDevice *) { return NOT_IMPLEMENTED; }
	result switchMiniaudioPlaybackDevice(const MiniaudioDevice *) { return NOT_IMPLEMENTED; }
	result recoverMiniaudioPlaybackDevice() { return NOT_IMPLEMENTED; }
	result setMiniaudioLatencyConfig(unsigned int, unsigned int) { return NOT_IMPLEMENTED; }
	result getMiniaudioLatencyInfo(MiniaudioLatencyInfo &) { return NOT_IMPLEMENTED; }
	void setMiniaudioRecoveryCallback(MiniaudioRecoveryCallback, void *) {}
	MiniaudioRecoveryStatus getMiniaudioRecoveryStatus() { return MiniaudioRecoveryStatus::Idle; }

    result miniaudio_init(SoLoud::Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer)
    {
        return NOT_IMPLEMENTED;
    }
}

#else

/* single miniaudio implementation for the engine and this backend */
#define MA_NO_JACK
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#define MA_COINIT_VALUE 2

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <math.h>
#include "soloud_miniaudio_recovery_policy.hpp"

namespace SoLoud
{
    ma_device gDevice;
    ma_context gContext;
    bool gContextInitialized = false;
    bool gDeviceInitialized = false;
    bool gRecoveryEnabled = false;
    bool gBackendInitializing = false;
    bool gRecovering = false;
    bool gShutdown = false;
    bool gOperationPending = false;
    bool gOperationRunning = false;
    bool gOperationCompleted = false;
    bool gOperationSucceeded = false;
    bool gDeviceActive = false;
    bool gStartupInitialized = false;
    bool gStartupReady = false;
    bool gStartupCompleted = false;
    bool gStartupSucceeded = false;
    bool gStartupHasPreferredDevice = false;
    bool gHasPreferredDevice = false;
    bool gHasActiveDevice = false;
    bool gSwitchUsesDevice = false;
    bool gSynchronousRequest = false;
    bool gWorkerExited = true;
    ma_device_id gPreferredDevice;
    ma_device_id gStartupPreferredDevice;
    ma_device_id gActiveDevice;
    ma_device_id gSwitchDevice;
    bool gSwitchToRecovery = false;
    unsigned int gSamplerate = 0;
    unsigned int gChannels = 0;
    unsigned int gPeriodSizeInFrames = 0;
    unsigned int gPeriods = 0;
    Soloud *gSoloud = nullptr;
    MiniaudioRecoveryStatus gRecoveryStatus = MiniaudioRecoveryStatus::Idle;
    MiniaudioRecoveryCallback gRecoveryCallback = nullptr;
    void *gRecoveryUserData = nullptr;
    std::mutex gRecoveryMutex;
    std::condition_variable gRecoveryCondition;
    std::thread gRecoveryThread;
    std::thread::id gWorkerThreadId;

    struct WorkerExitGuard
    {
        ~WorkerExitGuard()
        {
            if (gDeviceInitialized)
            {
                ma_device_uninit(&gDevice);
                gDeviceInitialized = false;
            }
            if (gContextInitialized)
            {
                ma_context_uninit(&gContext);
                gContextInitialized = false;
            }
            {
                std::lock_guard<std::mutex> lock(gRecoveryMutex);
                gWorkerExited = true;
                gWorkerThreadId = std::thread::id();
                if (gShutdown)
                    gSoloud = nullptr;
            }
            gRecoveryCondition.notify_all();
        }
    };

    void soloud_miniaudio_audiomixer(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount);

    static bool sameDevice(const ma_device_id &left, const ma_device_id &right)
    {
        return ma_device_id_equal(&left, &right) != MA_FALSE;
    }

    static result enumeratePlaybackDevices(std::vector<MiniaudioDevice> &devices)
    {
        devices.clear();
        ma_context context;
        if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS)
            return UNKNOWN_ERROR;
        ma_device_info *infos = NULL;
        ma_uint32 count = 0;
        const ma_result enumerationResult = ma_context_get_devices(&context, &infos, &count, NULL, NULL);
        if (enumerationResult == MA_SUCCESS)
        {
            try
            {
                for (ma_uint32 index = 0; index < count; ++index)
                {
                    MiniaudioDevice device = {infos[index].id, infos[index].name,
                                               infos[index].isDefault != MA_FALSE,
                                               infos[index].nativeDataFormatCount > 0, {}};
                    for (ma_uint32 formatIndex = 0; formatIndex < infos[index].nativeDataFormatCount; ++formatIndex)
                    {
                        const auto &nativeFormat = infos[index].nativeDataFormats[formatIndex];
                        device.formats.push_back({nativeFormat.format, nativeFormat.channels, nativeFormat.sampleRate});
                    }
                    devices.push_back(std::move(device));
                }
            }
            catch (const std::bad_alloc &)
            {
                devices.clear();
                ma_context_uninit(&context);
                return UNKNOWN_ERROR;
            }
        }
        ma_context_uninit(&context);
        return enumerationResult == MA_SUCCESS ? SO_NO_ERROR : UNKNOWN_ERROR;
    }

    result getMiniaudioPlaybackDevices(std::vector<MiniaudioDevice> &devices)
    {
        return enumeratePlaybackDevices(devices);
    }

    result setPreferredMiniaudioDevice(const MiniaudioDevice *device)
    {
        std::lock_guard<std::mutex> lock(gRecoveryMutex);
        if (gRecoveryEnabled || gBackendInitializing || gRecovering)
            return INVALID_PARAMETER;
        if (device != nullptr)
        {
            std::vector<MiniaudioDevice> devices;
            if (enumeratePlaybackDevices(devices) != SO_NO_ERROR)
                return UNKNOWN_ERROR;
            bool found = false;
            for (const MiniaudioDevice &availableDevice : devices)
            {
                if (sameDevice(availableDevice.id, device->id))
                {
                    found = true;
                    break;
                }
            }
            if (!found)
                return INVALID_PARAMETER;
        }
        gHasPreferredDevice = device != nullptr;
        if (device != nullptr)
            gPreferredDevice = device->id;
        return SO_NO_ERROR;
    }

    void setMiniaudioRecoveryCallback(MiniaudioRecoveryCallback callback, void *userData)
    {
        std::lock_guard<std::mutex> lock(gRecoveryMutex);
        gRecoveryCallback = callback;
        gRecoveryUserData = userData;
    }

    MiniaudioRecoveryStatus getMiniaudioRecoveryStatus()
    {
        std::lock_guard<std::mutex> lock(gRecoveryMutex);
        return gRecoveryStatus;
    }

    static ma_result initializeDevice(const ma_device_id *deviceId)
    {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = gChannels;
        config.playback.pDeviceID = deviceId;
        config.periodSizeInFrames = gPeriodSizeInFrames;
        config.periods = gPeriods;
        config.sampleRate = gSamplerate;
        config.dataCallback = soloud_miniaudio_audiomixer;
        config.notificationCallback = [](const ma_device_notification *notification)
        {
            if (notification->type != ma_device_notification_type_stopped)
                return;
            std::lock_guard<std::mutex> lock(gRecoveryMutex);
            if (gRecoveryEnabled && !gRecovering && !gOperationPending && !gOperationRunning && !gShutdown)
            {
                gSwitchToRecovery = true;
                gOperationPending = true;
                gRecoveryCondition.notify_one();
            }
        };
        config.pUserData = gSoloud;
        const ma_result initializationResult = ma_device_init(&gContext, &config, &gDevice);
        if (initializationResult != MA_SUCCESS)
            return MA_ERROR;
        gDeviceInitialized = true;
        return MA_SUCCESS;
    }

    static bool startDevice(const ma_device_id *deviceId)
    {
        const ma_result startResult = ma_device_start(&gDevice);
        if (startResult != MA_SUCCESS)
        {
            ma_device_uninit(&gDevice);
            gDeviceInitialized = false;
            return false;
        }
        gHasActiveDevice = deviceId != nullptr;
        if (deviceId != nullptr)
            gActiveDevice = *deviceId;
        return true;
    }

    static bool openDevice(const ma_device_id *deviceId)
    {
        return initializeDevice(deviceId) == MA_SUCCESS && startDevice(deviceId);
    }

    static void finishStartup(bool succeeded)
    {
        {
            std::lock_guard<std::mutex> lock(gRecoveryMutex);
            gStartupSucceeded = succeeded;
            gStartupCompleted = true;
            gDeviceActive = succeeded;
            gRecoveryEnabled = succeeded;
            gRecoveryStatus = MiniaudioRecoveryStatus::Idle;
        }
        gRecoveryCondition.notify_all();
    }

    static void publishRecoveryStatus(MiniaudioRecoveryStatus status)
    {
        MiniaudioRecoveryCallback callback;
        void *userData;
        {
            std::lock_guard<std::mutex> lock(gRecoveryMutex);
            gRecoveryStatus = status;
            if (gShutdown)
                return;
            callback = gRecoveryCallback;
            userData = gRecoveryUserData;
        }
        if (callback != nullptr)
            callback(status, userData);
    }

    static void runDeviceWorker()
    {
        {
            std::lock_guard<std::mutex> lock(gRecoveryMutex);
            gWorkerExited = false;
            gWorkerThreadId = std::this_thread::get_id();
        }
        WorkerExitGuard exitGuard;
        if (ma_context_init(NULL, 0, NULL, &gContext) != MA_SUCCESS)
        {
            std::lock_guard<std::mutex> lock(gRecoveryMutex);
            gStartupCompleted = true;
            gStartupSucceeded = false;
            gRecoveryCondition.notify_all();
            return;
        }
        gContextInitialized = true;
        const bool openedPreferred = gStartupHasPreferredDevice &&
            initializeDevice(&gStartupPreferredDevice) == MA_SUCCESS;
        if (!openedPreferred && initializeDevice(NULL) != MA_SUCCESS)
        {
            std::lock_guard<std::mutex> lock(gRecoveryMutex);
            gStartupCompleted = true;
            gStartupSucceeded = false;
            gRecoveryCondition.notify_all();
            return;
        }
        std::unique_lock<std::mutex> lock(gRecoveryMutex);
        gStartupInitialized = true;
        gRecoveryCondition.notify_all();
        gRecoveryCondition.wait(lock, [] { return gShutdown || gStartupReady; });
        if (gShutdown)
        {
            lock.unlock();
            if (gDeviceInitialized)
            {
                ma_device_uninit(&gDevice);
                gDeviceInitialized = false;
            }
            return;
        }
        const ma_device_id initialPreferred = gStartupPreferredDevice;
        lock.unlock();
        const bool started = startDevice(openedPreferred ? &initialPreferred : NULL);
        bool startupSucceeded = started;
        if (!started && openedPreferred)
            startupSucceeded = openDevice(NULL);
        finishStartup(startupSucceeded);
        if (!startupSucceeded)
        {
            return;
        }
        lock.lock();
        for (;;)
        {
            gRecoveryCondition.wait(lock, [] { return gShutdown || gOperationPending; });
            if (gShutdown)
            {
                lock.unlock();
                if (gDeviceInitialized)
                {
                    ma_device_uninit(&gDevice);
                    gDeviceInitialized = false;
                }
                return;
            }
            const bool isRecovery = gSwitchToRecovery;
            const bool synchronousRequest = gSynchronousRequest;
            const bool switchUsesDevice = gSwitchUsesDevice;
            const ma_device_id requestedDevice = gSwitchDevice;
            const bool hadActiveDevice = gDeviceActive && gHasActiveDevice;
            const ma_device_id previousDevice = gActiveDevice;
            const bool hadPreferredDevice = gHasPreferredDevice;
            const ma_device_id preferredDevice = gPreferredDevice;
            gOperationPending = false;
            gOperationRunning = true;
            gRecovering = true;
            gDeviceActive = false;
            lock.unlock();

            if (gDeviceInitialized)
            {
                ma_device_uninit(&gDevice);
                gDeviceInitialized = false;
            }
            publishRecoveryStatus(MiniaudioRecoveryStatus::Recovering);
            lock.lock();
            if (gShutdown)
            {
                if (synchronousRequest)
                {
                    gOperationSucceeded = false;
                    gOperationCompleted = true;
                }
                gOperationPending = false;
                gOperationRunning = false;
                gRecovering = false;
                gRecoveryCondition.notify_all();
                lock.unlock();
                if (gDeviceInitialized)
                {
                    ma_device_uninit(&gDevice);
                    gDeviceInitialized = false;
                }
                return;
            }
            lock.unlock();
            bool restoredRequested = false;
            bool restoredPrevious = false;
            bool restoredDefault = false;
            if (isRecovery)
            {
                const MiniaudioRecoveryTarget recoveryTarget = chooseMiniaudioRecoveryTarget(
                    hadPreferredDevice, [&](bool selected)
                    {
                        const ma_device_id *deviceId = selected ? &preferredDevice : NULL;
                        return openDevice(deviceId);
                    });
                restoredRequested = recoveryTarget == MiniaudioRecoveryTarget::Selected;
                restoredDefault = recoveryTarget == MiniaudioRecoveryTarget::Default;
            }
            else
            {
                restoredRequested = openDevice(switchUsesDevice ? &requestedDevice : NULL);
                if (!restoredRequested)
                {
                    restoredPrevious = hadActiveDevice && openDevice(&previousDevice);
                    restoredDefault = !restoredPrevious && openDevice(NULL);
                }
            }

            lock.lock();
            if (gShutdown)
            {
                gOperationSucceeded = false;
                gOperationCompleted = synchronousRequest;
                gOperationPending = false;
                gOperationRunning = false;
                gRecovering = false;
                lock.unlock();
                if (gDeviceInitialized)
                {
                    ma_device_uninit(&gDevice);
                    gDeviceInitialized = false;
                }
                gRecoveryCondition.notify_all();
                return;
            }
            gRecovering = false;
            gOperationRunning = false;
            gDeviceActive = restoredRequested || restoredPrevious || restoredDefault;
            MiniaudioRecoveryStatus status;
            if (isRecovery)
            {
                status = restoredRequested ? MiniaudioRecoveryStatus::SelectedDeviceRestored
                    : restoredDefault ? MiniaudioRecoveryStatus::DefaultDeviceRestored
                    : MiniaudioRecoveryStatus::Failed;
            }
            else
            {
                status = restoredRequested ? MiniaudioRecoveryStatus::OutputDeviceSwitched
                    : gDeviceActive ? MiniaudioRecoveryStatus::OutputDeviceSwitchFailed
                    : MiniaudioRecoveryStatus::Failed;
                gOperationSucceeded = restoredRequested;
                gOperationCompleted = true;
                if (restoredRequested)
                {
                    gHasPreferredDevice = switchUsesDevice;
                    if (switchUsesDevice)
                        gPreferredDevice = requestedDevice;
                }
                else if (!restoredPrevious && restoredDefault)
                {
                    gHasPreferredDevice = false;
                }
            }
            if (isRecovery && synchronousRequest)
            {
                gOperationSucceeded = restoredRequested || restoredDefault;
                gOperationCompleted = true;
            }
            gSynchronousRequest = false;
            if (!gDeviceActive)
            {
                gHasActiveDevice = false;
            }
            lock.unlock();
            publishRecoveryStatus(status);
            gRecoveryCondition.notify_all();
            lock.lock();
        }
    }

    static bool isAvailableDevice(const MiniaudioDevice &device)
    {
        std::vector<MiniaudioDevice> devices;
        if (enumeratePlaybackDevices(devices) != SO_NO_ERROR)
            return false;
        for (const MiniaudioDevice &availableDevice : devices)
        {
            if (sameDevice(availableDevice.id, device.id))
                return true;
        }
        return false;
    }

    result switchMiniaudioPlaybackDevice(const MiniaudioDevice *device)
    {
        if (device != nullptr && !isAvailableDevice(*device))
            return INVALID_PARAMETER;
        std::unique_lock<std::mutex> lock(gRecoveryMutex);
        if (!gRecoveryEnabled || gShutdown || gWorkerThreadId == std::this_thread::get_id())
            return INVALID_PARAMETER;
        gRecoveryCondition.wait(lock, []
        {
                return gShutdown || (!gOperationPending && !gOperationRunning && !gOperationCompleted);
        });
        if (gShutdown)
            return INVALID_PARAMETER;
        gSwitchUsesDevice = device != nullptr;
        if (device != nullptr)
            gSwitchDevice = device->id;
        gSwitchToRecovery = false;
        gSynchronousRequest = true;
        gOperationPending = true;
        gRecoveryCondition.notify_one();
        gRecoveryCondition.wait(lock, [] { return gShutdown || gOperationCompleted; });
        if (gShutdown || !gOperationCompleted)
            return UNKNOWN_ERROR;
        const bool succeeded = gOperationSucceeded;
        gOperationCompleted = false;
        gRecoveryCondition.notify_all();
        return succeeded ? SO_NO_ERROR : UNKNOWN_ERROR;
    }

    result recoverMiniaudioPlaybackDevice()
    {
        std::unique_lock<std::mutex> lock(gRecoveryMutex);
        if (!gRecoveryEnabled || gShutdown || gWorkerThreadId == std::this_thread::get_id())
            return INVALID_PARAMETER;
        gRecoveryCondition.wait(lock, []
        {
            return gShutdown || (!gOperationPending && !gOperationRunning && !gOperationCompleted);
        });
        if (gShutdown)
            return INVALID_PARAMETER;
        gSwitchToRecovery = true;
        gSynchronousRequest = true;
        gOperationPending = true;
        gRecoveryCondition.notify_one();
        gRecoveryCondition.wait(lock, [] { return gShutdown || gOperationCompleted; });
        if (gShutdown || !gOperationCompleted)
            return UNKNOWN_ERROR;
        const bool succeeded = gOperationSucceeded;
        gOperationCompleted = false;
        gRecoveryCondition.notify_all();
        return succeeded ? SO_NO_ERROR : UNKNOWN_ERROR;
    }

    result setMiniaudioLatencyConfig(unsigned int periodSizeInFrames, unsigned int periods)
    {
        if ((periodSizeInFrames == 0) != (periods == 0))
            return INVALID_PARAMETER;
        std::lock_guard<std::mutex> lock(gRecoveryMutex);
        if (gDeviceActive || gBackendInitializing || gOperationPending || gOperationRunning)
            return INVALID_PARAMETER;
        gPeriodSizeInFrames = periodSizeInFrames;
        gPeriods = periods;
        return SO_NO_ERROR;
    }

    result getMiniaudioLatencyInfo(MiniaudioLatencyInfo &info)
    {
        std::lock_guard<std::mutex> lock(gRecoveryMutex);
        if (!gDeviceActive || gRecovering)
            return INVALID_PARAMETER;
        const unsigned int sampleRate = gDevice.playback.internalSampleRate;
        const unsigned int periodSizeInFrames = gDevice.playback.internalPeriodSizeInFrames;
        const unsigned int periods = gDevice.playback.internalPeriods;
        if (sampleRate == 0)
            return UNKNOWN_ERROR;
        info.periodSizeInFrames = periodSizeInFrames;
        info.periods = periods;
        info.sampleRate = sampleRate;
        info.estimatedMilliseconds = static_cast<double>(periodSizeInFrames) * periods * 1000.0 / sampleRate;
        return SO_NO_ERROR;
    }

    void soloud_miniaudio_audiomixer(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
    {
        SoLoud::Soloud *soloud = (SoLoud::Soloud *)pDevice->pUserData;
            soloud->mix((float *)pOutput, frameCount);
    }

    static void soloud_miniaudio_deinit(SoLoud::Soloud *aSoloud)
    {
        const bool calledFromWorker = gRecoveryThread.joinable() &&
            gRecoveryThread.get_id() == std::this_thread::get_id();
        {
            std::lock_guard<std::mutex> lock(gRecoveryMutex);
            gShutdown = true;
            gRecoveryEnabled = false;
            gBackendInitializing = false;
            gDeviceActive = false;
            gRecoveryCondition.notify_all();
        }
        if (calledFromWorker)
        {
            if (gDeviceInitialized)
                ma_device_stop(&gDevice);
            gRecoveryThread.detach();
            return;
        }
        if (gRecoveryThread.joinable())
            gRecoveryThread.join();
        {
            std::lock_guard<std::mutex> lock(gRecoveryMutex);
            gSoloud = nullptr;
            gShutdown = false;
            gOperationPending = false;
            gOperationRunning = false;
            gOperationCompleted = false;
            gRecovering = false;
            gDeviceActive = false;
        }
    }

    result miniaudio_init(SoLoud::Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer, unsigned int aChannels)
    {
        gSamplerate = aSamplerate;
        gChannels = aChannels;
        {
            std::unique_lock<std::mutex> lock(gRecoveryMutex);
            if (gBackendInitializing || gRecoveryEnabled)
            {
                lock.unlock();
                return INVALID_PARAMETER;
            }
            if (!gWorkerExited && gWorkerThreadId == std::this_thread::get_id())
            {
                lock.unlock();
                return INVALID_PARAMETER;
            }
            gRecoveryCondition.wait(lock, [] { return gWorkerExited; });
            gSoloud = aSoloud;
            gRecoveryStatus = MiniaudioRecoveryStatus::Idle;
            gOperationPending = false;
            gOperationRunning = false;
            gOperationCompleted = false;
            gRecovering = false;
            gShutdown = false;
            gDeviceActive = false;
            gRecoveryEnabled = false;
            gBackendInitializing = true;
            gStartupInitialized = false;
            gStartupReady = false;
            gStartupCompleted = false;
            gStartupSucceeded = false;
            gWorkerExited = false;
            gWorkerThreadId = std::thread::id();
            gStartupHasPreferredDevice = gHasPreferredDevice;
            if (gStartupHasPreferredDevice)
                gStartupPreferredDevice = gPreferredDevice;
        }
        try
        {
            gRecoveryThread = std::thread(runDeviceWorker);
        }
        catch (const std::system_error &)
        {
            std::lock_guard<std::mutex> lock(gRecoveryMutex);
            gBackendInitializing = false;
            gWorkerExited = true;
            gRecoveryCondition.notify_all();
            gSoloud = nullptr;
            return UNKNOWN_ERROR;
        }

        std::unique_lock<std::mutex> lock(gRecoveryMutex);
        gRecoveryCondition.wait(lock, [] { return gStartupInitialized || gStartupCompleted; });
        if (gStartupCompleted && !gStartupInitialized)
        {
            lock.unlock();
            gRecoveryThread.join();
            lock.lock();
            gBackendInitializing = false;
            gSoloud = nullptr;
            lock.unlock();
            return UNKNOWN_ERROR;
        }
        lock.unlock();
        aSoloud->postinit_internal(gDevice.sampleRate, gDevice.playback.internalPeriodSizeInFrames,
                                   aFlags, gDevice.playback.channels);
        lock.lock();
        gStartupReady = true;
        gRecoveryCondition.notify_all();
        gRecoveryCondition.wait(lock, [] { return gStartupCompleted; });
        const bool succeeded = gStartupSucceeded;
        if (!succeeded)
        {
            lock.unlock();
            gRecoveryThread.join();
            lock.lock();
            gBackendInitializing = false;
            gSoloud = nullptr;
            lock.unlock();
            aSoloud->deinit();
            return UNKNOWN_ERROR;
        }
        gBackendInitializing = false;
        lock.unlock();
        aSoloud->mBackendCleanupFunc = soloud_miniaudio_deinit;
        aSoloud->mBackendString = "MiniAudio";
        return 0;
    }
};
#endif
