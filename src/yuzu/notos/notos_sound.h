// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include <QString>

namespace Notos {

/// Notos' own sounds: short clips in a folder, by name (`move.mp3`, `startup.wav`…, any format
/// Windows decodes), played on top of each other at the chosen volume.
class Sounds {
public:
    Sounds();
    ~Sounds();
    Sounds(const Sounds&) = delete;
    Sounds& operator=(const Sounds&) = delete;

    /// Decodes the clips found in `dir` (missing ones stay silent).
    void Load(const QString& dir);
    void Play(const QString& name);
    void Stop(const QString& name);
    void StopAll();
    void SetEnabled(bool enabled);
    void SetVolume(int percent); ///< 0..100

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Notos
