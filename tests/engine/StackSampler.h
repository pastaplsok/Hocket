#pragma once

// Samples the calling thread's stack from another thread, for finding where
// it waits (PROGRESS.md, Loose ends: the slow play()). Windows only; elsewhere
// it samples nothing. For function names and lines, build with symbols: the
// vs2026-symbols preset.

#include <memory>
#include <string>

namespace hocket::testing
{
    class StackSampler
    {
    public:
        // Starts sampling the thread that creates it, every intervalMs.
        explicit StackSampler (int intervalMs = 100);
        ~StackSampler();

        StackSampler (const StackSampler&) = delete;
        StackSampler& operator= (const StackSampler&) = delete;

        // Stops, and describes what was sampled: each distinct stack, innermost
        // frame first, with how many samples had it.
        std::string stop();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
