/* AudioStreamOutALSA.cpp
 **
 ** Copyright 2008-2009 Wind River Systems
 ** Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>

#define LOG_TAG "AudioStreamOutALSA"
#define LOG_NDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include "AudioHardwareALSA.h"

#ifndef ALSA_DEFAULT_SAMPLE_RATE
#define ALSA_DEFAULT_SAMPLE_RATE 44100 // in Hz
#endif

namespace android
{

// ----------------------------------------------------------------------------

static const int DEFAULT_SAMPLE_RATE = ALSA_DEFAULT_SAMPLE_RATE;

// ----------------------------------------------------------------------------

AudioStreamOutALSA::AudioStreamOutALSA(AudioHardwareALSA *parent, alsa_handle_t *handle) :
    ALSAStreamOps(parent, handle),
    mParent(parent),
    mFrameCount(0)
{
}

AudioStreamOutALSA::~AudioStreamOutALSA()
{
    close();
}

uint32_t AudioStreamOutALSA::channels() const
{
    int c = ALSAStreamOps::channels();
    return c;
}

status_t AudioStreamOutALSA::setVolume(float left, float right)
{
    int lpa_vol;
    float volume;
    status_t status = NO_ERROR;

    if(!strcmp(mHandle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER) ||
       !strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_LPA)) {
        volume = (left + right) / 2;
        if (volume < 0.0) {
            LOGW("AudioSessionOutMSM7xxx::setVolume(%f) under 0.0, assuming 0.0\n", volume);
            volume = 0.0;
        } else if (volume > 1.0) {
            LOGW("AudioSessionOutMSM7xxx::setVolume(%f) over 1.0, assuming 1.0\n", volume);
            volume = 1.0;
        }
        lpa_vol = lrint((volume * 100.0)+0.5);
        LOGV("setLpaVolume(%f)\n", volume);
        LOGV("Setting LPA volume to %d (available range is 0 to 100)\n", lpa_vol);
        mHandle->module->setLpaVolume(lpa_vol);

        return status;
    }
    return INVALID_OPERATION;
}

ssize_t AudioStreamOutALSA::write(const void *buffer, size_t bytes)
{
    char *use_case;
    AutoMutex lock(mLock);

    LOGV("write:: buffer %p, bytes %d", buffer, bytes);
    if (!mPowerLock) {
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioOutLock");
        mPowerLock = true;
    }

    snd_pcm_sframes_t n;
    size_t            sent = 0;
    status_t          err;

    int write_pending = bytes;

    if(mHandle->handle == NULL) {
        snd_use_case_get(mHandle->ucMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strlcpy(mHandle->useCase, SND_USE_CASE_VERB_HIFI, sizeof(mHandle->useCase));
        } else {
            strlcpy(mHandle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC, sizeof(mHandle->useCase));
        }
        free(use_case);
        mHandle->module->route(mHandle, mDevices, mParent->mode(), (mParent->getTtyMode()));
        if (!strcmp(mHandle->useCase, SND_USE_CASE_VERB_HIFI)) {
            snd_use_case_set(mHandle->ucMgr, "_verb", SND_USE_CASE_VERB_HIFI);
        } else {
            snd_use_case_set(mHandle->ucMgr, "_enamod", mHandle->useCase);
        }
        mHandle->module->open(mHandle);
        if(mHandle->handle == NULL) {
            LOGE("write:: device open failed");
            return 0;
        }
    }

    do {
        if (write_pending < mHandle->handle->period_size) {
            write_pending = mHandle->handle->period_size;
        }
        n = pcm_write(mHandle->handle,
                     (char *)buffer + sent,
                      mHandle->handle->period_size);
        if (n == -EBADFD) {
            // Somehow the stream is in a bad state. The driver probably
            // has a bug and snd_pcm_recover() doesn't seem to handle this.
            mHandle->module->open(mHandle);
        }
        else if (n < 0) {
            // Recovery is part of pcm_write. TODO split is later.
            return static_cast<ssize_t>(n);
        }
        else {
            mFrameCount += n;
            sent += static_cast<ssize_t>((mHandle->handle->period_size));
            write_pending -= mHandle->handle->period_size;
        }

    } while (mHandle->handle && sent < bytes);

    return sent;
}

status_t AudioStreamOutALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioStreamOutALSA::open(int mode)
{
    AutoMutex lock(mLock);

    return ALSAStreamOps::open(mode);
}

status_t AudioStreamOutALSA::close()
{
    AutoMutex lock(mLock);

    LOGV("close");
    ALSAStreamOps::close();

    if (mPowerLock) {
        release_wake_lock ("AudioOutLock");
        mPowerLock = false;
    }

    return NO_ERROR;
}

status_t AudioStreamOutALSA::standby()
{
    AutoMutex lock(mLock);

    LOGV("standby");

    mHandle->module->standby(mHandle);

    if (mPowerLock) {
        release_wake_lock ("AudioOutLock");
        mPowerLock = false;
    }

    mFrameCount = 0;

    return NO_ERROR;
}

#define USEC_TO_MSEC(x) ((x + 999) / 1000)

uint32_t AudioStreamOutALSA::latency() const
{
    // Android wants latency in milliseconds.
    return USEC_TO_MSEC (mHandle->latency);
}

// return the number of audio frames written by the audio dsp to DAC since
// the output has exited standby
status_t AudioStreamOutALSA::getRenderPosition(uint32_t *dspFrames)
{
    *dspFrames = mFrameCount;
    return NO_ERROR;
}

}       // namespace android
