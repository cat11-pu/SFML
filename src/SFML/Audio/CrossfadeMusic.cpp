////////////////////////////////////////////////////////////
// Headers
////////////////////////////////////////////////////////////
#include <SFML/Audio/CrossfadeMusic.hpp>
#include <SFML/Audio/InputSoundFile.hpp>

#include <SFML/System/Err.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <ostream>
#include <utility>
#include <vector>

#include <cstring>


namespace
{
constexpr std::size_t chunkFrameCount = 512;

std::uint64_t getDurationFrames(const sf::InputSoundFile& file)
{
    if (file.getChannelCount() == 0)
        return 0;

    return file.getSampleCount() / file.getChannelCount();
}

std::uint64_t timeToFrames(sf::Time time, unsigned int sampleRate)
{
    const auto microseconds = time.asMicroseconds();

    if (microseconds <= 0 || sampleRate == 0)
        return 0;

    const auto secondsPart = static_cast<std::uint64_t>(microseconds) / 1'000'000;
    const auto remainder   = static_cast<std::uint64_t>(microseconds) % 1'000'000;

    return secondsPart * sampleRate + (remainder * sampleRate + 500'000) / 1'000'000;
}

sf::Time framesToTime(std::uint64_t frames, unsigned int sampleRate)
{
    if (sampleRate == 0)
        return sf::Time::Zero;

    const auto secondsPart  = frames / sampleRate;
    const auto remainder    = frames % sampleRate;
    const auto microseconds = secondsPart * 1'000'000 +
                              (remainder * 1'000'000 + sampleRate / 2) / sampleRate;

    return sf::microseconds(static_cast<std::int64_t>(microseconds));
}

std::pair<float, float> getGains(bool fading,
                                 std::uint64_t fadeElapsed,
                                 std::uint64_t fadeFrameCount,
                                 std::uint64_t frameOffset)
{
    if (!fading || fadeFrameCount == 0)
        return {0.f, 1.f};

    const auto elapsed = fadeElapsed + frameOffset;

    if (elapsed >= fadeFrameCount)
        return {0.f, 1.f};

    const auto nextGain = static_cast<float>(elapsed) / static_cast<float>(fadeFrameCount);
    return {1.f - nextGain, nextGain};
}
} // namespace


namespace sf
{
struct CrossfadeMusic::Impl
{
    struct Track
    {
        std::unique_ptr<InputSoundFile> file;
        std::vector<std::int16_t>       samples;
        std::uint64_t                   positionFrames{};
        float                           gain{};
        bool                            open{};
    };

    [[nodiscard]] std::uint64_t getDurationFrames(const Track& track) const
    {
        return track.file ? ::getDurationFrames(*track.file) : 0;
    }

    [[nodiscard]] TrackInfo getTrackInfo(const Track& track) const
    {
        return TrackInfo{track.open, getDurationFrames(track), track.positionFrames, track.gain};
    }

    void seekTrack(Track& track, std::uint64_t frameOffset)
    {
        if (!track.file || channelCount == 0)
            return;

        const auto clampedFrame = std::min(frameOffset, getDurationFrames(track));
        track.file->seek(clampedFrame * channelCount);
        track.positionFrames = track.file->getSampleOffset() / channelCount;
    }

    std::size_t readFrames(Track& track, std::size_t frames)
    {
        if (!track.file || !track.open)
            return 0;

        const auto samplesRead = track.file->read(track.samples.data(), frames * channelCount);
        const auto framesRead  = samplesRead / channelCount;

        track.positionFrames += framesRead;
        return static_cast<std::size_t>(framesRead);
    }

    void updateUnderrun(std::size_t expectedFrames,
                        std::size_t currentFrames,
                        std::size_t nextFrames,
                        bool         nextActive)
    {
        const auto currentMissing = expectedFrames - currentFrames;
        const auto nextMissing    = nextActive ? expectedFrames - nextFrames : 0;

        auto flags = static_cast<unsigned int>(underrun);
        if (currentMissing != 0)
            flags |= static_cast<unsigned int>(Underrun::Current);
        if (nextMissing != 0)
            flags |= static_cast<unsigned int>(Underrun::Next);

        underrun = static_cast<Underrun>(flags);

        statistics.underrunFrames += currentMissing;
        statistics.underrunFrames += nextMissing;
    }

    void completeFadeIfReady()
    {
        if (!fading || fadeElapsed < fadeFrameCount)
            return;

        current = std::move(next);
        next    = Track{};

        current.gain   = 1.f;
        fading         = false;
        fadeElapsed    = 0;
        fadeFrameCount = 0;
    }

    mutable std::mutex        mutex;
    Track                     current;
    Track                     next;
    std::vector<std::int16_t> mixedSamples;
    unsigned int              channelCount{};
    unsigned int              sampleRate{};
    std::uint64_t             fadeElapsed{};
    std::uint64_t             fadeFrameCount{};
    bool                      fading{};
    Underrun                  underrun{Underrun::None};
    Statistics                statistics;
};


////////////////////////////////////////////////////////////
CrossfadeMusic::CrossfadeMusic() : m_impl(std::make_unique<Impl>())
{
}


////////////////////////////////////////////////////////////
CrossfadeMusic::~CrossfadeMusic()
{
    if (m_impl != nullptr)
        stop();
}


////////////////////////////////////////////////////////////
CrossfadeMusic::CrossfadeMusic(CrossfadeMusic&&) noexcept = default;


////////////////////////////////////////////////////////////
CrossfadeMusic& CrossfadeMusic::operator=(CrossfadeMusic&&) noexcept = default;


////////////////////////////////////////////////////////////
bool CrossfadeMusic::openCurrent(std::unique_ptr<InputSoundFile> file)
{
    if (!file || file->getChannelCount() == 0 || file->getSampleRate() == 0)
    {
        err() << "Failed to open current music track: invalid audio format" << std::endl;
        return false;
    }

    const auto channelCount = file->getChannelCount();
    const auto sampleRate   = file->getSampleRate();
    const auto channelMap   = file->getChannelMap();

    stop();

    {
        const std::lock_guard lock(m_impl->mutex);

        m_impl->current = {};
        m_impl->next    = {};

        m_impl->current.file           = std::move(file);
        m_impl->current.samples.assign(chunkFrameCount * channelCount, 0);
        m_impl->current.positionFrames = 0;
        m_impl->current.gain           = 1.f;
        m_impl->current.open           = true;

        m_impl->mixedSamples.assign(chunkFrameCount * channelCount, 0);
        m_impl->channelCount    = channelCount;
        m_impl->sampleRate      = sampleRate;
        m_impl->fadeElapsed     = 0;
        m_impl->fadeFrameCount  = 0;
        m_impl->fading          = false;
        m_impl->underrun        = Underrun::None;
        m_impl->statistics      = {};
    }

    SoundStream::initialize(channelCount, sampleRate, channelMap);
    return true;
}


////////////////////////////////////////////////////////////
bool CrossfadeMusic::openNext(std::unique_ptr<InputSoundFile> file)
{
    if (!file || file->getChannelCount() == 0 || file->getSampleRate() == 0)
    {
        err() << "Failed to open next music track: invalid audio format" << std::endl;
        return false;
    }

    const std::lock_guard lock(m_impl->mutex);

    if (!m_impl->current.open || !m_impl->current.file)
    {
        err() << "Failed to open next music track: no current track is open" << std::endl;
        return false;
    }

    if (m_impl->fading)
    {
        err() << "Failed to open next music track: a crossfade is already active" << std::endl;
        return false;
    }

    if (file->getChannelCount() != m_impl->channelCount || file->getSampleRate() != m_impl->sampleRate ||
        file->getChannelMap() != m_impl->current.file->getChannelMap())
    {
        err() << "Failed to open next music track: audio format does not match the current track" << std::endl;
        return false;
    }

    m_impl->next = {};
    m_impl->next.file           = std::move(file);
    m_impl->next.samples.assign(chunkFrameCount * m_impl->channelCount, 0);
    m_impl->next.positionFrames = 0;
    m_impl->next.gain           = 0.f;
    m_impl->next.open           = true;

    return true;
}


////////////////////////////////////////////////////////////
bool CrossfadeMusic::openFromFile(const std::filesystem::path& filename)
{
    auto file = std::make_unique<InputSoundFile>();
    if (!file->openFromFile(filename))
        return false;

    return openCurrent(std::move(file));
}


////////////////////////////////////////////////////////////
bool CrossfadeMusic::openFromMemory(const void* data, std::size_t sizeInBytes)
{
    auto file = std::make_unique<InputSoundFile>();
    if (!file->openFromMemory(data, sizeInBytes))
        return false;

    return openCurrent(std::move(file));
}


////////////////////////////////////////////////////////////
bool CrossfadeMusic::openFromStream(InputStream& stream)
{
    auto file = std::make_unique<InputSoundFile>();
    if (!file->openFromStream(stream))
        return false;

    return openCurrent(std::move(file));
}


////////////////////////////////////////////////////////////
bool CrossfadeMusic::openNextFromFile(const std::filesystem::path& filename)
{
    auto file = std::make_unique<InputSoundFile>();
    if (!file->openFromFile(filename))
        return false;

    return openNext(std::move(file));
}


////////////////////////////////////////////////////////////
bool CrossfadeMusic::openNextFromMemory(const void* data, std::size_t sizeInBytes)
{
    auto file = std::make_unique<InputSoundFile>();
    if (!file->openFromMemory(data, sizeInBytes))
        return false;

    return openNext(std::move(file));
}


////////////////////////////////////////////////////////////
bool CrossfadeMusic::openNextFromStream(InputStream& stream)
{
    auto file = std::make_unique<InputSoundFile>();
    if (!file->openFromStream(stream))
        return false;

    return openNext(std::move(file));
}


////////////////////////////////////////////////////////////
bool CrossfadeMusic::crossfadeToNext(Time duration)
{
    std::uint64_t durationFrames = 0;

    {
        const std::lock_guard lock(m_impl->mutex);
        durationFrames = timeToFrames(duration, m_impl->sampleRate);
    }

    return crossfadeToNextFrames(durationFrames);
}


////////////////////////////////////////////////////////////
bool CrossfadeMusic::crossfadeToNextFrames(std::uint64_t durationFrames)
{
    const std::lock_guard lock(m_impl->mutex);

    if (!m_impl->current.open || !m_impl->next.open)
    {
        err() << "Failed to start crossfade: current and next tracks must both be open" << std::endl;
        return false;
    }

    if (m_impl->fading)
    {
        err() << "Failed to start crossfade: a crossfade is already active" << std::endl;
        return false;
    }

    m_impl->fading         = true;
    m_impl->fadeElapsed    = 0;
    m_impl->fadeFrameCount = durationFrames;
    ++m_impl->statistics.crossfades;

    return true;
}


////////////////////////////////////////////////////////////
void CrossfadeMusic::setPlayingOffset(Time timeOffset)
{
    std::uint64_t frameOffset = 0;

    {
        const std::lock_guard lock(m_impl->mutex);
        frameOffset = timeToFrames(timeOffset, m_impl->sampleRate);
    }

    seekToFrame(frameOffset);
}


////////////////////////////////////////////////////////////
void CrossfadeMusic::seekToFrame(std::uint64_t frameOffset)
{
    std::uint64_t clampedFrame = 0;

    {
        const std::lock_guard lock(m_impl->mutex);

        if (!m_impl->current.open)
            return;

        clampedFrame = std::min(frameOffset, m_impl->getDurationFrames(m_impl->current));
    }

    SoundStream::setPlayingOffsetFrames(clampedFrame);
}


////////////////////////////////////////////////////////////
void CrossfadeMusic::seekNextToFrame(std::uint64_t frameOffset)
{
    const std::lock_guard lock(m_impl->mutex);

    if (!m_impl->next.open)
        return;

    m_impl->seekTrack(m_impl->next, frameOffset);
    ++m_impl->statistics.seeks;
}


////////////////////////////////////////////////////////////
std::uint64_t CrossfadeMusic::getFrame() const
{
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->current.positionFrames;
}


////////////////////////////////////////////////////////////
Time CrossfadeMusic::getDuration() const
{
    const std::lock_guard lock(m_impl->mutex);
    return framesToTime(m_impl->getDurationFrames(m_impl->current), m_impl->sampleRate);
}


////////////////////////////////////////////////////////////
std::uint64_t CrossfadeMusic::getDurationFrames() const
{
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->getDurationFrames(m_impl->current);
}


////////////////////////////////////////////////////////////
CrossfadeMusic::Underrun CrossfadeMusic::getUnderrun() const
{
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->underrun;
}


////////////////////////////////////////////////////////////
bool CrossfadeMusic::hasUnderrun() const
{
    return getUnderrun() != Underrun::None;
}


////////////////////////////////////////////////////////////
void CrossfadeMusic::clearUnderrun()
{
    const std::lock_guard lock(m_impl->mutex);
    m_impl->underrun = Underrun::None;
}


////////////////////////////////////////////////////////////
CrossfadeMusic::TrackInfo CrossfadeMusic::getCurrentTrack() const
{
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->getTrackInfo(m_impl->current);
}


////////////////////////////////////////////////////////////
CrossfadeMusic::TrackInfo CrossfadeMusic::getNextTrack() const
{
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->getTrackInfo(m_impl->next);
}


////////////////////////////////////////////////////////////
CrossfadeMusic::Statistics CrossfadeMusic::getStatistics() const
{
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->statistics;
}


////////////////////////////////////////////////////////////
bool CrossfadeMusic::onGetData(Chunk& data)
{
    const std::lock_guard lock(m_impl->mutex);

    data = {};

    if (!m_impl->current.open)
        return false;

    const auto currentFrames = m_impl->readFrames(m_impl->current, chunkFrameCount);
    const auto nextFrames    = m_impl->fading ? m_impl->readFrames(m_impl->next, chunkFrameCount) : 0;

    m_impl->updateUnderrun(chunkFrameCount, currentFrames, nextFrames, m_impl->fading);

    std::size_t framesToRender = currentFrames;
    if (m_impl->fading)
        framesToRender = std::max(framesToRender, nextFrames);

    if (framesToRender == 0)
    {
        m_impl->completeFadeIfReady();
        return false;
    }

    if (!m_impl->fading)
    {
        const auto sampleCount = framesToRender * m_impl->channelCount;
        std::memcpy(m_impl->mixedSamples.data(), m_impl->current.samples.data(), sampleCount * sizeof(std::int16_t));
        m_impl->current.gain = 1.f;
    }
    else
    {
        float currentGain = 0.f;
        float nextGain    = 0.f;

        for (std::size_t frame = 0; frame < framesToRender; ++frame)
        {
            const auto [frameCurrentGain, frameNextGain] = getGains(m_impl->fading,
                                                                    m_impl->fadeElapsed,
                                                                    m_impl->fadeFrameCount,
                                                                    frame);

            currentGain = frameCurrentGain;
            nextGain    = frameNextGain;

            for (unsigned int channel = 0; channel < m_impl->channelCount; ++channel)
            {
                const auto sampleIndex   = frame * m_impl->channelCount + channel;
                const auto currentSample = frame < currentFrames ? m_impl->current.samples[sampleIndex] : 0;
                const auto nextSample    = frame < nextFrames ? m_impl->next.samples[sampleIndex] : 0;

                const auto mixedSample = std::lround(static_cast<float>(currentSample) * currentGain +
                                                     static_cast<float>(nextSample) * nextGain);

                m_impl->mixedSamples[sampleIndex] = static_cast<std::int16_t>(
                    std::clamp<long>(mixedSample, -32768, 32767));
            }
        }

        m_impl->current.gain = currentGain;
        m_impl->next.gain    = nextGain;
    }

    data.samples     = m_impl->mixedSamples.data();
    data.sampleCount = framesToRender * m_impl->channelCount;

    m_impl->fadeElapsed += framesToRender;
    m_impl->statistics.framesRendered += framesToRender;

    m_impl->completeFadeIfReady();

    return true;
}


////////////////////////////////////////////////////////////
void CrossfadeMusic::onSeek(Time timeOffset)
{
    const std::lock_guard lock(m_impl->mutex);

    if (!m_impl->current.open)
        return;

    m_impl->seekTrack(m_impl->current, timeToFrames(timeOffset, m_impl->sampleRate));
    ++m_impl->statistics.seeks;
}


////////////////////////////////////////////////////////////
void CrossfadeMusic::onSeekToFrame(std::uint64_t frameOffset)
{
    const std::lock_guard lock(m_impl->mutex);

    if (!m_impl->current.open)
        return;

    m_impl->seekTrack(m_impl->current, frameOffset);
    ++m_impl->statistics.seeks;
}
} // namespace sf
