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

#if !defined(WITH_MINIAUDIO)

namespace SoLoud
{
	result getMiniaudioPlaybackDevices(std::vector<MiniaudioDevice> &devices) { devices.clear(); return NOT_IMPLEMENTED; }
	result setPreferredMiniaudioDevice(const MiniaudioDevice *) { return NOT_IMPLEMENTED; }
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

namespace SoLoud
{
    ma_device gDevice;
    ma_context gContext;
    bool gContextInitialized = false;
    bool gRecoveryEnabled = false;
    bool gRecoveryPending = false;
    bool gRecovering = false;
    bool gShutdown = false;
    bool gHasPreferredDevice = false;
    ma_device_id gPreferredDevice;
    unsigned int gSamplerate = 0;
    unsigned int gChannels = 0;
    Soloud *gSoloud = nullptr;
    MiniaudioRecoveryStatus gRecoveryStatus = MiniaudioRecoveryStatus::Idle;
    MiniaudioRecoveryCallback gRecoveryCallback = nullptr;
    void *gRecoveryUserData = nullptr;
    std::mutex gRecoveryMutex;
    std::condition_variable gRecoveryCondition;
    std::thread gRecoveryThread;

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
                    devices.push_back({infos[index].id, infos[index].name, infos[index].isDefault != MA_FALSE});
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
        if (gRecoveryEnabled || gRecovering)
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

    static ma_result openDevice(const ma_device_id *deviceId)
    {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = gChannels;
        config.playback.pDeviceID = deviceId;
        config.sampleRate = gSamplerate;
        config.dataCallback = soloud_miniaudio_audiomixer;
        config.notificationCallback = [](const ma_device_notification *notification)
        {
            if (notification->type != ma_device_notification_type_stopped)
                return;
            std::lock_guard<std::mutex> lock(gRecoveryMutex);
            if (gRecoveryEnabled && !gRecovering && !gShutdown)
            {
                gRecoveryPending = true;
                gRecoveryCondition.notify_one();
            }
        };
        config.pUserData = gSoloud;
        if (ma_device_init(&gContext, &config, &gDevice) != MA_SUCCESS)
            return MA_ERROR;
        if (ma_device_start(&gDevice) != MA_SUCCESS)
        {
            ma_device_uninit(&gDevice);
            return MA_ERROR;
        }
        return MA_SUCCESS;
    }

    static void publishRecoveryStatus(MiniaudioRecoveryStatus status)
    {
        MiniaudioRecoveryCallback callback;
        void *userData;
        {
            std::lock_guard<std::mutex> lock(gRecoveryMutex);
            gRecoveryStatus = status;
            callback = gRecoveryCallback;
            userData = gRecoveryUserData;
        }
        if (callback != nullptr)
            callback(status, userData);
    }

    static void recoverDevice()
    {
        std::unique_lock<std::mutex> lock(gRecoveryMutex);
        for (;;)
        {
            gRecoveryCondition.wait(lock, [] { return gShutdown || gRecoveryPending; });
            if (gShutdown)
                return;
            gRecoveryPending = false;
            gRecovering = true;
            const bool hasPreferred = gHasPreferredDevice;
            const ma_device_id preferred = gPreferredDevice;
            lock.unlock();

            publishRecoveryStatus(MiniaudioRecoveryStatus::Recovering);
            ma_device_uninit(&gDevice);
            bool restoredPreferred = hasPreferred && openDevice(&preferred) == MA_SUCCESS;
            bool restoredDefault = !restoredPreferred && openDevice(NULL) == MA_SUCCESS;

            lock.lock();
            gRecovering = false;
            const MiniaudioRecoveryStatus status = restoredPreferred
                ? MiniaudioRecoveryStatus::SelectedDeviceRestored
                : restoredDefault ? MiniaudioRecoveryStatus::DefaultDeviceRestored : MiniaudioRecoveryStatus::Failed;
            lock.unlock();
            publishRecoveryStatus(status);
            lock.lock();
        }
    }

    void soloud_miniaudio_audiomixer(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
    {
        SoLoud::Soloud *soloud = (SoLoud::Soloud *)pDevice->pUserData;
            soloud->mix((float *)pOutput, frameCount);
    }

    static void soloud_miniaudio_deinit(SoLoud::Soloud *aSoloud)
    {
        {
            std::lock_guard<std::mutex> lock(gRecoveryMutex);
            gShutdown = true;
            gRecoveryEnabled = false;
            gRecoveryCondition.notify_one();
        }
        if (gRecoveryThread.joinable())
            gRecoveryThread.join();
        ma_device_uninit(&gDevice);
        if (gContextInitialized)
        {
            ma_context_uninit(&gContext);
            gContextInitialized = false;
        }
        gSoloud = nullptr;
        gShutdown = false;
        gRecoveryPending = false;
        gRecovering = false;
    }

    result miniaudio_init(SoLoud::Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer, unsigned int aChannels)
    {
        gSamplerate = aSamplerate;
        gChannels = aChannels;
        gSoloud = aSoloud;
        gShutdown = false;
        if (ma_context_init(NULL, 0, NULL, &gContext) != MA_SUCCESS)
            return UNKNOWN_ERROR;
        gContextInitialized = true;

        const bool selectedDeviceOpened = gHasPreferredDevice && openDevice(&gPreferredDevice) == MA_SUCCESS;
        if (!selectedDeviceOpened && openDevice(NULL) != MA_SUCCESS)
        {
            ma_context_uninit(&gContext);
            gContextInitialized = false;
            gSoloud = nullptr;
            return UNKNOWN_ERROR;
        }

        aSoloud->postinit_internal(gDevice.sampleRate, gDevice.playback.internalPeriodSizeInFrames, aFlags, gDevice.playback.channels);
        aSoloud->mBackendCleanupFunc = soloud_miniaudio_deinit;
        aSoloud->mBackendString = "MiniAudio";
        {
            std::lock_guard<std::mutex> lock(gRecoveryMutex);
            gRecoveryStatus = MiniaudioRecoveryStatus::Idle;
            gRecoveryPending = false;
            gRecovering = false;
            gShutdown = false;
            gRecoveryEnabled = true;
        }
        try
        {
            gRecoveryThread = std::thread(recoverDevice);
        }
        catch (const std::system_error &)
        {
            {
                std::lock_guard<std::mutex> lock(gRecoveryMutex);
                gRecoveryEnabled = false;
            }
            ma_device_uninit(&gDevice);
            ma_context_uninit(&gContext);
            gContextInitialized = false;
            gSoloud = nullptr;
            aSoloud->mBackendCleanupFunc = nullptr;
            return UNKNOWN_ERROR;
        }
        return 0;
    }
};
#endif
