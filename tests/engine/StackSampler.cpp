#include "StackSampler.h"

#ifdef _WIN32

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

namespace hocket::testing
{
namespace
{
    constexpr int maxFrames = 48;
    using Frames = std::vector<DWORD64>;

    // Walks a suspended thread's stack with the OS's own unwind data. Nothing
    // here allocates or takes a lock the suspended thread might hold, which
    // dbghelp's StackWalk64 can't promise.
    Frames walk (HANDLE thread)
    {
        CONTEXT context {};
        context.ContextFlags = CONTEXT_FULL;

        if (! GetThreadContext (thread, &context))
            return {};

        std::array<DWORD64, maxFrames> frames {};
        int count = 0;

        while (count < maxFrames && context.Rip != 0)
        {
            frames[static_cast<std::size_t> (count++)] = context.Rip;
            DWORD64 imageBase = 0;
            auto* function = RtlLookupFunctionEntry (context.Rip, &imageBase, nullptr);

            if (function == nullptr)
            {
                // A leaf function: the return address is at the top of the stack.
                context.Rip = *reinterpret_cast<DWORD64*> (context.Rsp);
                context.Rsp += 8;
                continue;
            }

            void* handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind (UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisherFrame, nullptr);
        }

        return { frames.begin(), frames.begin() + count };
    }

    std::string describe (HANDLE process, DWORD64 address)
    {
        alignas (SYMBOL_INFO) char buffer[sizeof (SYMBOL_INFO) + MAX_SYM_NAME] {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*> (buffer);
        symbol->SizeOfStruct = sizeof (SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        std::ostringstream text;
        IMAGEHLP_MODULE64 module {};
        module.SizeOfStruct = sizeof (module);

        if (SymGetModuleInfo64 (process, address, &module))
            text << module.ModuleName << "!";

        DWORD64 displacement = 0;

        if (SymFromAddr (process, address, &displacement, symbol))
            text << symbol->Name << "+0x" << std::hex << displacement << std::dec;
        else
            text << "0x" << std::hex << address << std::dec;

        IMAGEHLP_LINE64 line {};
        line.SizeOfStruct = sizeof (line);
        DWORD lineDisplacement = 0;

        if (SymGetLineFromAddr64 (process, address, &lineDisplacement, &line))
            text << "  (" << line.FileName << ":" << line.LineNumber << ")";

        return text.str();
    }
}

struct StackSampler::Impl
{
    HANDLE target = nullptr;
    std::atomic<bool> running { true };
    std::vector<Frames> samples;
    std::thread sampler;

    explicit Impl (int intervalMs)
    {
        DuplicateHandle (GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        samples.reserve (1000);

        sampler = std::thread ([this, intervalMs]
        {
            while (running.load())
            {
                std::this_thread::sleep_for (std::chrono::milliseconds (intervalMs));

                if (! running.load() || SuspendThread (target) == static_cast<DWORD> (-1))
                    break;

                auto frames = walk (target);
                ResumeThread (target);

                if (samples.size() < samples.capacity())
                    samples.push_back (std::move (frames));
            }
        });
    }

    ~Impl()
    {
        running.store (false);

        if (sampler.joinable())
            sampler.join();

        if (target != nullptr)
            CloseHandle (target);
    }
};

StackSampler::StackSampler (int intervalMs) : impl (std::make_unique<Impl> (intervalMs))
{
}

StackSampler::~StackSampler() = default;

std::string StackSampler::stop()
{
    impl->running.store (false);

    if (impl->sampler.joinable())
        impl->sampler.join();

    // Names come from the PDBs next to the program, and Windows' own exports.
    const auto process = GetCurrentProcess();
    SymSetOptions (SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    const auto symbols = SymInitialize (process, nullptr, TRUE);

    std::map<Frames, int> counts;

    for (const auto& frames : impl->samples)
        ++counts[frames];

    std::vector<std::pair<int, const Frames*>> byCount;

    for (const auto& [frames, count] : counts)
        byCount.emplace_back (count, &frames);

    std::ranges::sort (byCount, [] (const auto& a, const auto& b) { return a.first > b.first; });

    std::ostringstream text;
    text << impl->samples.size() << " samples of the message thread, " << counts.size() << " distinct stacks"
         << (symbols ? "" : " (no symbols could be loaded)") << ":\n";

    for (const auto& [count, frames] : byCount)
    {
        text << "\n" << count << " sample(s):\n";

        for (const auto address : *frames)
            text << "    " << describe (process, address) << "\n";
    }

    if (symbols)
        SymCleanup (process);

    return text.str();
}
}

#else

namespace hocket::testing
{
struct StackSampler::Impl
{
};

StackSampler::StackSampler (int) {}
StackSampler::~StackSampler() = default;

std::string StackSampler::stop()
{
    return "stack sampling is Windows only\n";
}
}

#endif
