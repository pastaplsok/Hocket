#pragma once

// Helpers for the command bus's tests.

#include "core/commands/CommandBus.h"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hocket::testing
{
    inline notes::Clip parsedNotes (std::string_view text, notes::Meter meter = {}, const notes::DrumMap* drumMap = nullptr)
    {
        auto result = notes::parse (text, meter, drumMap);

        for (const auto& error : result.errors)
            MESSAGE (notes::formatDiagnostic (error, "notes"));

        REQUIRE (result.clip);
        return std::move (*result.clip);
    }

    // "NotesChanged clips/a.notes", "TempoChanged", "ClipRemoved clp-a from trk-bass"
    inline std::string describe (const commands::Change& change)
    {
        using namespace commands;

        struct Describer
        {
            std::string operator() (const NotesChanged& c) const { return "NotesChanged " + c.file; }
            std::string operator() (const TempoChanged&) const { return "TempoChanged"; }
            std::string operator() (const TimeSignatureChanged&) const { return "TimeSignatureChanged"; }
            std::string operator() (const TrackAdded& c) const { return "TrackAdded " + c.trackId; }
            std::string operator() (const TrackRemoved& c) const { return "TrackRemoved " + c.trackId; }
            std::string operator() (const TrackMoved& c) const { return "TrackMoved " + c.trackId; }
            std::string operator() (const TrackRenamed& c) const { return "TrackRenamed " + c.trackId; }
            std::string operator() (const MixChanged& c) const { return "MixChanged " + c.trackId; }
            std::string operator() (const InstrumentChanged& c) const { return "InstrumentChanged " + c.trackId; }
            std::string operator() (const DrumMapChanged& c) const { return "DrumMapChanged " + c.trackId; }
            std::string operator() (const ClipAdded& c) const { return "ClipAdded " + c.clipId; }
            std::string operator() (const ClipRemoved& c) const { return "ClipRemoved " + c.clipId + " from " + c.trackId; }
            std::string operator() (const ClipMoved& c) const { return "ClipMoved " + c.clipId; }
            std::string operator() (const ClipFileChanged& c) const { return "ClipFileChanged " + c.clipId; }
        };

        return std::visit (Describer {}, change);
    }

    // Records every change the bus announces, described.
    struct ChangeLog
    {
        explicit ChangeLog (commands::CommandBus& bus)
        {
            bus.addListener ([this] (const commands::Change& change) { changes.push_back (describe (change)); });
        }

        // The changes since the last call.
        std::vector<std::string> take() { return std::exchange (changes, {}); }

        std::vector<std::string> changes;
    };

    // Writes the project's files and reads them back: they must be valid,
    // and write out the same again.
    inline void checkReadsBack (const project::Project& project)
    {
        const auto files = project::write (project);
        const auto result = project::parse (files.at ("project.json"), [&files] (const std::string& path) -> std::optional<std::string>
        {
            const auto found = files.find (path);
            return found == files.end() ? std::nullopt : std::optional<std::string> (found->second);
        });

        for (const auto& error : result.errors)
            MESSAGE (project::formatDiagnostic (error));

        REQUIRE (result.project);
        CHECK (project::write (*result.project) == files);
    }

    inline bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }
}
