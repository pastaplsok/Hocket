#pragma once

// Loads the kits Hocket's drum samplers play (SPEC §8.1, §8.4), on a loader
// thread of its own, so the message thread never waits on decoding or
// sample-rate conversion (it would freeze the window, Milestone 2).
//
// - A subscription asks for a kit at a playback rate; each time the kit is
//   ready, its delivery gets a PlayableKit, on the message thread.
// - Converted audio is cached per sample file, by its path, size, modified
//   time and the rate it was converted to, so a change to a kit's levels
//   converts nothing, and a replaced sample converts only itself.
// - The user's kits in use are watched, every half second and when asked
//   (as play starts): a saved change is delivered once the files stop
//   changing, even one saved after the catalog read the kit but before it
//   was first used. A change that can't be used delivers nothing, so the
//   last version plays on, and the catalog keeps the reason for the overview.
// - A kit that couldn't play when asked for (not installed, or broken) is
//   looked for again at each look, and loaded once it's there and its files
//   have stopped changing.
// - Nothing that goes wrong on the loader thread ends the process: a kit that
//   can't be loaded plays nothing, as if missing, with the reason in the
//   catalog's load report.
// - The cache keeps only samples a kit still uses.
//
// Message thread only, but for the loader thread inside. It must outlive its
// subscriptions' use: the subscriptions it outlives are cut off.

#include "core/kits/KitCatalog.h"
#include "engine/devices/PlayableKit.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hocket::engine
{
    class KitLibrary final : private juce::Timer,
                             private juce::AsyncUpdater
    {
    public:
        explicit KitLibrary (kits::KitCatalog& catalog);
        ~KitLibrary() override;

        KitLibrary (const KitLibrary&) = delete;
        KitLibrary& operator= (const KitLibrary&) = delete;

        using Delivery = std::function<void (std::unique_ptr<PlayableKit>)>;

        // A kit asked for at a rate, kept while this lives. Moving it keeps it;
        // destroying it drops the request and any delivery still to come.
        class Subscription
        {
        public:
            Subscription() = default;
            Subscription (Subscription&& other) noexcept;
            Subscription& operator= (Subscription&& other) noexcept;
            ~Subscription();

            explicit operator bool() const { return library != nullptr; }

        private:
            friend class KitLibrary;
            Subscription (KitLibrary& owner, std::uint64_t subscriptionId);
            void release();

            KitLibrary* library = nullptr;
            std::uint64_t id = 0;
        };

        // Loads `kit` at `rate`, and delivers it when it's ready: then again
        // whenever a watched kit's files change. A kit that can't be played
        // (not installed, broken) delivers a silent kit. Problems that only
        // decoding finds go to the catalog's load report; if a piece's sample
        // can't be decoded, the kit plays without it the first time, and a
        // change that makes one undecodable is never delivered.
        [[nodiscard]] Subscription subscribe (std::string kit, double rate, Delivery deliver);

        // Blocks until everything asked for has been loaded, then delivers
        // it: for --play's start, renders and tests.
        void waitUntilIdle();

        // The same, but gives up after `timeout`, and says whether it got
        // there: for --play's start, to notice a Ctrl+C while kits load. A
        // library destroyed meanwhile lets the kit being built finish, then
        // drops it.
        bool waitUntilIdle (std::chrono::milliseconds timeout);

        // Looks at the watched kits' files now, as the timer does: a change
        // seen on two looks in a row (so the files have stopped changing) is
        // read and loaded.
        void checkForChanges();

        // Where to say what happens to watched kits, for --play to print:
        // "kit 'user/x' changed, and is loading" and the like.
        void setNotices (std::function<void (const std::string&)> notices);

        // What the loader has done, for tests.
        struct Counts
        {
            std::uint64_t decodes = 0;     // samples read from disk
            std::uint64_t conversions = 0; // samples converted to another rate
            std::uint64_t reused = 0;      // samples found in the cache
            std::uint64_t deliveries = 0;
            std::uint64_t cached = 0;      // samples in the cache after the latest load
        };

        Counts counts() const;

        static constexpr int watchIntervalMs = 500;

    private:
        struct Job
        {
            std::uint64_t id = 0;
            std::uint64_t subscription = 0;
            std::string kitName;
            double rate = 0.0;
            std::filesystem::path folder; // empty for a kit that can't play: a silent kit
            kits::Manifest manifest;
        };

        struct Result
        {
            std::uint64_t job = 0;
            std::uint64_t subscription = 0;
            std::string kitName;
            std::unique_ptr<PlayableKit> kit;
            kits::LoadReport report;
            bool complete = true; // every piece's sample decoded
        };

        struct Entry
        {
            std::string kitName;
            double rate = 0.0;
            Delivery deliver;
            Subscription* handle = nullptr;
            std::uint64_t latestJob = 0;
            bool delivered = false;
            bool missing = false; // the catalog had no kit to play: a silent one was asked for
        };

        // A watched kit's files: as last loaded, and as last seen.
        struct Watch
        {
            std::vector<kits::FileStamp> loaded;
            std::vector<kits::FileStamp> seen;
        };

        struct CachedSample
        {
            std::shared_ptr<const SampleAudio> audio;
            double peak = 0.0; // the loudest sample's magnitude, 1 at full scale
        };

        kits::KitCatalog& catalog;
        std::map<std::uint64_t, Entry> entries; // by subscription
        std::map<std::string, Watch, std::less<>> watches; // user kits in use, by name

        // Kits missing when asked for, found again on the latest look: their
        // files' stamps then, to see they've stopped changing.
        std::map<std::string, std::vector<kits::FileStamp>, std::less<>> appearing;
        std::function<void (const std::string&)> notify;
        std::uint64_t nextSubscription = 1;
        std::uint64_t nextJob = 1;

        // Shared with the loader thread, under `mutex`.
        mutable std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable idle;
        std::deque<Job> queue;
        std::vector<Result> done;
        bool busy = false;
        bool stopping = false;
        Counts loaderCounts;

        // The loader thread's alone.
        std::map<std::string, CachedSample> cache; // by key: path, size, modified time, rate

        std::thread loader;

        void load (std::uint64_t subscription);
        void unsubscribe (std::uint64_t subscription);
        void deliverDone();
        void loaderLoop();
        Result build (const Job& job);

        // What a job delivers when building it threw: a silent kit, and why.
        static Result failed (const Job& job, const std::string& why);

        // The missing kits in use that have appeared, and stopped changing.
        void lookForMissingKits();

        void handleAsyncUpdate() override;
        void timerCallback() override;

        // A drum sampler holds a weak reference, so it knows when this has gone.
        JUCE_DECLARE_WEAK_REFERENCEABLE (KitLibrary)
    };
}
