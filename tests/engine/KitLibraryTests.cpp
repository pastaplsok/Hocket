// Loading kits for the drum sampler (SPEC §8.4): on a loader thread, cached
// per sample file, delivered on the message thread, and watched while in use
// when they're the user's. Kits are made up in a non-ASCII temp folder.

#include "common/TemporaryFolder.h"
#include "common/WavWriter.h"
#include "core/Paths.h"
#include "core/kits/KitCatalog.h"
#include "engine/kits/KitLibrary.h"

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>

using namespace hocket;
using engine::KitLibrary;
using engine::PlayableKit;
using testing::WavEncoding;

namespace
{
    bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }

    std::filesystem::path path (std::string_view utf8)
    {
        return pathFromUtf8 (utf8);
    }

    void writeText (const std::filesystem::path& file, std::string_view text)
    {
        std::filesystem::create_directories (file.parent_path());
        std::ofstream out (file, std::ios::binary);
        out << text;
    }

    // n samples of a ramp from step to n * step.
    std::vector<float> ramp (std::size_t n, float step)
    {
        std::vector<float> samples (n);

        for (std::size_t i = 0; i < n; ++i)
            samples[i] = static_cast<float> (i + 1) * step;

        return samples;
    }

    std::string manifest (std::string_view kickLevel = "0dB")
    {
        return R"({
  "format": "hocket-kit",
  "formatVersion": 0,
  "description": "Made up",
  "pieces": [
    { "name": "kick", "note": 36, "file": "kick.wav", "level": ")" + std::string (kickLevel) + R"(" },
    { "name": "hat-closed", "note": 42, "file": "hats/closed.wav", "level": "-6dB" },
    { "name": "hat-open", "note": 46, "file": "hats/open.wav", "level": "-6dB" }
  ],
  "chokeGroups": [["hat-closed", "hat-open"]]
}
)";
    }

    // A kit with three 16-bit samples at `rate`.
    void writeKit (const std::filesystem::path& folder, double rate)
    {
        writeText (folder / path ("kit.json"), manifest());
        testing::writeWav (folder / path ("kick.wav"), { ramp (100, 0.005f) }, rate);
        testing::writeWav (folder / path ("hats/closed.wav"), { ramp (50, 0.01f), ramp (50, -0.01f) }, rate);
        testing::writeWav (folder / path ("hats/open.wav"), { ramp (400, 0.001f) }, rate);
    }

    // What a library delivers.
    struct Received
    {
        std::vector<std::unique_ptr<PlayableKit>> kits;

        KitLibrary::Delivery delivery()
        {
            return [this] (std::unique_ptr<PlayableKit> kit) { kits.push_back (std::move (kit)); };
        }
    };

    // A user kit and a library over it, with the notices it gives.
    struct Fixture
    {
        testing::TemporaryFolder temp { testing::unicodeName };
        std::filesystem::path userFolder = temp.path / path ("Kits");
        std::filesystem::path kitFolder = userFolder / path ("dusty");
        std::optional<kits::KitCatalog> catalog;
        std::optional<KitLibrary> library;
        std::vector<std::string> notices;

        explicit Fixture (double rate = 44100.0)
        {
            writeKit (kitFolder, rate);
            catalog.emplace (std::filesystem::path(), userFolder);
            library.emplace (*catalog);
            library->setNotices ([this] (const std::string& notice) { notices.push_back (notice); });
        }

        const kits::Kit& kit() { return *catalog->find ("user/dusty").kit; }

        // Two looks: a change is taken once the files have stopped changing.
        void lookTwice()
        {
            library->checkForChanges();
            library->checkForChanges();
            library->waitUntilIdle();
        }
    };

    float decoded16 (float value)
    {
        return static_cast<float> (std::lround (value * 32767.0)) / 32768.0f;
    }
}

TEST_CASE ("kit library: a kit loads on the loader thread and is delivered with each piece's gain and choke group")
{
    Fixture f;
    Received received;
    const auto subscription = f.library->subscribe ("user/dusty", 44100.0, received.delivery());
    CHECK (subscription);

    f.library->waitUntilIdle();
    REQUIRE (received.kits.size() == 1);
    const auto& kit = *received.kits.front();
    CHECK (kit.sampleRate() == 44100.0);

    const auto* kick = kit.pieceOn (36);
    REQUIRE (kick != nullptr);
    CHECK (kick->length == 100);
    CHECK (kick->left == kick->right); // mono
    CHECK (kick->gain == 1.0f);
    CHECK (kick->chokeGroup == -1);
    CHECK (kick->left[0] == decoded16 (0.005f));
    CHECK (kick->left[99] == decoded16 (0.5f));

    const auto* closed = kit.pieceOn (42);
    REQUIRE (closed != nullptr);
    CHECK (closed->left[49] == decoded16 (0.5f));
    CHECK (closed->right[49] == decoded16 (-0.5f));
    CHECK (closed->gain == doctest::Approx (0.501187f));
    CHECK (closed->chokeGroup == 0);
    CHECK (kit.pieceOn (46)->chokeGroup == 0);
    CHECK (kit.pieceOn (38) == nullptr);

    const auto counts = f.library->counts();
    CHECK (counts.decodes == 3);
    CHECK (counts.conversions == 0);
    CHECK (counts.deliveries == 1);
}

TEST_CASE ("kit library: samples at another rate are converted, and the cache keeps what was converted")
{
    Fixture f (48000.0);
    Received first, second, atOwnRate;

    const auto a = f.library->subscribe ("user/dusty", 44100.0, first.delivery());
    f.library->waitUntilIdle();
    REQUIRE (first.kits.size() == 1);
    CHECK (first.kits.front()->pieceOn (36)->length == 92); // 100 samples at 48 kHz, 91.875 at 44.1
    CHECK (first.kits.front()->pieceOn (46)->length == 368); // 400: 367.5
    CHECK (f.library->counts().conversions == 3);

    // Another track with the same kit at the same rate: nothing decoded or converted again.
    const auto b = f.library->subscribe ("user/dusty", 44100.0, second.delivery());
    f.library->waitUntilIdle();
    REQUIRE (second.kits.size() == 1);
    CHECK (f.library->counts().decodes == 3);
    CHECK (f.library->counts().reused == 3);
    CHECK (second.kits.front()->pieceOn (36)->left == first.kits.front()->pieceOn (36)->left); // the very same audio

    // At the samples' own rate: decoded for that rate, not converted.
    const auto c = f.library->subscribe ("user/dusty", 48000.0, atOwnRate.delivery());
    f.library->waitUntilIdle();
    CHECK (atOwnRate.kits.front()->pieceOn (36)->length == 100);
    CHECK (f.library->counts().conversions == 3);
}

TEST_CASE ("kit library: a watched kit's change is delivered: levels convert nothing, a replaced sample converts only itself")
{
    Fixture f (48000.0);
    Received received;
    const auto subscription = f.library->subscribe ("user/dusty", 44100.0, received.delivery());
    f.library->waitUntilIdle();
    REQUIRE (received.kits.size() == 1);

    // Nothing changed: nothing happens.
    f.lookTwice();
    CHECK (received.kits.size() == 1);

    // A level, in kit.json: a new kit, nothing decoded or converted.
    writeText (f.kitFolder / path ("kit.json"), manifest ("-12dB"));
    f.library->checkForChanges();
    f.library->waitUntilIdle();
    CHECK (received.kits.size() == 1); // seen once: maybe still being written

    f.lookTwice();
    REQUIRE (received.kits.size() == 2);
    CHECK (received.kits.back()->pieceOn (36)->gain == doctest::Approx (0.251189f));
    CHECK (f.library->counts().decodes == 3);
    CHECK (f.library->counts().conversions == 3);
    CHECK (f.notices.back() == "kit 'user/dusty' changed; loading it again");

    // A sample replaced: only it is decoded and converted.
    testing::writeWav (f.kitFolder / path ("hats/open.wav"), { ramp (800, 0.001f) }, 48000.0);
    f.lookTwice();
    REQUIRE (received.kits.size() == 3);
    CHECK (received.kits.back()->pieceOn (46)->length == 735);
    CHECK (f.library->counts().decodes == 4);
    CHECK (f.library->counts().conversions == 4);
    CHECK (f.library->counts().reused == 5);
}

TEST_CASE ("kit library: a change that can't be used leaves the last version playing, and says why")
{
    Fixture f;
    Received received;
    const auto subscription = f.library->subscribe ("user/dusty", 44100.0, received.delivery());
    f.library->waitUntilIdle();
    REQUIRE (received.kits.size() == 1);

    // An error in kit.json.
    writeText (f.kitFolder / path ("kit.json"), manifest ("loud"));
    f.lookTwice();
    CHECK (received.kits.size() == 1);
    CHECK (contains (f.kit().problem, "kit.json has an error: kit.json:6:"));
    CHECK (contains (f.notices.back(), "kit 'user/dusty' changed, but can't be used as it is now, so the version that loaded plays on: kit.json has an error"));

    // Fixed, but with a sample the header reader can't judge and the decoder can't read.
    writeText (f.kitFolder / path ("kit.json"), manifest());
    std::filesystem::copy_file (pathFromUtf8 (HOCKET_FIXTURES_DIR) / path ("wav") / path ("data-size-unknown.wav"), f.kitFolder / path ("kick.wav"),
                                std::filesystem::copy_options::overwrite_existing);
    f.lookTwice();
    CHECK (received.kits.size() == 1);
    CHECK (f.kit().problem.empty());
    REQUIRE (f.kit().loaded.problems.size() == 1);
    CHECK (f.kit().loaded.problems.front() == "the sample for piece 'kick', 'kick.wav', has no audio the decoder could find; the file may be cut "
                                               "short or damaged");
    CHECK (f.notices.back() == "kit 'user/dusty' changed, but a sample in it can't be played, so the version that loaded plays on: the sample for "
                               "piece 'kick', 'kick.wav', has no audio the decoder could find; the file may be cut short or damaged");

    // Fixed again: delivered, and the report is clear.
    testing::writeWav (f.kitFolder / path ("kick.wav"), { ramp (100, 0.005f) }, 44100.0);
    f.lookTwice();
    CHECK (received.kits.size() == 2);
    CHECK (f.kit().loaded.problems.empty());
}

TEST_CASE ("kit library: a first load plays the samples that decode; a kit that can't play delivers silence")
{
    Fixture f;
    std::filesystem::copy_file (pathFromUtf8 (HOCKET_FIXTURES_DIR) / path ("wav") / path ("data-size-unknown.wav"), f.kitFolder / path ("kick.wav"),
                                std::filesystem::copy_options::overwrite_existing);

    Received received, missing;
    const auto a = f.library->subscribe ("user/dusty", 44100.0, received.delivery());
    const auto b = f.library->subscribe ("acoustic", 44100.0, missing.delivery());
    f.library->waitUntilIdle();

    REQUIRE (received.kits.size() == 1);
    CHECK (received.kits.front()->pieceOn (36) == nullptr);
    CHECK (received.kits.front()->pieceOn (42) != nullptr);
    CHECK (f.kit().loaded.problems.size() == 1);

    REQUIRE (missing.kits.size() == 1);
    for (int note = 0; note < 128; ++note)
        CHECK (missing.kits.front()->pieceOn (note) == nullptr);
}

TEST_CASE ("kit library: a load overtaken by a later one is dropped, and an ended subscription gets nothing")
{
    Fixture f;
    Received received, ended;

    auto gone = f.library->subscribe ("user/dusty", 44100.0, ended.delivery());
    gone = {};

    const auto subscription = f.library->subscribe ("user/dusty", 44100.0, received.delivery());
    writeText (f.kitFolder / path ("kit.json"), manifest ("-12dB"));
    f.library->checkForChanges();
    f.library->checkForChanges(); // a second load for the same subscription, before the first is delivered
    f.library->waitUntilIdle();

    CHECK (ended.kits.empty());
    REQUIRE (received.kits.size() == 1);
    CHECK (received.kits.front()->pieceOn (36)->gain == doctest::Approx (0.251189f));

    // Moving a subscription keeps it.
    Received moved;
    auto first = f.library->subscribe ("user/dusty", 44100.0, moved.delivery());
    auto second = std::move (first);
    CHECK_FALSE (first);
    f.library->waitUntilIdle();
    CHECK (moved.kits.size() == 1);

    // A library destroyed first cuts its subscriptions off.
    f.library.reset();
    CHECK_FALSE (second);
}

TEST_CASE ("kit library: a 32-bit float sample above full scale is played as it is, and reported with its peak")
{
    Fixture f;
    testing::writeWav (f.kitFolder / path ("kick.wav"), { ramp (100, 0.015f) }, 44100.0, WavEncoding::float32); // peaks at 1.5
    testing::writeWav (f.kitFolder / path ("hats/open.wav"), { ramp (400, 0.002f) }, 44100.0, WavEncoding::pcm24);

    Received received;
    const auto subscription = f.library->subscribe ("user/dusty", 44100.0, received.delivery());
    f.library->waitUntilIdle();

    REQUIRE (received.kits.size() == 1);
    CHECK (received.kits.front()->pieceOn (36)->left[99] == 1.5f);
    CHECK (received.kits.front()->pieceOn (46)->left[399] == doctest::Approx (0.8f).epsilon (1e-6));
    REQUIRE (f.kit().loaded.overFullScale.size() == 1);
    CHECK (f.kit().loaded.overFullScale.front().piece == "kick");
    CHECK (f.kit().loaded.overFullScale.front().peak == doctest::Approx (3.52).epsilon (0.001));
}

TEST_CASE ("kit library: every WAV fixture decodes to its values, or the decoder says why not")
{
    // Each fixture as a kit's kick: channel 1's frame i is (i + 1) / 64, channel 2 its negative.
    struct Case
    {
        const char* file;
        double rate;
        float scale;     // 3 for the loud one
        float tolerance; // a 16-bit sample's step for the 16-bit ones
    };

    const Case decodable[] = {
        { "pcm16-mono.wav", 44100, 1, 1.0f / 32768 },
        { "pcm24-stereo.wav", 44100, 1, 1.0f / 8388608 },
        { "float32-stereo.wav", 44100, 1, 0 },
        { "float32-loud.wav", 44100, 3, 0 },
        { "extensible-pcm24.wav", 44100, 1, 1.0f / 8388608 },
        { "extensible-pcm24-in-32.wav", 44100, 1, 1.0f / 8388608 },
        { "extensible-float32-48k.wav", 48000, 1, 0 },
        { "chunks-around.wav", 44100, 1, 1.0f / 32768 },
    };

    for (const auto& c : decodable)
    {
        INFO (std::string (c.file));
        Fixture f;
        std::filesystem::copy_file (pathFromUtf8 (HOCKET_FIXTURES_DIR) / path ("wav") / path (c.file), f.kitFolder / path ("kick.wav"),
                                    std::filesystem::copy_options::overwrite_existing);
        Received received;
        const auto subscription = f.library->subscribe ("user/dusty", c.rate, received.delivery());
        f.library->waitUntilIdle();

        REQUIRE (received.kits.size() == 1);
        const auto* kick = received.kits.front()->pieceOn (36);
        INFO ((f.kit().loaded.problems.empty() ? std::string ("no problems") : f.kit().loaded.problems.front()));
        REQUIRE (kick != nullptr);
        CHECK (f.kit().loaded.problems.empty());
        REQUIRE (kick->length == 32);

        for (std::size_t i = 0; i < 32; ++i)
        {
            const auto expected = static_cast<float> (i + 1) / 64.0f * c.scale;
            CHECK (std::abs (kick->left[i] - expected) <= c.tolerance + 1e-7f);

            if (kick->right != kick->left)
                CHECK (std::abs (kick->right[i] + expected) <= c.tolerance + 1e-7f);
        }
    }

    // Refused by the header reader (so the kit can't be used), and what the
    // header reader leaves to the decoder.
    struct Refused
    {
        const char* file;
        std::string_view problem;
    };

    for (const auto& [file, problem] : { Refused { "pcm8-mono.wav", "kit 'user/dusty' can't be used: " },
                                         Refused { "three-channels.wav", "kit 'user/dusty' can't be used: " },
                                         Refused { "data-size-unknown.wav", "has no audio the decoder could find" },
                                         Refused { "rf64-pcm16-mono.wav", "has no audio the decoder could find" } })
    {
        INFO (std::string (file));
        Fixture f;
        std::filesystem::copy_file (pathFromUtf8 (HOCKET_FIXTURES_DIR) / path ("wav") / path (file), f.kitFolder / path ("kick.wav"),
                                    std::filesystem::copy_options::overwrite_existing);
        f.library.reset(); // read again, with the new kick
        f.catalog.emplace (std::filesystem::path(), f.userFolder);
        f.library.emplace (*f.catalog);

        Received received;
        const auto subscription = f.library->subscribe ("user/dusty", 44100.0, received.delivery());
        f.library->waitUntilIdle();
        REQUIRE (received.kits.size() == 1);
        CHECK (received.kits.front()->pieceOn (36) == nullptr);

        const auto found = f.catalog->find ("user/dusty");
        const auto said = found.kit == nullptr ? found.problem : found.kit->loaded.problems.empty() ? std::string() : found.kit->loaded.problems.front();
        CHECK_MESSAGE (contains (said, problem), said);
    }
}

TEST_CASE ("kit library: with the message loop running, kits arrive by themselves, and the timer picks up changes")
{
    Fixture f;
    Received received;
    const auto subscription = f.library->subscribe ("user/dusty", 44100.0, received.delivery());

    const auto runUntil = [&received] (std::size_t count)
    {
        for (int i = 0; i < 300 && received.kits.size() < count; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    };

    runUntil (1);
    CHECK (received.kits.size() == 1);

    writeText (f.kitFolder / path ("kit.json"), manifest ("-12dB"));
    runUntil (2); // two of the timer's looks, half a second apart
    CHECK (received.kits.size() == 2);
}
