#include <SFML/Graphics/BlendMode.hpp>
#include <SFML/Graphics/Color.hpp>
#include <SFML/Graphics/Image.hpp>
#include <SFML/Graphics/RectangleShape.hpp>
#include <SFML/Graphics/RenderTexture.hpp>
#include <SFML/Graphics/Sprite.hpp>
#include <SFML/Graphics/Texture.hpp>
#include <SFML/Graphics/VertexBuffer.hpp>
#include <SFML/Graphics/View.hpp>

#include <catch2/catch_test_macros.hpp>

#include <WindowUtil.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{
constexpr sf::Vector2u targetSize{64, 48};
constexpr sf::Color    backgroundColor(30, 60, 90);

struct PixelData
{
    std::vector<std::uint8_t> bytes;
};

PixelData readPixels(sf::RenderTexture& target)
{
    target.display();
    const sf::Image image = target.getTexture().copyToImage();
    const auto*     data  = image.getPixelsPtr();
    const auto      count = static_cast<std::size_t>(targetSize.x) * targetSize.y * 4;
    return {std::vector<std::uint8_t>(data, data + count)};
}

std::size_t pixelOffset(int x, int y)
{
    return (static_cast<std::size_t>(y) * targetSize.x + static_cast<std::size_t>(x)) * 4;
}

sf::IntRect clipped(sf::IntRect region)
{
    const sf::IntRect bounds({0, 0}, {static_cast<int>(targetSize.x), static_cast<int>(targetSize.y)});
    return region.findIntersection(bounds).value_or(sf::IntRect{{0, 0}, {0, 0}});
}

struct Diff
{
    std::size_t insideMismatches{};
    std::size_t outsideMismatches{};
    std::size_t maxChannelDelta{};
};

Diff compareRegion(const PixelData& regionPixels, const PixelData& fullPixels, sf::IntRect region)
{
    region    = clipped(region);
    Diff result;

    for (int y = 0; y < static_cast<int>(targetSize.y); ++y)
    {
        for (int x = 0; x < static_cast<int>(targetSize.x); ++x)
        {
            const bool        inside = region.contains({x, y});
            const std::size_t offset = pixelOffset(x, y);

            for (std::size_t channel = 0; channel < 4; ++channel)
            {
                const std::uint8_t actual = regionPixels.bytes[offset + channel];
                const std::uint8_t expected = inside
                                                  ? fullPixels.bytes[offset + channel]
                                                  : std::array<std::uint8_t, 4>{
                                                        backgroundColor.r,
                                                        backgroundColor.g,
                                                        backgroundColor.b,
                                                        backgroundColor.a,
                                                    }[channel];

                const auto delta = static_cast<std::size_t>(
                    std::abs(static_cast<int>(actual) - static_cast<int>(expected)));
                result.maxChannelDelta = std::max(result.maxChannelDelta, delta);

                if (actual != expected)
                    inside ? ++result.insideMismatches : ++result.outsideMismatches;
            }
        }
    }

    return result;
}

bool allPixelsEqual(const PixelData& pixels, sf::Color color)
{
    for (std::size_t offset = 0; offset < pixels.bytes.size(); offset += 4)
    {
        if (pixels.bytes[offset + 0] != color.r || pixels.bytes[offset + 1] != color.g ||
            pixels.bytes[offset + 2] != color.b || pixels.bytes[offset + 3] != color.a)
            return false;
    }

    return true;
}

void checkEquivalent(sf::IntRect region, sf::BlendMode blendMode = sf::BlendAlpha)
{
    sf::RenderTexture regional(targetSize);
    sf::RenderTexture reference(targetSize);
    REQUIRE(regional.resize(targetSize));
    REQUIRE(reference.resize(targetSize));

    regional.clear(backgroundColor);
    reference.clear(backgroundColor);

    sf::RectangleShape shape({52.f, 38.f});
    shape.setPosition({7.f, 5.f});
    shape.setFillColor(sf::Color(220, 90, 40, 173));

    regional.drawRegion(shape, region, blendMode);
    reference.draw(shape, blendMode);

    const Diff diff = compareRegion(readPixels(regional), readPixels(reference), region);
    CHECK(diff.insideMismatches == 0);
    CHECK(diff.outsideMismatches == 0);
    CHECK(diff.maxChannelDelta == 0);
}
} // namespace

TEST_CASE("[Graphics] sf::RenderTarget::drawRegion", runDisplayTests())
{
    SECTION("Matches a full render cropped to the region")
    {
        checkEquivalent({{20, 14}, {28, 22}});
        checkEquivalent({{20, 14}, {28, 22}}, sf::BlendAdd);
        checkEquivalent({{20, 14}, {28, 22}}, sf::BlendMultiply);
    }

    SECTION("Clips regions crossing every target edge")
    {
        checkEquivalent({{-10, -8}, {30, 24}});
        checkEquivalent({{52, 31}, {40, 30}});
        checkEquivalent({{-30, 10}, {20, 12}});
        checkEquivalent({{30, -20}, {18, 15}});
        checkEquivalent({{-10, -10}, {84, 68}});
    }

    SECTION("Normalizes negative-sized regions")
    {
        checkEquivalent({{48, 36}, {-28, -22}});
        checkEquivalent({{70, 60}, {-80, -70}});
    }

    SECTION("Empty and disjoint regions leave the target untouched")
    {
        for (const sf::IntRect region : {
                 sf::IntRect{{20, 14}, {0, 22}},
                 sf::IntRect{{20, 14}, {28, 0}},
                 sf::IntRect{{-30, 14}, {20, 22}},
                 sf::IntRect{{64, 14}, {20, 22}},
                 sf::IntRect{{20, 48}, {28, 22}},
             })
        {
            sf::RenderTexture target(targetSize);
            REQUIRE(target.resize(targetSize));
            target.clear(backgroundColor);

            sf::RectangleShape shape{sf::Vector2f(targetSize)};
            shape.setFillColor(sf::Color::White);
            target.drawRegion(shape, region);

            const PixelData pixels = readPixels(target);
            for (int y = 0; y < static_cast<int>(targetSize.y); ++y)
            {
                for (int x = 0; x < static_cast<int>(targetSize.x); ++x)
                {
                    const std::size_t offset = pixelOffset(x, y);
                    CHECK(pixels.bytes[offset + 0] == backgroundColor.r);
                    CHECK(pixels.bytes[offset + 1] == backgroundColor.g);
                    CHECK(pixels.bytes[offset + 2] == backgroundColor.b);
                    CHECK(pixels.bytes[offset + 3] == backgroundColor.a);
                }
            }
        }
    }

    SECTION("Intersects the active view scissor and restores it")
    {
        sf::RenderTexture regional(targetSize);
        sf::RenderTexture reference(targetSize);
        REQUIRE(regional.resize(targetSize));
        REQUIRE(reference.resize(targetSize));
        regional.clear(backgroundColor);
        reference.clear(backgroundColor);

        sf::View view = regional.getDefaultView();
        view.setScissor({{0.25f, 0.25f}, {0.5f, 0.5f}});
        regional.setView(view);
        reference.setView(view);

        sf::RectangleShape shape{sf::Vector2f(targetSize)};
        shape.setFillColor(sf::Color(200, 100, 50, 200));

        const sf::IntRect region{{0, 0}, {48, 40}};
        regional.drawRegion(shape, region);
        reference.draw(shape);

        const Diff diff = compareRegion(readPixels(regional), readPixels(reference), region);
        CHECK(diff.insideMismatches == 0);
        CHECK(diff.outsideMismatches == 0);
        CHECK(diff.maxChannelDelta == 0);
        CHECK(regional.getView().getCenter() == view.getCenter());
        CHECK(regional.getView().getSize() == view.getSize());
        CHECK(regional.getView().getScissor() == view.getScissor());
    }

    SECTION("A region disjoint from the view scissor draws nothing")
    {
        sf::RenderTexture target(targetSize);
        REQUIRE(target.resize(targetSize));
        target.clear(backgroundColor);

        sf::View view = target.getDefaultView();
        view.setScissor({{0.25f, 0.25f}, {0.5f, 0.5f}});
        target.setView(view);

        sf::RectangleShape shape{sf::Vector2f(targetSize)};
        shape.setFillColor(sf::Color::White);
        target.drawRegion(shape, {{0, 40}, {8, 8}});

        const PixelData disjointPixels = readPixels(target);
        std::size_t     firstMismatch  = disjointPixels.bytes.size();
        for (std::size_t offset = 0; offset < disjointPixels.bytes.size(); offset += 4)
        {
            if (disjointPixels.bytes[offset + 0] != backgroundColor.r ||
                disjointPixels.bytes[offset + 1] != backgroundColor.g ||
                disjointPixels.bytes[offset + 2] != backgroundColor.b ||
                disjointPixels.bytes[offset + 3] != backgroundColor.a)
            {
                firstMismatch = offset;
                break;
            }
        }

        INFO("first mismatch byte offset " << firstMismatch);
        CHECK(allPixelsEqual(disjointPixels, backgroundColor));
    }

    SECTION("Restores the view scissor after a region draw")
    {
        sf::RenderTexture target(targetSize);
        REQUIRE(target.resize(targetSize));
        target.clear(backgroundColor);

        sf::View view = target.getDefaultView();
        view.setScissor({{0.25f, 0.25f}, {0.5f, 0.5f}});
        target.setView(view);

        sf::RectangleShape shape{sf::Vector2f(targetSize)};
        shape.setFillColor(sf::Color(200, 100, 50, 200));
        target.drawRegion(shape, {{0, 0}, {48, 40}});

        target.setView(target.getDefaultView());
        target.clear(backgroundColor);

        sf::RectangleShape fullShape{sf::Vector2f(targetSize)};
        fullShape.setFillColor(sf::Color::White);
        target.draw(fullShape);

        const PixelData afterDraw = readPixels(target);
        for (const sf::Vector2i point : {sf::Vector2i{0, 0}, sf::Vector2i{32, 24}, sf::Vector2i{63, 47}})
        {
            const std::size_t offset = pixelOffset(point.x, point.y);
            INFO("pixel " << point.x << ", " << point.y);
            CHECK(afterDraw.bytes[offset + 0] == sf::Color::White.r);
            CHECK(afterDraw.bytes[offset + 1] == sf::Color::White.g);
            CHECK(afterDraw.bytes[offset + 2] == sf::Color::White.b);
            CHECK(afterDraw.bytes[offset + 3] == sf::Color::White.a);
        }
    }

    SECTION("Raw vertices and vertex buffers support regions")
    {
        const std::array<sf::Vertex, 4> vertices = {
            sf::Vertex{{4.f, 4.f}, sf::Color(230, 80, 40, 190)},
            sf::Vertex{{60.f, 4.f}, sf::Color(80, 230, 40, 190)},
            sf::Vertex{{4.f, 44.f}, sf::Color(40, 80, 230, 190)},
            sf::Vertex{{60.f, 44.f}, sf::Color(230, 230, 40, 190)},
        };

        sf::RenderTexture regional(targetSize);
        sf::RenderTexture reference(targetSize);
        REQUIRE(regional.resize(targetSize));
        REQUIRE(reference.resize(targetSize));
        regional.clear(backgroundColor);
        reference.clear(backgroundColor);

        const sf::IntRect region{{12, 8}, {35, 27}};
        regional.drawRegion(vertices.data(), vertices.size(), sf::PrimitiveType::TriangleStrip, region);
        reference.draw(vertices.data(), vertices.size(), sf::PrimitiveType::TriangleStrip);

        const Diff diff = compareRegion(readPixels(regional), readPixels(reference), region);
        CHECK(diff.insideMismatches == 0);
        CHECK(diff.outsideMismatches == 0);
        CHECK(diff.maxChannelDelta == 0);

        if (sf::VertexBuffer::isAvailable())
        {
            sf::VertexBuffer buffer(sf::PrimitiveType::TriangleStrip, sf::VertexBuffer::Usage::Static);
            REQUIRE(buffer.create(vertices.size()));
            REQUIRE(buffer.update(vertices.data()));

            regional.clear(backgroundColor);
            regional.drawRegion(buffer, region);
            const PixelData bufferRegionalPixels = readPixels(regional);

            regional.clear(backgroundColor);
            regional.draw(buffer);
            const PixelData bufferFullPixels = readPixels(regional);

            const Diff bufferDiff = compareRegion(bufferRegionalPixels, bufferFullPixels, region);
            CHECK(bufferDiff.insideMismatches == 0);
            CHECK(bufferDiff.outsideMismatches == 0);
            CHECK(bufferDiff.maxChannelDelta == 0);

            regional.clear(backgroundColor);
            regional.drawRegion(buffer, 1, 2, region);
            const PixelData rangeRegionalPixels = readPixels(regional);

            regional.clear(backgroundColor);
            regional.draw(buffer, 1, 2);
            const PixelData rangeFullPixels = readPixels(regional);

            const Diff rangeDiff = compareRegion(rangeRegionalPixels, rangeFullPixels, region);
            CHECK(rangeDiff.insideMismatches == 0);
            CHECK(rangeDiff.outsideMismatches == 0);
            CHECK(rangeDiff.maxChannelDelta == 0);
        }
    }

    SECTION("Textured sprites match a full render crop")
    {
        sf::Image image({8, 8});
        for (std::uint32_t y = 0; y < 8; ++y)
        {
            for (std::uint32_t x = 0; x < 8; ++x)
            {
                image.setPixel({x, y}, sf::Color(static_cast<std::uint8_t>(x * 32),
                                                  static_cast<std::uint8_t>(y * 32),
                                                  static_cast<std::uint8_t>((x + y) * 16),
                                                  static_cast<std::uint8_t>(128 + (x + y) * 8)));
            }
        }
        const sf::Texture texture(image);
        sf::Sprite        sprite(texture);
        sprite.setScale({8.f, 6.f});
        sprite.setPosition({-5.f, -3.f});

        sf::RenderTexture regional(targetSize);
        sf::RenderTexture reference(targetSize);
        REQUIRE(regional.resize(targetSize));
        REQUIRE(reference.resize(targetSize));
        regional.clear(backgroundColor);
        reference.clear(backgroundColor);

        const sf::IntRect region{{17, 11}, {31, 24}};
        regional.drawRegion(sprite, region, sf::BlendAlpha);
        reference.draw(sprite, sf::BlendAlpha);

        const Diff diff = compareRegion(readPixels(regional), readPixels(reference), region);
        CHECK(diff.insideMismatches == 0);
        CHECK(diff.outsideMismatches == 0);
        CHECK(diff.maxChannelDelta == 0);
    }

    SECTION("10,000 updates match an unrestricted reference")
    {
        sf::RenderTexture regional(targetSize);
        sf::RenderTexture reference(targetSize);
        REQUIRE(regional.resize(targetSize));
        REQUIRE(reference.resize(targetSize));
        regional.clear(backgroundColor);
        reference.clear(backgroundColor);

        sf::RectangleShape shape{sf::Vector2f(targetSize)};
        shape.setFillColor(sf::Color(10, 20, 30, 3));

        const sf::IntRect region{{9, 7}, {41, 32}};
        for (int i = 0; i < 10'000; ++i)
        {
            regional.drawRegion(shape, region);
            reference.draw(shape);
        }

        const Diff diff = compareRegion(readPixels(regional), readPixels(reference), region);
        CHECK(diff.insideMismatches == 0);
        CHECK(diff.outsideMismatches == 0);
        CHECK(diff.maxChannelDelta == 0);
    }
}
