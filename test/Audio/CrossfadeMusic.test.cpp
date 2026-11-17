#include <SFML/Audio/CrossfadeMusic.hpp>
#include <SFML/Audio/PlaybackDevice.hpp>

#include <catch2/catch_test_macros.hpp>

#include <AudioUtil.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>


namespace
{
void appendU16(std::vector<std::byte>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::byte>(value & 0xFF));
    bytes.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
}

void appendU32(std::vector<std::byte>& bytes, std::uint32_t value)
{
    for (unsigned int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
}

void appendTag(std::vector<std::byte>& bytes, std::string_view tag)
{
    for (const char character : tag)
        bytes.push_back(static_cast<std::byte>(character));
}

std::vector<std::byte> makeWav(unsigned int channels, unsigned int sampleRate, const std::vector<std::int16_t>& samples)
{
    const auto dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));

    std::vector<std::byte> bytes;
    bytes.reserve(44 + dataSize);

    appendTag(bytes, "RIFF");
    appendU32(bytes, 36 + dataSize);
    appendTag(bytes, "WAVE");
    appendTag(bytes, "fmt ");
    appendU32(bytes, 16);
    appendU16(bytes, 1);
    appendU16(bytes, static_cast<std::uint16_t>(channels));
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * channels * sizeof(std::int16_t));
    appendU16(bytes, static_cast<std::uint16_t>(channels * sizeof(std::int16_t)));
    appendU16(bytes, 16);
    appendTag(bytes, "data");
    appendU32(bytes, dataSize);

    for (const auto sample : samples)
        appendU16(bytes, static_cast<std::uint16_t>(sample));

    return bytes;
}

std::vector<std::int16_t> makeRamp(std::uint64_t frames, unsigned int channels, int base)
{
    std::vector<std::int16_t> samples;
    samples.reserve(static_cast<std::size_t>(frames * channels));

    for (std::uint64_t frame = 0; frame < frames; ++frame)
    {
        for (unsigned int channel = 0; channel < channels; ++channel)
            samples.push_back(static_cast<std::int16_t>(base + static_cast<int>(frame * channels + channel)));
    }

    return samples;
}

std::int16_t sampleAt(int base, unsigned int channels, std::uint64_t frame, unsigned int channel)
{
    return static_cast<std::int16_t>(base + static_cast<int>(frame * channels + channel));
}

std::int16_t mixSamples(std::int16_t current, std::int16_t next, float currentGain, float nextGain)
{
    return static_cast<std::int16_t>(
        std::lround(static_cast<float>(current) * currentGain + static_cast<float>(next) * nextGain));
}

class TestCrossfadeMusic : public sf::CrossfadeMusic
{
public:
    using sf::CrossfadeMusic::onGetData;
};

struct PulledChunk
{
    std::vector<std::int16_t> samples;
    bool                      continuing{};
};

PulledChunk pull(TestCrossfadeMusic& stream)
{
    sf::SoundStream::Chunk chunk;
    const bool             continuing = stream.onGetData(chunk);

    PulledChunk pulled;
    pulled.continuing = continuing;

    if (chunk.samples != nullptr && chunk.sampleCount != 0)
        pulled.samples.assign(chunk.samples, chunk.samples + chunk.sampleCount);

    return pulled;
}
} // namespace


TEST_CASE("[Audio] sf::CrossfadeMusic", runAudioDeviceTests())
{
    [[maybe_unused]] auto result = sf::PlaybackDevice::setDeviceToNull();

    constexpr unsigned int channels   = 2;
    constexpr unsigned int sampleRate = 1000;

    SECTION("Type traits")
    {
        STATIC_CHECK(!std::is_copy_constructible_v<sf::CrossfadeMusic>);
        STATIC_CHECK(!std::is_copy_assignable_v<sf::CrossfadeMusic>);
        STATIC_CHECK(std::is_nothrow_move_constructible_v<sf::CrossfadeMusic>);
        STATIC_CHECK(std::is_nothrow_move_assignable_v<sf::CrossfadeMusic>);
        STATIC_CHECK(std::has_virtual_destructor_v<sf::CrossfadeMusic>);
    }

    SECTION("Construction")
    {
        const sf::CrossfadeMusic music;

        CHECK(!music.getCurrentTrack().open);
        CHECK(!music.getNextTrack().open);
        CHECK(music.getFrame() == 0);
        CHECK(music.getDurationFrames() == 0);
        CHECK(music.getDuration() == sf::Time::Zero);
        CHECK(music.getUnderrun() == sf::CrossfadeMusic::Underrun::None);
        CHECK(!music.hasUnderrun());

        const auto statistics = music.getStatistics();
        CHECK(statistics.framesRendered == 0);
        CHECK(statistics.underrunFrames == 0);
        CHECK(statistics.seeks == 0);
        CHECK(statistics.crossfades == 0);
    }

    SECTION("Open validation")
    {
        const auto stereo = makeWav(channels, sampleRate, makeRamp(128, channels, 1000));
        const auto mono   = makeWav(1, sampleRate, makeRamp(128, 1, 1000));

        TestCrossfadeMusic music;

        CHECK(!music.openNextFromMemory(stereo.data(), stereo.size()));
        CHECK(!music.getNextTrack().open);

        REQUIRE(music.openFromMemory(stereo.data(), stereo.size()));
        CHECK(music.getCurrentTrack().open);
        CHECK(music.getCurrentTrack().durationFrames == 128);
        CHECK(music.getCurrentTrack().positionFrames == 0);
        CHECK(music.getDurationFrames() == 128);
        CHECK(music.getDuration() == sf::milliseconds(128));

        CHECK(!music.openNextFromMemory(mono.data(), mono.size()));
        CHECK(!music.getNextTrack().open);
        CHECK(!music.crossfadeToNextFrames(10));
        CHECK(music.getUnderrun() == sf::CrossfadeMusic::Underrun::None);
    }

    SECTION("Crossfade from the first sample")
    {
        constexpr std::uint64_t frameCount  = 2048;
        constexpr std::uint64_t fadeFrames  = 4;
        constexpr int           currentBase = 1000;
        constexpr int           nextBase    = 20000;

        const auto currentWav = makeWav(channels, sampleRate, makeRamp(frameCount, channels, currentBase));
        const auto nextWav    = makeWav(channels, sampleRate, makeRamp(frameCount, channels, nextBase));

        TestCrossfadeMusic music;
        REQUIRE(music.openFromMemory(currentWav.data(), currentWav.size()));
        REQUIRE(music.openNextFromMemory(nextWav.data(), nextWav.size()));
        REQUIRE(music.crossfadeToNextFrames(fadeFrames));

        const auto firstChunk = pull(music);
        REQUIRE(firstChunk.continuing);
        REQUIRE(firstChunk.samples.size() == 512 * channels);

        for (std::uint64_t frame = 0; frame < fadeFrames; ++frame)
        {
            const auto gain = static_cast<float>(frame) / static_cast<float>(fadeFrames);

            for (unsigned int channel = 0; channel < channels; ++channel)
            {
                const auto index = static_cast<std::size_t>(frame * channels + channel);
                CHECK(firstChunk.samples[index] ==
                      mixSamples(sampleAt(currentBase, channels, frame, channel),
                                 sampleAt(nextBase, channels, frame, channel),
                                 1.f - gain,
                                 gain));
            }
        }

        for (std::uint64_t frame = fadeFrames; frame < 512; ++frame)
        {
            for (unsigned int channel = 0; channel < channels; ++channel)
            {
                const auto index = static_cast<std::size_t>(frame * channels + channel);
                CHECK(firstChunk.samples[index] == sampleAt(nextBase, channels, frame, channel));
            }
        }

        CHECK(!music.getNextTrack().open);
        CHECK(music.getCurrentTrack().open);
        CHECK(music.getCurrentTrack().positionFrames == 512);
        CHECK(music.getCurrentTrack().gain == 1.f);
        CHECK(music.getStatistics().crossfades == 1);

        const auto secondChunk = pull(music);
        REQUIRE(secondChunk.continuing);
        REQUIRE(secondChunk.samples.size() == 512 * channels);
        CHECK(secondChunk.samples.front() == sampleAt(nextBase, channels, 512, 0));
        CHECK(secondChunk.samples.back() == sampleAt(nextBase, channels, 1023, channels - 1));
    }

    SECTION("Crossfade starts on the next output frame")
    {
        constexpr std::uint64_t frameCount  = 2048;
        constexpr std::uint64_t fadeFrames  = 4;
        constexpr int           currentBase = 1000;
        constexpr int           nextBase    = 20000;

        const auto currentWav = makeWav(channels, sampleRate, makeRamp(frameCount, channels, currentBase));
        const auto nextWav    = makeWav(channels, sampleRate, makeRamp(frameCount, channels, nextBase));

        TestCrossfadeMusic music;
        REQUIRE(music.openFromMemory(currentWav.data(), currentWav.size()));
        REQUIRE(music.openNextFromMemory(nextWav.data(), nextWav.size()));

        const auto firstChunk = pull(music);
        REQUIRE(firstChunk.continuing);
        CHECK(firstChunk.samples.front() == sampleAt(currentBase, channels, 0, 0));
        CHECK(firstChunk.samples.back() == sampleAt(currentBase, channels, 511, channels - 1));

        REQUIRE(music.crossfadeToNextFrames(fadeFrames));
        const auto secondChunk = pull(music);
        REQUIRE(secondChunk.continuing);

        CHECK(secondChunk.samples[0] == sampleAt(currentBase, channels, 512, 0));

        for (std::uint64_t frame = 1; frame <= fadeFrames; ++frame)
        {
            const auto gain = static_cast<float>(frame) / static_cast<float>(fadeFrames);
            for (unsigned int channel = 0; channel < channels; ++channel)
            {
                const auto index = static_cast<std::size_t>(frame * channels + channel);
                CHECK(secondChunk.samples[index] ==
                      mixSamples(sampleAt(currentBase, channels, 512 + frame, channel),
                                 sampleAt(nextBase, channels, frame, channel),
                                 1.f - gain,
                                 gain));
            }
        }

        CHECK(secondChunk.samples.front() == sampleAt(currentBase, channels, 512, 0));
        CHECK(secondChunk.samples[static_cast<std::size_t>(4 * channels)] ==
              sampleAt(nextBase, channels, 4, 0));

        const auto thirdChunk = pull(music);
        REQUIRE(thirdChunk.continuing);
        CHECK(thirdChunk.samples.front() == sampleAt(nextBase, channels, 512, 0));
    }

    SECTION("Zero-duration crossfade switches without a gap")
    {
        constexpr std::uint64_t frameCount  = 2048;
        constexpr int           currentBase = 1000;
        constexpr int           nextBase    = 20000;

        const auto currentWav = makeWav(channels, sampleRate, makeRamp(frameCount, channels, currentBase));
        const auto nextWav    = makeWav(channels, sampleRate, makeRamp(frameCount, channels, nextBase));

        TestCrossfadeMusic music;
        REQUIRE(music.openFromMemory(currentWav.data(), currentWav.size()));
        REQUIRE(music.openNextFromMemory(nextWav.data(), nextWav.size()));

        const auto firstChunk = pull(music);
        REQUIRE(firstChunk.continuing);

        REQUIRE(music.crossfadeToNextFrames(0));
        const auto secondChunk = pull(music);
        REQUIRE(secondChunk.continuing);
        REQUIRE(secondChunk.samples.size() == 512 * channels);

        CHECK(secondChunk.samples.front() == sampleAt(nextBase, channels, 0, 0));
        CHECK(secondChunk.samples.back() == sampleAt(nextBase, channels, 511, channels - 1));
        CHECK(music.getCurrentTrack().positionFrames == 512);
    }

    SECTION("Crossfade cannot be started twice")
    {
        const auto currentWav = makeWav(channels, sampleRate, makeRamp(1024, channels, 1000));
        const auto nextWav    = makeWav(channels, sampleRate, makeRamp(1024, channels, 20000));

        TestCrossfadeMusic music;
        REQUIRE(music.openFromMemory(currentWav.data(), currentWav.size()));
        REQUIRE(music.openNextFromMemory(nextWav.data(), nextWav.size()));
        REQUIRE(music.crossfadeToNextFrames(100));
        CHECK(!music.crossfadeToNextFrames(100));
    }

    SECTION("Underrun state is reported per deck")
    {
        const auto currentWav = makeWav(channels, sampleRate, makeRamp(100, channels, 1000));
        const auto nextWav    = makeWav(channels, sampleRate, makeRamp(600, channels, 20000));

        TestCrossfadeMusic music;
        REQUIRE(music.openFromMemory(currentWav.data(), currentWav.size()));
        REQUIRE(music.openNextFromMemory(nextWav.data(), nextWav.size()));
        REQUIRE(music.crossfadeToNextFrames(200));

        const auto chunk = pull(music);
        REQUIRE(chunk.continuing);
        REQUIRE(chunk.samples.size() == 512 * channels);

        CHECK(music.getUnderrun() == sf::CrossfadeMusic::Underrun::Current);
        CHECK(music.hasUnderrun());
        CHECK(music.getStatistics().underrunFrames == 412);
        CHECK(music.getCurrentTrack().open);
        CHECK(music.getCurrentTrack().positionFrames == 512);
        CHECK(!music.getNextTrack().open);

        music.clearUnderrun();
        CHECK(music.getUnderrun() == sf::CrossfadeMusic::Underrun::None);
        CHECK(!music.hasUnderrun());
        CHECK(music.getStatistics().underrunFrames == 412);
    }

    SECTION("A short next deck reports a next-deck underrun")
    {
        const auto currentWav = makeWav(channels, sampleRate, makeRamp(1024, channels, 1000));
        const auto nextWav    = makeWav(channels, sampleRate, makeRamp(100, channels, 20000));

        TestCrossfadeMusic music;
        REQUIRE(music.openFromMemory(currentWav.data(), currentWav.size()));
        REQUIRE(music.openNextFromMemory(nextWav.data(), nextWav.size()));
        REQUIRE(music.crossfadeToNextFrames(200));

        const auto chunk = pull(music);
        REQUIRE(chunk.continuing);
        CHECK(music.getUnderrun() == sf::CrossfadeMusic::Underrun::Next);
        CHECK(music.getStatistics().underrunFrames == 412);

        const auto exhaustedChunk = pull(music);
        CHECK(!exhaustedChunk.continuing);
        CHECK(music.getUnderrun() == sf::CrossfadeMusic::Underrun::Both);
    }

    SECTION("Search past the end reports an underrun")
    {
        const auto currentWav = makeWav(channels, sampleRate, makeRamp(512, channels, 1000));

        TestCrossfadeMusic music;
        REQUIRE(music.openFromMemory(currentWav.data(), currentWav.size()));
        music.seekToFrame(1'000'000);

        CHECK(music.getFrame() == 512);

        const auto chunk = pull(music);
        CHECK(!chunk.continuing);
        CHECK(chunk.samples.empty());
        CHECK(music.getUnderrun() == sf::CrossfadeMusic::Underrun::Current);
    }

    SECTION("Sample-accurate seeking")
    {
        const auto currentWav = makeWav(channels, sampleRate, makeRamp(4096, channels, 1000));

        TestCrossfadeMusic music;
        REQUIRE(music.openFromMemory(currentWav.data(), currentWav.size()));

        music.seekToFrame(123);
        CHECK(music.getFrame() == 123);

        const auto chunk = pull(music);
        REQUIRE(chunk.continuing);
        REQUIRE(chunk.samples.size() == 512 * channels);
        CHECK(chunk.samples[0] == sampleAt(1000, channels, 123, 0));
        CHECK(chunk.samples[1] == sampleAt(1000, channels, 123, 1));
        CHECK(chunk.samples[2] == sampleAt(1000, channels, 124, 0));
        CHECK(music.getFrame() == 123 + 512);

        music.setPlayingOffset(sf::milliseconds(123));
        CHECK(music.getFrame() == 123);
        music.setPlayingOffset(sf::milliseconds(-100));
        CHECK(music.getFrame() == 0);
        CHECK(music.getDurationFrames() == 4096);
        CHECK(music.getDuration() == sf::milliseconds(4096));
    }

    SECTION("Next deck can be positioned independently")
    {
        const auto currentWav = makeWav(channels, sampleRate, makeRamp(4096, channels, 1000));
        const auto nextWav    = makeWav(channels, sampleRate, makeRamp(4096, channels, 20000));

        TestCrossfadeMusic music;
        REQUIRE(music.openFromMemory(currentWav.data(), currentWav.size()));
        REQUIRE(music.openNextFromMemory(nextWav.data(), nextWav.size()));

        music.seekNextToFrame(77);
        CHECK(music.getNextTrack().positionFrames == 77);
        CHECK(music.getFrame() == 0);

        music.seekNextToFrame(1'000'000);
        CHECK(music.getNextTrack().positionFrames == 4096);
        music.seekNextToFrame(77);

        REQUIRE(music.crossfadeToNextFrames(0));
        const auto chunk = pull(music);
        REQUIRE(chunk.continuing);
        CHECK(chunk.samples.front() == sampleAt(20000, channels, 77, 0));
    }

    SECTION("One thousand sample-accurate seeks do not accumulate state")
    {
        constexpr std::uint64_t frameCount = 8192;
        constexpr std::uint64_t maxSeek    = frameCount - 512;
        constexpr int           base       = 1000;
        constexpr std::uint64_t seekCount  = 1000;

        const auto currentWav = makeWav(channels, sampleRate, makeRamp(frameCount, channels, base));

        TestCrossfadeMusic music;
        REQUIRE(music.openFromMemory(currentWav.data(), currentWav.size()));

        for (std::uint64_t i = 0; i < seekCount; ++i)
        {
            const auto frame = (i * 7919) % maxSeek;
            music.seekToFrame(frame);

            REQUIRE(music.getFrame() == frame);

            const auto chunk = pull(music);
            REQUIRE(chunk.continuing);
            REQUIRE(chunk.samples.size() == 512 * channels);
            CHECK(chunk.samples.front() == sampleAt(base, channels, frame, 0));
            CHECK(chunk.samples.back() == sampleAt(base, channels, frame + 511, channels - 1));
        }

        const auto statistics = music.getStatistics();
        CHECK(statistics.framesRendered == seekCount * 512);
        CHECK(statistics.seeks == seekCount);
        CHECK(statistics.underrunFrames == 0);
        CHECK(statistics.crossfades == 0);
        CHECK(music.getUnderrun() == sf::CrossfadeMusic::Underrun::None);
    }
}
