#pragma once

////////////////////////////////////////////////////////////
// Headers
////////////////////////////////////////////////////////////
#include <SFML/Audio/Export.hpp>

#include <SFML/Audio/SoundStream.hpp>

#include <SFML/System/Time.hpp>

#include <filesystem>
#include <memory>

#include <cstddef>
#include <cstdint>


namespace sf
{
class InputStream;
class InputSoundFile;

////////////////////////////////////////////////////////////
/// \brief Streamed music with two decks and sample-accurate crossfading
///
/// `CrossfadeMusic` mixes a current track and an optional next
/// track. Both tracks must have the same sample rate, channel
/// count and channel map. A crossfade starts at the next sample
/// produced by the audio thread and advances by exactly one frame
/// per output frame, including across chunk boundaries.
///
/// The current deck can be positioned precisely with
/// `setPlayingOffsetFrames` or `seekToFrame`. The next deck can be
/// positioned independently with `seekNextToFrame`.
///
/// When a crossfade completes, the next deck becomes the current
/// deck and the old current deck is closed. `getCurrentTrack()`
/// and `getNextTrack()` always describe that state.
///
/// Underruns are sticky until `clearUnderrun()` is called. A
/// deck raises an underrun when it cannot provide all frames that
/// were requested by the audio callback. This includes reaching
/// the end of a finite track.
///
////////////////////////////////////////////////////////////
class SFML_AUDIO_API CrossfadeMusic : public SoundStream
{
public:
    ////////////////////////////////////////////////////////////
    /// \brief Deck that could not provide all requested frames
    ///
    ////////////////////////////////////////////////////////////
    enum class Underrun : std::uint8_t
    {
        None,    //!< All active decks provided the requested frames
        Current, //!< The current deck could not provide all requested frames
        Next,    //!< The next deck could not provide all requested frames
        Both     //!< Both active decks could not provide all requested frames
    };

    ////////////////////////////////////////////////////////////
    /// \brief Snapshot of a deck
    ///
    ////////////////////////////////////////////////////////////
    struct TrackInfo
    {
        bool          open{};           //!< `true` if the deck contains a track
        std::uint64_t durationFrames{}; //!< Total number of frames in the track
        std::uint64_t positionFrames{}; //!< Next frame that will be read from this deck
        float         gain{};           //!< Gain applied to the last rendered frame
    };

    ////////////////////////////////////////////////////////////
    /// \brief Streaming statistics
    ///
    ////////////////////////////////////////////////////////////
    struct Statistics
    {
        std::uint64_t framesRendered{};  //!< Number of frames returned by the audio callback
        std::uint64_t underrunFrames{};  //!< Sum of missing frames reported by active decks
        std::uint64_t seeks{};           //!< Number of current and next deck seeks
        std::uint64_t crossfades{};      //!< Number of crossfades started
    };

    ////////////////////////////////////////////////////////////
    /// \brief Default constructor
    ///
    ////////////////////////////////////////////////////////////
    CrossfadeMusic();

    ////////////////////////////////////////////////////////////
    /// \brief Destructor
    ///
    ////////////////////////////////////////////////////////////
    ~CrossfadeMusic() override;

    ////////////////////////////////////////////////////////////
    /// \brief Move constructor
    ///
    ////////////////////////////////////////////////////////////
    CrossfadeMusic(CrossfadeMusic&&) noexcept;

    ////////////////////////////////////////////////////////////
    /// \brief Move assignment
    ///
    ////////////////////////////////////////////////////////////
    CrossfadeMusic& operator=(CrossfadeMusic&&) noexcept;

    ////////////////////////////////////////////////////////////
    /// \brief Open the current track from a file
    ///
    /// Opening a current track closes the next track and resets
    /// all positions and statistics.
    ///
    /// \param filename Path of the music file to open
    ///
    /// \return `true` if loading succeeded, `false` otherwise
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] bool openFromFile(const std::filesystem::path& filename);

    ////////////////////////////////////////////////////////////
    /// \brief Open the current track from memory
    ///
    /// The buffer must remain valid until another track is opened
    /// or this object is destroyed.
    ///
    /// \param data        Pointer to the file data in memory
    /// \param sizeInBytes Size of the data to load, in bytes
    ///
    /// \return `true` if loading succeeded, `false` otherwise
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] bool openFromMemory(const void* data, std::size_t sizeInBytes);

    ////////////////////////////////////////////////////////////
    /// \brief Open the current track from a custom stream
    ///
    /// The stream must remain valid until another track is opened
    /// or this object is destroyed.
    ///
    /// \param stream Source stream to read from
    ///
    /// \return `true` if loading succeeded, `false` otherwise
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] bool openFromStream(InputStream& stream);

    ////////////////////////////////////////////////////////////
    /// \brief Open the next track from a file
    ///
    /// The next track must match the current track's sample rate,
    /// channel count and channel map.
    ///
    /// \param filename Path of the music file to open
    ///
    /// \return `true` if loading succeeded, `false` otherwise
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] bool openNextFromFile(const std::filesystem::path& filename);

    ////////////////////////////////////////////////////////////
    /// \brief Open the next track from memory
    ///
    /// The buffer must remain valid until another next track is
    /// opened or this object is destroyed.
    ///
    /// \param data        Pointer to the file data in memory
    /// \param sizeInBytes Size of the data to load, in bytes
    ///
    /// \return `true` if loading succeeded, `false` otherwise
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] bool openNextFromMemory(const void* data, std::size_t sizeInBytes);

    ////////////////////////////////////////////////////////////
    /// \brief Open the next track from a custom stream
    ///
    /// The stream must remain valid until another next track is
    /// opened or this object is destroyed.
    ///
    /// \param stream Source stream to read from
    ///
    /// \return `true` if loading succeeded, `false` otherwise
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] bool openNextFromStream(InputStream& stream);

    ////////////////////////////////////////////////////////////
    /// \brief Start a linear crossfade from the current to the next track
    ///
    /// The next track begins at its current position. The crossfade
    /// starts at the next sample produced by the audio thread and
    /// lasts for \a duration. A zero duration switches at the next
    /// sample boundary.
    ///
    /// \param duration Duration of the crossfade
    ///
    /// \return `true` if the crossfade was started, `false` if there is no next track or a crossfade is already active
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] bool crossfadeToNext(Time duration);

    ////////////////////////////////////////////////////////////
    /// \brief Start a linear crossfade using an exact frame duration
    ///
    /// \param durationFrames Duration of the crossfade, in sample frames
    ///
    /// \return `true` if the crossfade was started, `false` if there is no next track or a crossfade is already active
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] bool crossfadeToNextFrames(std::uint64_t durationFrames);

    ////////////////////////////////////////////////////////////
    /// \brief Set the current playing position of the current track
    ///
    /// This overload avoids floating-point time conversion and is
    /// exact at sample-frame granularity.
    ///
    /// \param timeOffset New playing position
    ///
    ////////////////////////////////////////////////////////////
    void setPlayingOffset(Time timeOffset);

    ////////////////////////////////////////////////////////////
    /// \brief Set the current playing position of the current track in frames
    ///
    /// \param frameOffset New playing position, in sample frames
    ///
    ////////////////////////////////////////////////////////////
    void seekToFrame(std::uint64_t frameOffset);

    ////////////////////////////////////////////////////////////
    /// \brief Set the current playing position of the next track in frames
    ///
    /// This can be used to prepare the next track before starting
    /// a crossfade.
    ///
    /// \param frameOffset New playing position, in sample frames
    ///
    ////////////////////////////////////////////////////////////
    void seekNextToFrame(std::uint64_t frameOffset);

    ////////////////////////////////////////////////////////////
    /// \brief Get the current playing position of the current track in frames
    ///
    /// \return Current playing position, in sample frames
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] std::uint64_t getFrame() const;

    ////////////////////////////////////////////////////////////
    /// \brief Get the duration of the current track
    ///
    /// \return Current track duration, or `sf::Time::Zero` if no track is open
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] Time getDuration() const;

    ////////////////////////////////////////////////////////////
    /// \brief Get the duration of the current track in frames
    ///
    /// \return Current track duration, in sample frames
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] std::uint64_t getDurationFrames() const;

    ////////////////////////////////////////////////////////////
    /// \brief Get the current underrun state
    ///
    /// The state is sticky and is cleared by `clearUnderrun()`.
    ///
    /// \return Decks that underran since the last clear
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] Underrun getUnderrun() const;

    ////////////////////////////////////////////////////////////
    /// \brief Tell whether a current or next deck underran
    ///
    /// \return `true` if at least one deck underran
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] bool hasUnderrun() const;

    ////////////////////////////////////////////////////////////
    /// \brief Clear the sticky underrun state
    ///
    /// The cumulative `Statistics::underrunFrames` value is not
    /// reset by this function.
    ///
    ////////////////////////////////////////////////////////////
    void clearUnderrun();

    ////////////////////////////////////////////////////////////
    /// \brief Get a snapshot of the current deck
    ///
    /// \return Current deck information
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] TrackInfo getCurrentTrack() const;

    ////////////////////////////////////////////////////////////
    /// \brief Get a snapshot of the next deck
    ///
    /// \return Next deck information
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] TrackInfo getNextTrack() const;

    ////////////////////////////////////////////////////////////
    /// \brief Get a snapshot of the streaming statistics
    ///
    /// \return Streaming statistics
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] Statistics getStatistics() const;

protected:
    ////////////////////////////////////////////////////////////
    /// \brief Request a new chunk of mixed audio samples
    ///
    /// \param data Chunk of data to fill
    ///
    /// \return `true` to continue playback, `false` to stop
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] bool onGetData(Chunk& data) override;

    ////////////////////////////////////////////////////////////
    /// \brief Change the current playing position of the current track
    ///
    /// \param timeOffset New playing position
    ///
    ////////////////////////////////////////////////////////////
    void onSeek(Time timeOffset) override;

    ////////////////////////////////////////////////////////////
    /// \brief Change the current playing position of the current track, in frames
    ///
    /// \param frameOffset New playing position, in sample frames
    ///
    ////////////////////////////////////////////////////////////
    void onSeekToFrame(std::uint64_t frameOffset) override;

private:
    ////////////////////////////////////////////////////////////
    /// \brief Install a validated current track
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] bool openCurrent(std::unique_ptr<InputSoundFile> file);

    ////////////////////////////////////////////////////////////
    /// \brief Install a validated next track
    ///
    ////////////////////////////////////////////////////////////
    [[nodiscard]] bool openNext(std::unique_ptr<InputSoundFile> file);

    ////////////////////////////////////////////////////////////
    // Member data
    ////////////////////////////////////////////////////////////
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace sf
