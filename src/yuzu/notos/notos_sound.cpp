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
#include <map>
#include <vector>

#include <QDir>
#include <QFileInfo>

#include "common/logging.h"

namespace Notos {

#ifdef _WIN32

namespace {

struct Clip {
    WORD channels = 0;
    DWORD rate = 0;
    std::vector<int16_t> pcm;
    // Each clip keeps its own output open: a press starts it at once, and again restarts it.
    HWAVEOUT out = nullptr;
    WAVEHDR header{};
    std::vector<int16_t> data; ///< pcm at the volume
    bool queued = false;
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
    std::map<QString, Clip> clips;
    bool enabled = true;
    int volume = 70;
    bool started = false;

    static void Halt(Clip& clip) {
        if (clip.out && clip.queued) {
            waveOutReset(clip.out);
            clip.queued = false;
        }
    }

    void Scale(Clip& clip) const {
        // Perceived loudness: the slider's square.
        const double gain = (volume / 100.0) * (volume / 100.0);
        std::transform(clip.pcm.begin(), clip.pcm.end(), clip.data.begin(),
                       [gain](int16_t s) { return static_cast<int16_t>(s * gain); });
    }

    bool Open(Clip& clip) {
        // A clip a few frames long can be lost while the output starts: some silence after it.
        const size_t min_samples = static_cast<size_t>(clip.rate) * clip.channels / 8;
        if (clip.pcm.size() < min_samples) {
            clip.pcm.resize(min_samples, 0);
        }
        clip.data.resize(clip.pcm.size());
        Scale(clip);
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = clip.channels;
        format.nSamplesPerSec = clip.rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(clip.channels * 2);
        format.nAvgBytesPerSec = clip.rate * format.nBlockAlign;
        if (waveOutOpen(&clip.out, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) !=
            MMSYSERR_NOERROR) {
            clip.out = nullptr;
            return false;
        }
        clip.header.lpData = reinterpret_cast<LPSTR>(clip.data.data());
        clip.header.dwBufferLength = static_cast<DWORD>(clip.data.size() * 2);
        if (waveOutPrepareHeader(clip.out, &clip.header, sizeof(clip.header)) !=
            MMSYSERR_NOERROR) {
            waveOutClose(clip.out);
            clip.out = nullptr;
            return false;
        }
        return true;
    }

    void Close(Clip& clip) {
        if (!clip.out) {
            return;
        }
        Halt(clip);
        waveOutUnprepareHeader(clip.out, &clip.header, sizeof(clip.header));
        waveOutClose(clip.out);
        clip.out = nullptr;
    }
};

Sounds::Sounds() : impl{std::make_unique<Impl>()} {
    impl->started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
}

Sounds::~Sounds() {
    for (auto& [name, clip] : impl->clips) {
        impl->Close(clip);
    }
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
        if (!Decode(file.absoluteFilePath(), clip)) {
            LOG_WARNING(Frontend, "Notos: can't decode sound {}",
                        file.absoluteFilePath().toStdString());
            continue;
        }
        LOG_INFO(Frontend, "Notos: sound {} ({} ms, {} Hz, {} ch)", name.toStdString(),
                 clip.pcm.size() * 1000 / (static_cast<size_t>(clip.rate) * clip.channels),
                 clip.rate, clip.channels);
        Clip& kept = impl->clips.emplace(name, std::move(clip)).first->second;
        if (!impl->Open(kept)) {
            LOG_WARNING(Frontend, "Notos: can't open an output for sound {}", name.toStdString());
        }
    }
}

void Sounds::Play(const QString& name) {
    if (!impl->enabled || impl->volume <= 0) {
        return;
    }
    const auto found = impl->clips.find(name);
    if (found == impl->clips.end() || !found->second.out) {
        return;
    }
    Clip& clip = found->second;
    Impl::Halt(clip); // playing already: from the start
    if (waveOutWrite(clip.out, &clip.header, sizeof(clip.header)) == MMSYSERR_NOERROR) {
        clip.queued = true;
    }
}

void Sounds::Stop(const QString& name) {
    const auto found = impl->clips.find(name);
    if (found != impl->clips.end()) {
        Impl::Halt(found->second);
    }
}

void Sounds::StopAll() {
    for (auto& [name, clip] : impl->clips) {
        Impl::Halt(clip);
    }
}

void Sounds::SetEnabled(bool enabled) {
    impl->enabled = enabled;
    if (!enabled) {
        StopAll();
    }
}

void Sounds::SetVolume(int percent) {
    impl->volume = std::clamp(percent, 0, 100);
    for (auto& [name, clip] : impl->clips) {
        Impl::Halt(clip);
        impl->Scale(clip);
    }
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
