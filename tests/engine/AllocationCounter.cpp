#include "AllocationCounter.h"

#include <cstdlib>
#include <new>

// The replaceable global allocation functions, every form of them, so that
// nothing allocated by one form is freed by a form left as the library's.
// They forward to malloc and free (and their aligned forms), counting only
// while this thread is inside a CountAllocations. Plain thread_local integers
// need no allocation of their own.

namespace
{
    thread_local int countingDepth = 0;
    thread_local std::size_t allocationCount = 0;
    thread_local std::size_t freeCount = 0;

    void* allocate (std::size_t size)
    {
        if (countingDepth > 0)
            ++allocationCount;

        if (auto* memory = std::malloc (size == 0 ? 1 : size))
            return memory;

        throw std::bad_alloc();
    }

    void* allocateAligned (std::size_t size, std::align_val_t alignment)
    {
        if (countingDepth > 0)
            ++allocationCount;

#ifdef _WIN32
        if (auto* memory = _aligned_malloc (size == 0 ? 1 : size, static_cast<std::size_t> (alignment)))
            return memory;
#else
        if (auto* memory = std::aligned_alloc (static_cast<std::size_t> (alignment), size == 0 ? static_cast<std::size_t> (alignment) : size))
            return memory;
#endif

        throw std::bad_alloc();
    }

    void release (void* memory) noexcept
    {
        if (memory == nullptr)
            return;

        if (countingDepth > 0)
            ++freeCount;

        std::free (memory);
    }

    void releaseAligned (void* memory) noexcept
    {
        if (memory == nullptr)
            return;

        if (countingDepth > 0)
            ++freeCount;

#ifdef _WIN32
        _aligned_free (memory);
#else
        std::free (memory);
#endif
    }
}

void* operator new (std::size_t size) { return allocate (size); }
void* operator new[] (std::size_t size) { return allocate (size); }
void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    try { return allocate (size); } catch (...) { return nullptr; }
}
void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    try { return allocate (size); } catch (...) { return nullptr; }
}
void* operator new (std::size_t size, std::align_val_t alignment) { return allocateAligned (size, alignment); }
void* operator new[] (std::size_t size, std::align_val_t alignment) { return allocateAligned (size, alignment); }
void* operator new (std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try { return allocateAligned (size, alignment); } catch (...) { return nullptr; }
}
void* operator new[] (std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try { return allocateAligned (size, alignment); } catch (...) { return nullptr; }
}

void operator delete (void* memory) noexcept { release (memory); }
void operator delete[] (void* memory) noexcept { release (memory); }
void operator delete (void* memory, std::size_t) noexcept { release (memory); }
void operator delete[] (void* memory, std::size_t) noexcept { release (memory); }
void operator delete (void* memory, const std::nothrow_t&) noexcept { release (memory); }
void operator delete[] (void* memory, const std::nothrow_t&) noexcept { release (memory); }
void operator delete (void* memory, std::align_val_t) noexcept { releaseAligned (memory); }
void operator delete[] (void* memory, std::align_val_t) noexcept { releaseAligned (memory); }
void operator delete (void* memory, std::size_t, std::align_val_t) noexcept { releaseAligned (memory); }
void operator delete[] (void* memory, std::size_t, std::align_val_t) noexcept { releaseAligned (memory); }
void operator delete (void* memory, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned (memory); }
void operator delete[] (void* memory, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned (memory); }

namespace hocket::testing
{
CountAllocations::CountAllocations() : allocationsBefore (allocationCount), freesBefore (freeCount)
{
    ++countingDepth;
}

CountAllocations::~CountAllocations()
{
    --countingDepth;
}

std::size_t CountAllocations::allocations() const
{
    return allocationCount - allocationsBefore;
}

std::size_t CountAllocations::frees() const
{
    return freeCount - freesBefore;
}
}
