#include "engine/kits/KitLibrary.h"

#include "core/Paths.h"
#include "core/kits/WavHeader.h"
#include "engine/kits/SampleRateConversion.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <optional>

namespace hocket::engine
{
namespace
{
    std::string inQuotes (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    // A sample read from its WAV file, at the file's own rate.
    struct Decoded
    {
        std::vector<std::vector<float>> channels;
        double rate = 0.0;
    };

    // The sample in `file`, or why it can't be played. The catalog has checked
    // what the header says, when it could read it; this is the final word.
    std::optional<Decoded> decode (const std::filesystem::path& file, std::string& problem)
    {
        auto stream = std::make_unique<juce::FileInputStream> (juce::File (juce::String::fromUTF8 (utf8FromPath (file).c_str())));

        if (! stream->openedOk())
        {
            problem = "couldn't be opened";
            return {};
        }

        juce::WavAudioFormat format;
        const std::unique_ptr<juce::AudioFormatReader> reader (format.createReaderFor (stream.release(), true));

        if (reader == nullptr)
        {
            problem = "couldn't be read as a WAV file";
            return {};
        }

        // The header reader holds kits to 16- or 24-bit PCM and 32-bit float
        // (sampleFormatProblem), going by the bits that carry the sample. JUCE
        // gives the container's size instead, so 24 bits in 32-bit containers
        // (an extensible header's) read as 32: let those through here.
        const auto bits = static_cast<int> (reader->bitsPerSample);

        if (reader->usesFloatingPointData ? bits != 32 : bits != 16 && bits != 24 && bits != 32)
        {
            problem = "is " + std::to_string (bits) + (reader->usesFloatingPointData ? "-bit float" : "-bit PCM")
                    + "; kits' samples are 16- or 24-bit PCM, or 32-bit float";
            return {};
        }

        if (reader->numChannels < 1 || reader->numChannels > 2)
        {
            problem = "has " + std::to_string (reader->numChannels) + " channels; a kit's samples are mono or stereo";
            return {};
        }

        // As the header reader holds them, for a header it couldn't read.
        if (const auto rateProblem = kits::sampleRateProblem (reader->sampleRate))
        {
            problem = "is" + rateProblem->substr (std::string_view ("it's").size());
            return {};
        }

        if (reader->lengthInSamples <= 0 || static_cast<double> (reader->lengthInSamples) / reader->sampleRate > kits::maxSampleSeconds)
        {
            // JUCE opens a file whose data chunk claims more than the file
            // holds, and finds no audio in it.
            problem = reader->lengthInSamples <= 0 ? std::string ("has no audio the decoder could find; the file may be cut short or damaged")
                                                   : "is longer than 10 s; a kit's samples are one-shots of at most 10 s";
            return {};
        }

        const auto length = static_cast<int> (reader->lengthInSamples);
        juce::AudioBuffer<float> buffer (static_cast<int> (reader->numChannels), length);

        if (! reader->read (&buffer, 0, length, 0, true, true))
        {
            problem = "couldn't be read to the end";
            return {};
        }

        Decoded decoded;
        decoded.rate = reader->sampleRate;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            decoded.channels.emplace_back (buffer.getReadPointer (channel), buffer.getReadPointer (channel) + length);

        return decoded;
    }

    std::string cacheKey (const std::filesystem::path& file, const kits::FileStamp& stamp, double rate)
    {
        return utf8FromPath (file) + "|" + std::to_string (stamp.size) + "|" + std::to_string (stamp.modified) + "|" + std::to_string (rate);
    }
}

//==============================================================================
KitLibrary::Subscription::Subscription (KitLibrary& owner, std::uint64_t subscriptionId) : library (&owner), id (subscriptionId)
{
    library->entries.at (id).handle = this;
}

KitLibrary::Subscription::Subscription (Subscription&& other) noexcept : library (other.library), id (other.id)
{
    other.library = nullptr;

    if (library != nullptr)
        library->entries.at (id).handle = this;
}

KitLibrary::Subscription& KitLibrary::Subscription::operator= (Subscription&& other) noexcept
{
    if (this != &other)
    {
        release();
        library = other.library;
        id = other.id;
        other.library = nullptr;

        if (library != nullptr)
            library->entries.at (id).handle = this;
    }

    return *this;
}

KitLibrary::Subscription::~Subscription()
{
    release();
}

void KitLibrary::Subscription::release()
{
    if (library != nullptr)
        std::exchange (library, nullptr)->unsubscribe (id);
}

//==============================================================================
KitLibrary::KitLibrary (kits::KitCatalog& kitCatalog) : catalog (kitCatalog)
{
    loader = std::thread ([this] { loaderLoop(); });
    startTimer (watchIntervalMs);
}

KitLibrary::~KitLibrary()
{
    stopTimer();

    {
        const std::scoped_lock lock (mutex);
        stopping = true;
    }

    wake.notify_all();
    loader.join();
    cancelPendingUpdate();

    // Subscriptions that outlive this are cut off.
    for (auto& [id, entry] : entries)
        if (entry.handle != nullptr)
            entry.handle->library = nullptr;
}

KitLibrary::Subscription KitLibrary::subscribe (std::string kit, double rate, Delivery deliver)
{
    const auto id = nextSubscription++;
    entries.emplace (id, Entry { std::move (kit), rate, std::move (deliver) });
    Subscription subscription (*this, id);
    load (id);
    return subscription;
}

void KitLibrary::unsubscribe (std::uint64_t subscription)
{
    const auto found = entries.find (subscription);

    if (found == entries.end())
        return;

    const auto kitName = found->second.kitName;
    entries.erase (found);

    // A kit no one plays any more isn't watched.
    if (std::ranges::none_of (entries, [&kitName] (const auto& entry) { return entry.second.kitName == kitName; }))
        watches.erase (kitName);
}

void KitLibrary::load (std::uint64_t subscription)
{
    auto& entry = entries.at (subscription);
    Job job { nextJob++, subscription, entry.kitName, entry.rate };
    entry.missing = true;

    if (const auto found = catalog.find (entry.kitName); found.kit != nullptr && ! found.kit->folder.empty())
    {
        job.folder = found.kit->folder;
        job.manifest = found.kit->manifest;
        entry.missing = false;

        // The user's kits are watched for changes; bundled ones never change
        // (SPEC §8.4). From the files as the catalog read them, so a change
        // saved since is seen; a kit already watched is left as it is, so a
        // change it hasn't taken in yet still reaches the tracks playing it.
        if (found.kit->source == kits::Source::user && ! watches.contains (entry.kitName))
        {
            auto& watch = watches[entry.kitName];
            watch.loaded = watch.seen = found.kit->stamps.empty() ? kits::stampsOf (*found.kit) : found.kit->stamps;
        }
    }

    entry.latestJob = job.id;

    {
        const std::scoped_lock lock (mutex);
        queue.push_back (std::move (job));
    }

    wake.notify_one();
}

void KitLibrary::waitUntilIdle()
{
    {
        std::unique_lock lock (mutex);
        idle.wait (lock, [this] { return queue.empty() && ! busy; });
    }

    deliverDone();
}

bool KitLibrary::waitUntilIdle (std::chrono::milliseconds timeout)
{
    {
        std::unique_lock lock (mutex);

        if (! idle.wait_for (lock, timeout, [this] { return queue.empty() && ! busy; }))
            return false;
    }

    deliverDone();
    return true;
}

void KitLibrary::setNotices (std::function<void (const std::string&)> notices)
{
    notify = std::move (notices);
}

KitLibrary::Counts KitLibrary::counts() const
{
    const std::scoped_lock lock (mutex);
    return loaderCounts;
}

void KitLibrary::checkForChanges()
{
    std::vector<std::string> changed;

    for (auto& [name, watch] : watches)
    {
        const auto found = catalog.find (name);

        if (found.kit == nullptr)
            continue;

        const auto now = kits::stampsOf (*found.kit);

        // Changed since it loaded, and the same on two looks in a row: the files have settled.
        if (now != watch.loaded && now == watch.seen)
            changed.push_back (name);

        watch.seen = now;
    }

    for (const auto& name : changed)
    {
        const auto* kit = catalog.refresh (name).kit;
        auto& watch = watches[name];
        watch.seen = kits::stampsOf (*kit);

        // As the catalog read them, so a change saved straight after is
        // taken in at the next looks; as they are now when they can't be used,
        // so it isn't tried again until they change.
        watch.loaded = kit->problem.empty() && ! kit->stamps.empty() ? kit->stamps : watch.seen;

        if (! kit->problem.empty())
        {
            if (notify)
                notify ("kit " + inQuotes (name) + " changed, but can't be used as it is now, so the version that loaded plays on: " + kit->problem);

            continue;
        }

        if (notify)
            notify ("kit " + inQuotes (name) + " changed; loading it again");

        for (const auto& [id, entry] : entries)
            if (entry.kitName == name)
                load (id);
    }

    lookForMissingKits();
}

void KitLibrary::lookForMissingKits()
{
    std::vector<std::string> names;

    for (const auto& [id, entry] : entries)
        if (entry.missing && std::ranges::find (names, entry.kitName) == names.end())
            names.push_back (entry.kitName);

    std::erase_if (appearing, [&names] (const auto& kit) { return std::ranges::find (names, kit.first) == names.end(); });

    for (const auto& name : names)
    {
        // Lists the kit folders again while it isn't there.
        const auto found = catalog.find (name);

        if (found.kit == nullptr || found.kit->folder.empty())
        {
            appearing.erase (name);
            continue;
        }

        // There, and the same on two looks in a row: its files have settled.
        const auto now = kits::stampsOf (*found.kit);

        if (const auto seen = appearing.find (name); seen == appearing.end() || seen->second != now)
        {
            appearing.insert_or_assign (name, now);
            continue;
        }

        appearing.erase (name);

        // The files as they are now, which the catalog may have read a moment ago.
        if (const auto* kit = catalog.refresh (name).kit; kit == nullptr || ! kit->problem.empty())
            continue;

        if (notify)
            notify ("kit " + inQuotes (name) + " is there now; loading it");

        for (const auto& [id, entry] : entries)
            if (entry.missing && entry.kitName == name)
                load (id);
    }
}

//==============================================================================
void KitLibrary::deliverDone()
{
    std::vector<Result> results;

    {
        const std::scoped_lock lock (mutex);
        results.swap (done);
    }

    for (auto& result : results)
    {
        const auto found = entries.find (result.subscription);

        // Overtaken by a later load, or no longer wanted.
        if (found == entries.end() || found->second.latestJob != result.job)
            continue;

        catalog.setLoadReport (result.kitName, result.report);

        if (! result.complete && found->second.delivered)
        {
            if (notify)
                notify ("kit " + inQuotes (result.kitName) + " changed, but a sample in it can't be played, so the version that loaded plays on: "
                        + result.report.problems.front());

            continue;
        }

        found->second.delivered = true;

        {
            const std::scoped_lock lock (mutex);
            ++loaderCounts.deliveries;
        }

        // A copy: the delivery may end its own subscription.
        const auto deliver = found->second.deliver;
        deliver (std::move (result.kit));
    }
}

void KitLibrary::handleAsyncUpdate()
{
    deliverDone();
}

void KitLibrary::timerCallback()
{
    checkForChanges();
}

//==============================================================================
void KitLibrary::loaderLoop()
{
    for (;;)
    {
        Job job;

        {
            std::unique_lock lock (mutex);
            wake.wait (lock, [this] { return stopping || ! queue.empty(); });

            if (stopping)
                return;

            job = std::move (queue.front());
            queue.pop_front();
            busy = true;
        }

        // Nothing that goes wrong here may end the process: the kit can't be
        // loaded, and plays nothing, saying why.
        Result result;

        try
        {
            result = build (job);
        }
        catch (const std::exception& e)
        {
            result = failed (job, e.what());
        }
        catch (...)
        {
            result = failed (job, "something went wrong");
        }

        // What no kit uses any more: a sample replaced since, or a kit no
        // track plays. Only this thread copies from the cache, so a count of
        // one is the cache's alone, and stays so.
        std::erase_if (cache, [] (const auto& entry) { return entry.second.audio.use_count() == 1; });

        {
            const std::scoped_lock lock (mutex);
            done.push_back (std::move (result));
            loaderCounts.cached = cache.size();
            busy = false;
        }

        idle.notify_all();
        triggerAsyncUpdate();
    }
}

KitLibrary::Result KitLibrary::failed (const Job& job, const std::string& why)
{
    Result result { job.id, job.subscription, job.kitName };
    result.complete = false;
    result.report.failed = why;
    result.report.problems.push_back ("it couldn't be loaded: " + why);

    try
    {
        result.kit = PlayableKit::silent (job.rate);
    }
    catch (...)
    {
        // At a rate no kit can have, none: a sampler takes only kits at its rate.
    }

    return result;
}

KitLibrary::Result KitLibrary::build (const Job& job)
{
    Result result { job.id, job.subscription, job.kitName };
    std::vector<PlayableKit::Piece> pieces;
    Counts counted;

    for (const auto& piece : job.manifest.pieces)
    {
        const auto file = job.folder / pathFromUtf8 (piece.file);
        const auto stamp = kits::stampOf (file);
        const auto key = cacheKey (file, stamp, job.rate);
        CachedSample sample;

        if (const auto hit = cache.find (key); hit != cache.end())
        {
            sample = hit->second;
            ++counted.reused;
        }
        else
        {
            std::string problem;
            auto decoded = decode (file, problem);

            if (! decoded)
            {
                result.complete = false;
                result.report.problems.push_back ("the sample for piece " + inQuotes (piece.name) + ", " + inQuotes (piece.file) + ", " + problem);
                continue;
            }

            ++counted.decodes;
            auto audio = std::make_shared<SampleAudio>();

            for (auto& channel : decoded->channels)
            {
                for (const auto value : channel)
                    sample.peak = std::max (sample.peak, static_cast<double> (std::abs (value)));

                audio->channels.push_back (decoded->rate == job.rate ? std::move (channel) : convertSampleRate (channel, decoded->rate, job.rate));
            }

            if (decoded->rate != job.rate)
                ++counted.conversions;

            sample.audio = std::move (audio);
            cache.insert_or_assign (key, sample);
        }

        if (sample.peak > 1.0)
            result.report.overFullScale.push_back ({ piece.name, 20.0 * std::log10 (sample.peak) });

        pieces.push_back ({ piece.note, sample.audio, static_cast<float> (std::pow (10.0, piece.level / 20.0)), job.manifest.chokeGroupOf (piece.name) });
    }

    try
    {
        result.kit = std::make_unique<PlayableKit> (job.rate, std::move (pieces));
    }
    catch (const std::exception& e)
    {
        result.complete = false;
        result.report.problems.push_back (std::string ("it couldn't be put together: ") + e.what());
        result.kit = PlayableKit::silent (job.rate);
    }

    const std::scoped_lock lock (mutex);
    loaderCounts.decodes += counted.decodes;
    loaderCounts.conversions += counted.conversions;
    loaderCounts.reused += counted.reused;
    return result;
}
}
