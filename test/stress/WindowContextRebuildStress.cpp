#include <SFML/Graphics.hpp>
#include <SFML/System.hpp>
#include <SFML/Window.hpp>

#if defined(SFML_STRESS_HAS_X11)
#include <X11/Xlib.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <malloc.h>
#include <unistd.h>
#endif

#if !defined(SFML_SYSTEM_WINDOWS)
#include <unistd.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SFML_STRESS_ADDRESS_SANITIZER
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define SFML_STRESS_ADDRESS_SANITIZER
#endif

#if !defined(SFML_STRESS_ADDRESS_SANITIZER)
#include <csignal>
#endif

namespace
{
constexpr sf::Vector2u canonicalSize{320, 240};
constexpr std::uint64_t fnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t fnvPrime  = 1099511628211ULL;

#if defined(SFML_STRESS_HAS_X11)
int g_x11ErrorCount = 0;

int countX11Error(Display*, XErrorEvent*)
{
    ++g_x11ErrorCount;
    return 0;
}

class X11ErrorTrap
{
public:
    X11ErrorTrap() : m_previous(XSetErrorHandler(countX11Error))
    {
    }

    ~X11ErrorTrap()
    {
        XSetErrorHandler(m_previous);
    }

private:
    XErrorHandler m_previous;
};
#endif

std::array<char, 65536> g_sequenceBuffer{};
std::size_t             g_sequenceSize = 0;

enum class OperationKind
{
    CaptureGolden,
    Create,
    Recreate,
    DuplicateCreate,
    Close,
    Activate,
    Deactivate,
    DestroyDuringSwitch,
    DestroyAfterDeactivate,
    InvalidHandle,
    RecreateRenderTexture,
    RecreateContext,
    PollEvents,
    InjectSceneFault,
    CompareImage
};

struct Operation
{
    OperationKind kind;
    int           slot{-1};
    int           otherSlot{-1};
    std::uint64_t value{0};
};

std::string describe(const Operation& operation)
{
    std::ostringstream stream;

    switch (operation.kind)
    {
        case OperationKind::CaptureGolden:
            return "CaptureGoldenImage";
        case OperationKind::Create:
            stream << "CreateWindow(slot=" << operation.slot << ')';
            break;
        case OperationKind::Recreate:
            stream << "RecreateWindow(slot=" << operation.slot << ')';
            break;
        case OperationKind::DuplicateCreate:
            stream << "CreateWindowTwice(slot=" << operation.slot << ')';
            break;
        case OperationKind::Close:
            stream << "CloseWindow(slot=" << operation.slot << ')';
            break;
        case OperationKind::Activate:
            stream << "ActivateWindow(slot=" << operation.slot << ')';
            break;
        case OperationKind::Deactivate:
            stream << "DeactivateWindow(slot=" << operation.slot << ')';
            break;
        case OperationKind::DestroyDuringSwitch:
            stream << "DestroyDuringSwitch(destroy=" << operation.slot << ", activate=" << operation.otherSlot << ')';
            break;
        case OperationKind::DestroyAfterDeactivate:
            stream << "DestroyAfterDeactivate(slot=" << operation.slot << ')';
            break;
        case OperationKind::InvalidHandle:
            stream << "CreateInvalidHandle(0x" << std::hex << operation.value << std::dec << ')';
            break;
        case OperationKind::RecreateRenderTexture:
            return "CreateResizeDestroyRenderTexture";
        case OperationKind::RecreateContext:
            return "SwitchDestroyRecreateContext";
        case OperationKind::PollEvents:
            return "PollAllWindowEvents";
        case OperationKind::InjectSceneFault:
            return "InjectSceneFault";
        case OperationKind::CompareImage:
            return "CompareCapturedImage";
    }

    return stream.str();
}

void publishSequence(int iteration, const std::vector<Operation>& operations)
{
    const int written = std::snprintf(g_sequenceBuffer.data(),
                                      g_sequenceBuffer.size(),
                                      "iteration=%d, seed sequence:\n",
                                      iteration);
    g_sequenceSize    = written > 0 ? static_cast<std::size_t>(written) : 0;

    for (std::size_t i = 0; i < operations.size() && g_sequenceSize < g_sequenceBuffer.size(); ++i)
    {
        const int count = std::snprintf(g_sequenceBuffer.data() + g_sequenceSize,
                                        g_sequenceBuffer.size() - g_sequenceSize,
                                        "#%zu %s\n",
                                        i,
                                        describe(operations[i]).c_str());
        if (count <= 0)
            break;
        if (static_cast<std::size_t>(count) >= g_sequenceBuffer.size() - g_sequenceSize)
        {
            g_sequenceSize = g_sequenceBuffer.size() - 1;
            break;
        }
        g_sequenceSize += static_cast<std::size_t>(count);
    }
}

#if !defined(SFML_STRESS_ADDRESS_SANITIZER) && !defined(SFML_SYSTEM_WINDOWS)
void onFatalSignal(int signal)
{
    constexpr char prefix[] = "\nFATAL SIGNAL during operation sequence:\n";
    const auto ignoredPrefix = ::write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    const auto ignoredBody   = ::write(STDERR_FILENO, g_sequenceBuffer.data(), g_sequenceSize);
    (void)ignoredPrefix;
    (void)ignoredBody;
    _exit(128 + signal);
}

void installCrashHandler()
{
    struct sigaction action
    {};
    action.sa_handler = onFatalSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = static_cast<int>(SA_RESETHAND);

    for (const int signal : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT})
        sigaction(signal, &action, nullptr);
}
#else
void installCrashHandler()
{
}
#endif

std::uint64_t hashImage(const sf::Image& image)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(image.getPixelsPtr());
    const auto  size  = static_cast<std::size_t>(image.getSize().x) * image.getSize().y * 4;

    std::uint64_t hash = fnvOffset;
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= fnvPrime;
    }
    return hash;
}

sf::Image makeSpriteImage()
{
    sf::Image image({64, 64}, sf::Color(14, 20, 28));

    for (unsigned int y = 0; y < 64; ++y)
    {
        for (unsigned int x = 0; x < 64; ++x)
        {
            const auto checker = static_cast<std::uint8_t>(((x / 8) + (y / 8)) % 2 == 0 ? 196 : 32);
            image.setPixel({x, y},
                           sf::Color(checker,
                                     static_cast<std::uint8_t>((x * 4) & 0xFF),
                                     static_cast<std::uint8_t>((y * 4) & 0xFF)));
        }
    }

    return image;
}

void drawScene(sf::RenderTarget& target, const sf::Image& spriteImage)
{
    target.clear(sf::Color(9, 13, 21));

    sf::RectangleShape panel({286.f, 204.f});
    panel.setPosition({17.f, 18.f});
    panel.setFillColor(sf::Color(29, 47, 67));
    panel.setOutlineColor(sf::Color(87, 134, 168));
    panel.setOutlineThickness(2.f);
    target.draw(panel);

    for (unsigned int i = 0; i < 8; ++i)
    {
        sf::RectangleShape bar({24.f, 13.f});
        bar.setPosition({30.f + static_cast<float>(i) * 32.f, 38.f + static_cast<float>((i * 17) % 42)});
        bar.setFillColor(sf::Color(static_cast<std::uint8_t>(40 + i * 21),
                                   176,
                                   static_cast<std::uint8_t>(220 - i * 13)));
        target.draw(bar);
    }

    sf::CircleShape circle(27.f, 48);
    circle.setPosition({74.f, 112.f});
    circle.setFillColor(sf::Color(225, 102, 61));
    circle.setOutlineColor(sf::Color(249, 205, 124));
    circle.setOutlineThickness(3.f);
    target.draw(circle);

    sf::ConvexShape diamond(4);
    diamond.setPoint(0, {0.f, -25.f});
    diamond.setPoint(1, {32.f, 0.f});
    diamond.setPoint(2, {0.f, 25.f});
    diamond.setPoint(3, {-32.f, 0.f});
    diamond.setPosition({216.f, 168.f});
    diamond.setFillColor(sf::Color(99, 204, 149));
    target.draw(diamond);

    sf::Texture texture(spriteImage);
    texture.setSmooth(false);
    sf::Sprite sprite(texture);
    sprite.setPosition({139.f, 112.f});
    target.draw(sprite);

    sf::VertexArray triangle(sf::PrimitiveType::Triangles, 3);
    triangle[0].position = {34.f, 192.f};
    triangle[0].color    = sf::Color(237, 73, 91);
    triangle[1].position = {74.f, 192.f};
    triangle[1].color    = sf::Color(68, 226, 190);
    triangle[2].position = {54.f, 220.f};
    triangle[2].color    = sf::Color(90, 126, 240);
    target.draw(triangle);
}

sf::Image captureWindow(sf::RenderWindow& window, const sf::Image& spriteImage, bool injectFault)
{
    if (!window.setActive(true))
        throw std::runtime_error("failed to activate canonical window");

    drawScene(window, spriteImage);

    sf::Texture readback(window.getSize());
    readback.update(window);

    sf::Image image = readback.copyToImage();

    if (injectFault)
    {
        const sf::Vector2u pixel{31, 47};
        const sf::Color    oldColor = image.getPixel(pixel);
        image.setPixel(pixel, sf::Color(static_cast<std::uint8_t>(oldColor.r ^ 0xFF),
                                        static_cast<std::uint8_t>(oldColor.g ^ 0xFF),
                                        static_cast<std::uint8_t>(oldColor.b ^ 0xFF),
                                        oldColor.a));
    }

    return image;
}

sf::WindowHandle invalidHandle(std::uint64_t value)
{
#if defined(SFML_SYSTEM_WINDOWS) || defined(SFML_SYSTEM_MACOS) || defined(SFML_SYSTEM_IOS) || \
    defined(SFML_SYSTEM_ANDROID)
    return reinterpret_cast<sf::WindowHandle>(static_cast<std::uintptr_t>(value));
#else
    return static_cast<sf::WindowHandle>(value);
#endif
}

struct Sandbox
{
    std::array<std::unique_ptr<sf::RenderWindow>, 3> windows;
    std::optional<sf::Image>                         golden;
    bool                                             injectFault{};

    sf::RenderWindow& ensureWindow(int slot)
    {
        if (!windows[static_cast<std::size_t>(slot)])
        {
            windows[static_cast<std::size_t>(slot)] = std::make_unique<sf::RenderWindow>(
                sf::VideoMode(canonicalSize, 24),
                "SFML context rebuild stress",
                sf::Style::Default,
                sf::State::Windowed,
                sf::ContextSettings{});
        }

        return *windows[static_cast<std::size_t>(slot)];
    }

    sf::RenderWindow* getWindow(int slot)
    {
        if (slot < 0 || slot >= static_cast<int>(windows.size()))
            return nullptr;
        return windows[static_cast<std::size_t>(slot)].get();
    }
};

enum class FailureKind
{
    NoFailure,
    Exception,
    ImageMismatch
};

struct Failure
{
    FailureKind   kind{FailureKind::NoFailure};
    std::string   detail;
    std::uint64_t goldenHash{};
    std::uint64_t currentHash{};
    std::size_t   mismatchedPixels{};

    [[nodiscard]] bool hasValue() const
    {
        return kind != FailureKind::NoFailure;
    }
};

class Harness
{
public:
    Harness(const sf::Image& expected, const sf::Image& spriteImage) :
    m_expected(expected),
    m_spriteImage(spriteImage)
    {
    }

    Failure run(const std::vector<Operation>& operations) const
    {
        Sandbox sandbox;

        for (const auto& operation : operations)
        {
            try
            {
                if (auto failure = execute(sandbox, operation); failure.hasValue())
                    return failure;
            }
            catch (const std::exception& exception)
            {
                Failure failure;
                failure.kind   = FailureKind::Exception;
                failure.detail = std::string(describe(operation)) + ": " + exception.what();
                return failure;
            }
            catch (...)
            {
                Failure failure;
                failure.kind   = FailureKind::Exception;
                failure.detail = std::string(describe(operation)) + ": unknown exception";
                return failure;
            }
        }

        return {};
    }

private:
    Failure execute(Sandbox& sandbox, const Operation& operation) const
    {
        switch (operation.kind)
        {
            case OperationKind::CaptureGolden:
                sandbox.golden = m_expected;
                return {};

            case OperationKind::Create:
                sandbox.ensureWindow(operation.slot);
                return {};

            case OperationKind::Recreate:
            {
                auto& window = sandbox.ensureWindow(operation.slot);
                window.create(sf::VideoMode(canonicalSize, 24),
                              "SFML context rebuild stress",
                              sf::Style::Default,
                              sf::State::Windowed,
                              sf::ContextSettings{});
                return {};
            }

            case OperationKind::DuplicateCreate:
            {
                auto& window = sandbox.ensureWindow(operation.slot);
                window.create(sf::VideoMode(canonicalSize, 24),
                              "SFML context rebuild stress",
                              sf::Style::Default,
                              sf::State::Windowed,
                              sf::ContextSettings{});
                window.create(sf::VideoMode(canonicalSize, 24),
                              "SFML context rebuild stress",
                              sf::Style::Default,
                              sf::State::Windowed,
                              sf::ContextSettings{});
                return {};
            }

            case OperationKind::Close:
                sandbox.windows[static_cast<std::size_t>(operation.slot)].reset();
                return {};

            case OperationKind::Activate:
                if (!sandbox.ensureWindow(operation.slot).setActive(true))
                    throw std::runtime_error("setActive(true) failed");
                return {};

            case OperationKind::Deactivate:
                if (auto* window = sandbox.getWindow(operation.slot))
                    window->setActive(false);
                return {};

            case OperationKind::DestroyDuringSwitch:
            {
                auto& activate = sandbox.ensureWindow(operation.otherSlot);
                (void)activate.setActive(true);
                sandbox.windows[static_cast<std::size_t>(operation.slot)].reset();
                return {};
            }

            case OperationKind::DestroyAfterDeactivate:
            {
                auto& window = sandbox.ensureWindow(operation.slot);
                (void)window.setActive(false);
                sandbox.windows[static_cast<std::size_t>(operation.slot)].reset();
                return {};
            }

            case OperationKind::InvalidHandle:
            {
                sandbox.ensureWindow(0);
                const auto handle = invalidHandle(operation.value);

                {
                    sf::RenderWindow invalid;
                    invalid.create(handle);
                }

                {
                    sf::RenderWindow invalid;
                    invalid.create(handle);
                    invalid.create(sf::VideoMode(canonicalSize, 24),
                                   "SFML context rebuild stress",
                                   sf::Style::Default,
                                   sf::State::Windowed,
                                   sf::ContextSettings{});
                    invalid.close();
                }

                return {};
            }

            case OperationKind::RecreateRenderTexture:
            {
                sandbox.ensureWindow(0);
                sf::RenderTexture renderTexture({96, 80});
                renderTexture.clear(sf::Color(7, 11, 19));
                drawScene(renderTexture, m_spriteImage);
                if (!renderTexture.resize({80, 64}))
                    throw std::runtime_error("RenderTexture::resize failed");
                return {};
            }

            case OperationKind::RecreateContext:
            {
                sandbox.ensureWindow(0);
                sf::Context first;
                (void)first.setActive(true);
                {
                    sf::Context second;
                    (void)second.setActive(true);
                    (void)second.setActive(false);
                }
                (void)first.setActive(true);
                return {};
            }

            case OperationKind::PollEvents:
                for (auto& window : sandbox.windows)
                {
                    if (!window)
                        continue;
                    while (window->pollEvent())
                    {
                    }
                }
                return {};

            case OperationKind::InjectSceneFault:
                sandbox.injectFault = true;
                return {};

            case OperationKind::CompareImage:
            {
                if (!sandbox.golden)
                    return {};

                auto& window = sandbox.ensureWindow(0);
                const sf::Image current = captureWindow(window, m_spriteImage, sandbox.injectFault);

                if (current.getSize() != sandbox.golden->getSize())
                {
                    Failure failure;
                    failure.kind   = FailureKind::ImageMismatch;
                    failure.detail = "image size mismatch";
                    return failure;
                }

                Failure failure;
                failure.goldenHash  = hashImage(*sandbox.golden);
                failure.currentHash = hashImage(current);

                if (failure.goldenHash == failure.currentHash)
                    return {};

                const auto  size = current.getSize();
                const auto& left = sandbox.golden->getPixelsPtr();
                const auto& right = current.getPixelsPtr();
                for (std::size_t i = 0; i < static_cast<std::size_t>(size.x) * size.y * 4; i += 4)
                {
                    if (std::memcmp(left + i, right + i, 4) != 0)
                        ++failure.mismatchedPixels;
                }

                failure.kind   = FailureKind::ImageMismatch;
                failure.detail = "image hash mismatch after context restore";
                return failure;
            }
        }

        return {};
    }

    const sf::Image& m_expected;
    const sf::Image& m_spriteImage;
};

bool sameFailure(const Failure& left, const Failure& right)
{
    if (left.kind != right.kind)
        return false;
    if (left.kind == FailureKind::ImageMismatch)
        return left.mismatchedPixels == right.mismatchedPixels;
    return left.detail == right.detail;
}

std::vector<Operation> minimize(const std::vector<Operation>& operations, const Harness& harness, const Failure& target)
{
    std::vector<Operation> current = operations;

    auto reproduces = [&](const std::vector<Operation>& candidate)
    {
        const Failure failure = harness.run(candidate);
        return failure.hasValue() && sameFailure(failure, target);
    };

    if (!reproduces(current))
        return current;

    std::size_t granularity = 2;
    while (current.size() >= 2)
    {
        const std::size_t chunkSize = (current.size() + granularity - 1) / granularity;
        bool              reduced   = false;

        for (std::size_t begin = 0; begin < current.size(); begin += chunkSize)
        {
            const std::size_t end = std::min(current.size(), begin + chunkSize);
            std::vector<Operation> candidate;
            candidate.reserve(current.size() - (end - begin));
            candidate.insert(candidate.end(), current.begin(), current.begin() + static_cast<std::ptrdiff_t>(begin));
            candidate.insert(candidate.end(), current.begin() + static_cast<std::ptrdiff_t>(end), current.end());

            if (reproduces(candidate))
            {
                current     = std::move(candidate);
                granularity = std::max<std::size_t>(2, granularity - 1);
                reduced     = true;
                break;
            }
        }

        if (!reduced)
        {
            if (granularity >= current.size())
                break;
            granularity = std::min(current.size(), granularity * 2);
        }
    }

    return current;
}

std::vector<Operation> makeOperations(std::mt19937_64& random, bool faultThisIteration)
{
    std::vector<Operation> body;
    body.push_back({OperationKind::Create, 0, -1, 0});
    body.push_back({OperationKind::Create, 1, -1, 0});
    body.push_back({OperationKind::DuplicateCreate, static_cast<int>(random() % 2), -1, 0});
    body.push_back({OperationKind::DestroyDuringSwitch, 0, 1, 0});
    body.push_back({OperationKind::DestroyAfterDeactivate, 1, -1, 0});
    body.push_back({OperationKind::InvalidHandle, 2, -1, random() % 2 == 0 ? 0 : 0x7FFFFFF0ULL});
    body.push_back({OperationKind::RecreateRenderTexture, -1, -1, 0});
    body.push_back({OperationKind::RecreateContext, -1, -1, 0});
    body.push_back({OperationKind::Activate, static_cast<int>(random() % 2), -1, 0});
    body.push_back({OperationKind::PollEvents, -1, -1, 0});

    if (const auto kindRoll = random() % 3; kindRoll == 0)
        body.push_back({OperationKind::Recreate, static_cast<int>(random() % 2), -1, 0});
    else if (kindRoll == 1)
        body.push_back({OperationKind::Close, static_cast<int>(random() % 2), -1, 0});
    else
        body.push_back({OperationKind::Deactivate, static_cast<int>(random() % 2), -1, 0});

    std::shuffle(body.begin(), body.end(), random);

    std::vector<Operation> operations;
    operations.reserve(body.size() + 3);
    operations.push_back({OperationKind::CaptureGolden, -1, -1, 0});
    operations.insert(operations.end(), body.begin(), body.end());
    if (faultThisIteration)
        operations.push_back({OperationKind::InjectSceneFault, -1, -1, 0});
    operations.push_back({OperationKind::CompareImage, -1, -1, 0});
    return operations;
}

struct ProcessMetrics
{
    std::size_t fdCount{};
    std::size_t rssBytes{};
    std::size_t heapBytes{};
};

std::size_t countFileDescriptors()
{
#if defined(__linux__)
    DIR* directory = opendir("/proc/self/fd");
    if (!directory)
        return 0;

    std::size_t count = 0;
    while (const dirent* entry = readdir(directory))
    {
        if (entry->d_name[0] != '.')
            ++count;
    }
    closedir(directory);
    return count;
#else
    return 0;
#endif
}

std::size_t residentBytes()
{
#if defined(__linux__)
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (!file)
        return 0;

    unsigned long totalPages = 0;
    unsigned long residentPages = 0;
    const int     read = std::fscanf(file, "%lu %lu", &totalPages, &residentPages);
    std::fclose(file);
    (void)totalPages;
    return read == 2 ? residentPages * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)) : 0;
#else
    return 0;
#endif
}

std::size_t heapBytes()
{
#if defined(__linux__) && defined(__GLIBC__)
    const auto info = mallinfo2();
    return info.uordblks + info.hblkhd;
#else
    return 0;
#endif
}

ProcessMetrics processMetrics()
{
    return {countFileDescriptors(), residentBytes(), heapBytes()};
}

std::string humanBytes(std::size_t bytes)
{
    std::ostringstream stream;
    stream << bytes << " bytes";
    return stream.str();
}

void printMetrics(const char* tag, int iteration, int total)
{
    const auto metrics = processMetrics();
    std::printf("[%s] iter %d/%d rss=%s fds=%zu heap=%s x11-errors-swallowed=%d\n",
                tag,
                iteration,
                total,
                humanBytes(metrics.rssBytes).c_str(),
                metrics.fdCount,
                humanBytes(metrics.heapBytes).c_str(),
#if defined(SFML_STRESS_HAS_X11)
                g_x11ErrorCount
#else
                0
#endif
    );
}

void printFailure(int iteration, const Failure& failure)
{
    std::printf("[main] FAIL at iteration %d: %s", iteration, failure.detail.c_str());
    if (failure.kind == FailureKind::ImageMismatch)
    {
        std::printf(" golden=%llu current=%llu mismatched_pixels=%zu",
                    static_cast<unsigned long long>(failure.goldenHash),
                    static_cast<unsigned long long>(failure.currentHash),
                    failure.mismatchedPixels);
    }
    std::printf("\n");
}

void printSequence(const std::vector<Operation>& operations)
{
    std::printf("--- Minimal failing operation sequence (%zu ops) ---\n", operations.size());
    for (std::size_t i = 0; i < operations.size(); ++i)
        std::printf("#%zu %s\n", i, describe(operations[i]).c_str());
    std::printf("--- end of sequence ---\n");
}

struct Config
{
    int      iterations{200};
    std::uint64_t seed{0xC0FFEE123456789ULL};
    int      faultInjection{-1};
    bool     minimizeFailure{true};
    bool     showSfmlErrors{false};
    int      warmupIterations{20};
    std::size_t maxRssGrowth{static_cast<std::size_t>(192) * 1024 * 1024};
    std::size_t maxHeapGrowth{static_cast<std::size_t>(8) * 1024 * 1024};
};

bool parseInteger(const char* value, int& output)
{
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (!end || *end != '\0' || parsed < 0)
        return false;
    output = static_cast<int>(parsed);
    return true;
}

std::optional<Config> parseArguments(int argc, char** argv)
{
    Config config;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];

        if (argument == "--help")
        {
            std::printf("Usage: %s [--iterations N] [--seed N] [--fault-inject N] [--no-minimize]\n", argv[0]);
            return std::nullopt;
        }

        if (argument == "--no-minimize")
        {
            config.minimizeFailure = false;
            continue;
        }

        if (argument == "--show-sfml-errors")
        {
            config.showSfmlErrors = true;
            continue;
        }

        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "Missing value for %s\n", argument.c_str());
            return std::nullopt;
        }

        if (argument == "--iterations")
        {
            if (!parseInteger(argv[++i], config.iterations))
                return std::nullopt;
        }
        else if (argument == "--fault-inject")
        {
            if (!parseInteger(argv[++i], config.faultInjection))
                return std::nullopt;
        }
        else if (argument == "--seed")
        {
            char* end = nullptr;
            config.seed = std::strtoull(argv[++i], &end, 0);
            if (!end || *end != '\0')
                return std::nullopt;
        }
        else if (argument == "--max-rss-growth-mb")
        {
            int megabytes = 0;
            if (!parseInteger(argv[++i], megabytes))
                return std::nullopt;
            config.maxRssGrowth = static_cast<std::size_t>(megabytes) * 1024 * 1024;
        }
        else
        {
            std::fprintf(stderr, "Unknown argument: %s\n", argument.c_str());
            return std::nullopt;
        }
    }

    return config;
}
} // namespace

int main(int argc, char** argv)
{
    const auto config = parseArguments(argc, argv);
    if (!config)
        return 0;

    installCrashHandler();

    if (!config->showSfmlErrors)
        sf::err().rdbuf(nullptr);

#if defined(SFML_STRESS_HAS_X11)
    X11ErrorTrap errorTrap;
#endif

    const sf::Image spriteImage = makeSpriteImage();

    sf::Image expected;
    try
    {
        sf::RenderWindow window(sf::VideoMode(canonicalSize, 24),
                                "SFML context rebuild golden",
                                sf::Style::Default,
                                sf::State::Windowed,
                                sf::ContextSettings{});
        expected = captureWindow(window, spriteImage, false);
    }
    catch (const std::exception& exception)
    {
        std::fprintf(stderr, "Failed to establish golden image: %s\n", exception.what());
        return 2;
    }

    const std::uint64_t goldenHash = hashImage(expected);
    std::printf("Window/context rebuild stress test: %d iterations, seed=%llu",
                config->iterations,
                static_cast<unsigned long long>(config->seed));
    if (config->faultInjection >= 0)
        std::printf(", fault injection at iteration %d", config->faultInjection);
    std::printf(", golden=%llu\n", static_cast<unsigned long long>(goldenHash));

    Harness harness(expected, spriteImage);
    std::mt19937_64 random(config->seed);
    ProcessMetrics baseline;
    const auto start = std::chrono::steady_clock::now();

    for (int iteration = 0; iteration < config->iterations; ++iteration)
    {
        const bool injectFault = config->faultInjection == iteration;
        const auto operations  = makeOperations(random, injectFault);
        publishSequence(iteration + 1, operations);

        const Failure failure = harness.run(operations);
        if (failure.hasValue())
        {
            printFailure(iteration + 1, failure);

            if (config->minimizeFailure)
            {
                const auto minimized = minimize(operations, harness, failure);
                std::printf("[ddmin] reduced %zu operations to %zu\n", operations.size(), minimized.size());
                printSequence(minimized);
            }
            else
            {
                printSequence(operations);
            }
            return 1;
        }

        if (iteration + 1 == config->warmupIterations)
            baseline = processMetrics();

        if ((iteration + 1) % 25 == 0)
            printMetrics("main", iteration + 1, config->iterations);
    }

    const auto end      = std::chrono::steady_clock::now();
    const auto elapsed  = std::chrono::duration<double>(end - start).count();
    const auto finished = processMetrics();

    if (baseline.fdCount == 0)
        baseline = finished;

    const auto delta = [](std::size_t after, std::size_t before)
    {
        if (after >= before)
            return static_cast<std::ptrdiff_t>(after - before);
        return -static_cast<std::ptrdiff_t>(before - after);
    };
    const auto fdDelta   = delta(finished.fdCount, baseline.fdCount);
    const auto rssDelta  = delta(finished.rssBytes, baseline.rssBytes);
    const auto heapDelta = delta(finished.heapBytes, baseline.heapBytes);

    std::printf("[main] completed %d iterations, rss_delta=%td bytes fd_delta=%td heap_delta=%td\n",
                config->iterations,
                rssDelta,
                fdDelta,
                heapDelta);

    if (fdDelta != 0)
    {
        std::printf("FAIL: file descriptor leak detected\n");
        return 1;
    }

    if (heapDelta > static_cast<std::ptrdiff_t>(config->maxHeapGrowth))
    {
        std::printf("FAIL: heap leak growth exceeds %s\n", humanBytes(config->maxHeapGrowth).c_str());
        return 1;
    }

    if (rssDelta > static_cast<std::ptrdiff_t>(config->maxRssGrowth))
    {
        std::printf("FAIL: RSS growth exceeds %s\n", humanBytes(config->maxRssGrowth).c_str());
        return 1;
    }

    std::printf("PASS: zero crashes, no leak growth, all %d rebuilt contexts rendered identical images (%.6fs).\n",
                config->iterations,
                elapsed);
    return 0;
}
