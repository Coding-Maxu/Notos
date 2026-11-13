// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif

#include "yuzu/notos/notos_sound.h"

#include <algorithm>
#include <cstdint>
#include <list>
#include <map>
#include <vector>

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>

#include "common/logging.h"

namespace Notos {

#ifdef _WIN32

namespace {

struct Clip {
    WORD channels = 0;
    DWORD rate = 0;
    std::vector<int16_t> pcm;
    qint64 last_played = -1000;
};

template <typename T>
void Release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

/// The whole file as 16-bit PCM, through Media Foundation (MP3, WAV, AAC, WMA…).
bool Decode(const QString& path, Clip& clip) {
    IMFSourceReader* reader = nullptr;
    IMFMediaType* want = nullptr;
    IMFMediaType* got = nullptr;
    bool ok = false;
    const std::wstring file = QDir::toNativeSeparators(path).toStdWString();
    if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader))) {
        return false;
    }
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (SUCCEEDED(MFCreateMediaType(&want))) {
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
                                                  want)) &&
            SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got))) {
            clip.channels = static_cast<WORD>(MFGetAttributeUINT32(got, MF_MT_AUDIO_NUM_CHANNELS, 0));
            clip.rate = MFGetAttributeUINT32(got, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
            ok = clip.channels > 0 && clip.rate > 0;
        }
    }
    while (ok) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags,
                                      nullptr, &sample))) {
            ok = !clip.pcm.empty();
            break;
        }
        if (sample) {
            IMFMediaBuffer* buffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
                BYTE* data = nullptr;
                DWORD length = 0;
                if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                    const auto* samples = reinterpret_cast<const int16_t*>(data);
                    clip.pcm.insert(clip.pcm.end(), samples, samples + length / 2);
                    buffer->Unlock();
                }
                Release(buffer);
            }
            Release(sample);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            break;
        }
    }
    Release(got);
    Release(want);
    Release(reader);
    return ok && !clip.pcm.empty();
}

} // namespace

struct Sounds::Impl {
    struct Voice {
        QString name;
        HWAVEOUT out = nullptr;
        WAVEHDR header{};
        std::vector<int16_t> data;
    };

    std::map<QString, Clip> clips;
    std::list<Voice> voices;
    bool enabled = true;
    int volume = 70;
    bool started = false;
    QElapsedTimer clock;

    /// Closes the voices that are done (or all of them).
    void Reap(bool all, const QString& name = {}) {
        for (auto it = voices.begin(); it != voices.end();) {
            const bool stop = all || (!name.isEmpty() && it->name == name);
            if (stop) {
                waveOutReset(it->out);
            }
            if (stop || (it->header.dwFlags & WHDR_DONE)) {
                waveOutUnprepareHeader(it->out, &it->header, sizeof(it->header));
                waveOutClose(it->out);
                it = voices.erase(it);
            } else {
                ++it;
            }
        }
    }
};

Sounds::Sounds() : impl{std::make_unique<Impl>()} {
    impl->started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    impl->clock.start();
}

Sounds::~Sounds() {
    impl->Reap(true);
    if (impl->started) {
        MFShutdown();
    }
}

void Sounds::Load(const QString& dir) {
    if (!impl->started) {
        return;
    }
    const QFileInfoList files = QDir(dir).entryInfoList(QDir::Files, QDir::Name);
    for (const QFileInfo& file : files) {
        const QString name = file.completeBaseName().toLower();
        if (impl->clips.contains(name)) {
            continue;
        }
        Clip clip;
        if (Decode(file.absoluteFilePath(), clip)) {
            impl->clips.emplace(name, std::move(clip));
        } else {
            LOG_WARNING(Frontend, "Notos: can't decode sound {}",
                        file.absoluteFilePath().toStdString());
        }
    }
}

void Sounds::Play(const QString& name) {
    impl->Reap(false);
    if (!impl->enabled || impl->volume <= 0) {
        return;
    }
    const auto found = impl->clips.find(name);
    if (found == impl->clips.end()) {
        return;
    }
    Clip& clip = found->second;
    // One action can pass through several places that sound (a menu closing into another).
    const qint64 now = impl->clock.elapsed();
    if (now - clip.last_played < 60) {
        return;
    }
    clip.last_played = now;

    Impl::Voice& voice = impl->voices.emplace_back();
    voice.name = name;
    voice.data.resize(clip.pcm.size());
    // Perceived loudness: the slider's square.
    const double gain = (impl->volume / 100.0) * (impl->volume / 100.0);
    std::transform(clip.pcm.begin(), clip.pcm.end(), voice.data.begin(),
                   [gain](int16_t s) { return static_cast<int16_t>(s * gain); });
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = clip.channels;
    format.nSamplesPerSec = clip.rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(clip.channels * 2);
    format.nAvgBytesPerSec = clip.rate * format.nBlockAlign;
    if (waveOutOpen(&voice.out, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        impl->voices.pop_back();
        return;
    }
    voice.header.lpData = reinterpret_cast<LPSTR>(voice.data.data());
    voice.header.dwBufferLength = static_cast<DWORD>(voice.data.size() * 2);
    if (waveOutPrepareHeader(voice.out, &voice.header, sizeof(voice.header)) != MMSYSERR_NOERROR ||
        waveOutWrite(voice.out, &voice.header, sizeof(voice.header)) != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(voice.out, &voice.header, sizeof(voice.header));
        waveOutClose(voice.out);
        impl->voices.pop_back();
    }
}

void Sounds::Stop(const QString& name) {
    impl->Reap(false, name);
}

void Sounds::StopAll() {
    impl->Reap(true);
}

void Sounds::SetEnabled(bool enabled) {
    impl->enabled = enabled;
    if (!enabled) {
        impl->Reap(true);
    }
}

void Sounds::SetVolume(int percent) {
    impl->volume = std::clamp(percent, 0, 100);
}

#else

struct Sounds::Impl {};
Sounds::Sounds() = default;
Sounds::~Sounds() = default;
void Sounds::Load(const QString&) {}
void Sounds::Play(const QString&) {}
void Sounds::Stop(const QString&) {}
void Sounds::StopAll() {}
void Sounds::SetEnabled(bool) {}
void Sounds::SetVolume(int) {}

#endif

} // namespace Notos
