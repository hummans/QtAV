/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2012-2016 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV 2016

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include "QtAV/private/AudioOutputBackend.h"
#include <QtCore/QQueue>
#include <QtCore/QSemaphore>
#include <QtCore/QThread>
#include <AudioToolbox/AudioQueue.h>
#include <AudioToolbox/AudioToolbox.h>
#include "QtAV/private/mkid.h"
#include "QtAV/private/factory.h"
#include "utils/Logger.h"

// TODO: ao.pause to avoid continue playing
namespace QtAV {

static const char kName[] = "AudioToolbox";
class AudioOutputAudioToolbox Q_DECL_FINAL: public AudioOutputBackend
{
public:
    AudioOutputAudioToolbox(QObject *parent = 0);
    ~AudioOutputAudioToolbox();

    QString name() const Q_DECL_OVERRIDE { return QLatin1String(kName);}
    bool open() Q_DECL_OVERRIDE;
    bool close() Q_DECL_OVERRIDE;
    //bool flush() Q_DECL_OVERRIDE;
    BufferControl bufferControl() const Q_DECL_OVERRIDE;
    void onCallback() Q_DECL_OVERRIDE;
    bool write(const QByteArray& data) Q_DECL_OVERRIDE;
    bool play() Q_DECL_OVERRIDE;

    static void outCallback(void* inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer);
private:
    QVector<AudioQueueBufferRef> m_buffer;
    QVector<AudioQueueBufferRef> m_buffer_fill;
    QMutex m_mutex;
    AudioQueueRef m_queue;
    AudioStreamBasicDescription m_desc;

    QSemaphore sem;
};

typedef AudioOutputAudioToolbox AudioOutputBackendAudioToolbox;
static const AudioOutputBackendId AudioOutputBackendId_AudioToolbox = mkid::id32base36_2<'A', 'T'>::value;
FACTORY_REGISTER(AudioOutputBackend, AudioToolbox, kName)

#define AT_ENSURE(FUNC, ...) \
    do { \
        OSStatus ret = FUNC; \
        if (ret) { \
            qWarning("AudioOutputAudioToolbox Error>>> " #FUNC " (%d)", ret); \
            return __VA_ARGS__; \
        } \
    } while(0)

static AudioStreamBasicDescription audioFormatToAT(const AudioFormat &format)
{
    AudioStreamBasicDescription desc;
    desc.mSampleRate = format.sampleRate();
    desc.mFormatID = kAudioFormatLinearPCM;
    desc.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked; // TODO: float, endian
    // FIXME: unsigned int
    if (format.isFloat())
        desc.mFormatFlags |= kAudioFormatFlagIsFloat;
    else if (!format.isUnsigned())
        desc.mFormatFlags |= kAudioFormatFlagIsSignedInteger;
    desc.mFramesPerPacket = 1;// FIXME:??
    desc.mChannelsPerFrame = format.channels();
    desc.mBitsPerChannel = format.bytesPerSample()*8;
    desc.mBytesPerFrame = format.bytesPerFrame();
    desc.mBytesPerPacket = desc.mBytesPerFrame * desc.mFramesPerPacket;

    return desc;
}

void AudioOutputAudioToolbox::outCallback(void* inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer)
{
    Q_UNUSED(inAQ);
    AudioOutputAudioToolbox *ao = reinterpret_cast<AudioOutputAudioToolbox*>(inUserData);
    if (ao->bufferControl() & AudioOutputBackend::CountCallback) {
        ao->onCallback();
    }
    QMutexLocker locker(&ao->m_mutex);
    Q_UNUSED(locker);
    ao->m_buffer_fill.push_back(inBuffer);
}

AudioOutputAudioToolbox::AudioOutputAudioToolbox(QObject *parent)
    : AudioOutputBackend(AudioOutput::DeviceFeatures()
                         , parent)
{
    available = false;

    available = true;
}

AudioOutputAudioToolbox::~AudioOutputAudioToolbox()
{
}

AudioOutputBackend::BufferControl AudioOutputAudioToolbox::bufferControl() const
{
    return CountCallback;//BufferControl(Callback | PlayedCount);
}

void AudioOutputAudioToolbox::onCallback()
{
    if (bufferControl() & CountCallback)
        sem.release();
    //qDebug("callback. sem: %d", sem.available());
}

bool AudioOutputAudioToolbox::open()
{
    m_buffer.resize(buffer_count);
    m_desc = audioFormatToAT(format);
    AT_ENSURE(AudioQueueNewOutput(&m_desc, AudioOutputAudioToolbox::outCallback, this, NULL, kCFRunLoopCommonModes/*NULL*/, 0, &m_queue), false);
    for (int i = 0; i < m_buffer.size(); ++i) {
        AT_ENSURE(AudioQueueAllocateBuffer(m_queue, buffer_size, &m_buffer[i]), false);
    }
    m_buffer_fill = m_buffer;

    sem.release(buffer_count - sem.available());
    return true;
}

bool AudioOutputAudioToolbox::close()
{
    AT_ENSURE(AudioQueueStop(m_queue, true), false);
    foreach (AudioQueueBufferRef buf, m_buffer) {
        AT_ENSURE(AudioQueueFreeBuffer(m_queue, buf), false);
    }

    return true;
}

bool AudioOutputAudioToolbox::write(const QByteArray& data)
{
    // blocking queue.
    // if queue not full, fill buffer and enqueue buffer
    //qDebug("write. sem: %d", sem.available());
    if (bufferControl() & CountCallback)
        sem.acquire();

    AudioQueueBufferRef buf = NULL;
    {
        QMutexLocker locker(&m_mutex);
        Q_UNUSED(locker);
        // put to data queue, if has buffer to fill (was available in callback), fill the front data
        if (m_buffer_fill.isEmpty()) { //FIXME: the first call may be empty! use blocking queue instead
            qWarning("internal error buffer queue to fill should not be empty");
            return false;
        }
        buf = m_buffer_fill.front();
        m_buffer_fill.pop_front();
    }
    assert(buf->mAudioDataBytesCapacity >= (UInt32)data.size() && "too many data to write to audio queue buffer");
    memcpy(buf->mAudioData, data.constData(), data.size());
    buf->mAudioDataByteSize = data.size();
    //buf->mUserData
    AT_ENSURE(AudioQueueEnqueueBuffer(m_queue, buf, 0, NULL), false);
    return true;
}

bool AudioOutputAudioToolbox::play()
{
    //UInt32 running = 0;
    //AT_ENSURE(AudioQueueGetProperty(m_queue, kAudioQueueProperty_IsRunning, &running, NULL), false);
    AT_ENSURE(AudioQueueStart(m_queue, NULL), false);
    return true;
}
} //namespace QtAV
