#pragma once

// Counting heap allocations, to check that real-time code makes none (CLAUDE.md).
// The engine tests replace the global operator new and delete
// (AllocationCounter.cpp); they count only on a thread inside a CountAllocations
// scope, so everything else (Tracktion, the tests themselves) is untouched.

#include <cstddef>

namespace hocket::testing
{
    // Counts the allocations and frees made on this thread while it's alive.
    class CountAllocations
    {
    public:
        CountAllocations();
        ~CountAllocations();

        CountAllocations (const CountAllocations&) = delete;
        CountAllocations& operator= (const CountAllocations&) = delete;

        std::size_t allocations() const;
        std::size_t frees() const;

    private:
        std::size_t allocationsBefore;
        std::size_t freesBefore;
    };
}
