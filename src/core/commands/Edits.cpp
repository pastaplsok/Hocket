#include "core/commands/Edits.h"

#include <algorithm>
#include <stdexcept>

namespace hocket::commands::edits
{
namespace
{
    // Edits are made by validation for the project as it is, so what they
    // name is there. If it isn't, that's a bug in the bus: better an exception,
    // which the API reports as an internal error, than undefined behaviour.
    [[noreturn]] void missing (std::string_view what, std::string_view id)
    {
        throw std::logic_error ("command bus: an edit names " + std::string (what) + " '" + std::string (id) + "', which isn't in the project");
    }

    std::vector<project::Track>::iterator locateTrack (project::Project& project, std::string_view id)
    {
        const auto found = std::ranges::find (project.tracks, id, &project::Track::id);

        if (found == project.tracks.end())
            missing ("track", id);

        return found;
    }

    struct ClipPlace
    {
        project::Track* track = nullptr;
        std::size_t index = 0;
    };

    ClipPlace locateClip (project::Project& project, std::string_view id)
    {
        for (auto& track : project.tracks)
            for (std::size_t i = 0; i < track.clips.size(); ++i)
                if (track.clips[i].id == id)
                    return { &track, i };

        missing ("clip", id);
    }

    void requireFile (const project::Project& project, std::string_view file)
    {
        if (! project.notesFiles.contains (file))
            missing ("clip file", file);
    }

    template <typename Container>
    auto placeAt (Container& container, std::size_t index)
    {
        return container.begin() + static_cast<std::ptrdiff_t> (std::min (index, container.size()));
    }

    //==============================================================================
    Edit applyEdit (project::Project& project, const SetNotes& edit, std::vector<Change>& changes)
    {
        SetNotes inverse { edit.file, {} };

        if (const auto found = project.notesFiles.find (edit.file); found != project.notesFiles.end())
        {
            inverse.content = std::move (found->second);

            if (edit.content)
                found->second = *edit.content;
            else
                project.notesFiles.erase (found);
        }
        else if (edit.content)
        {
            project.notesFiles.emplace (edit.file, *edit.content);
        }

        changes.push_back (NotesChanged { edit.file });
        return inverse;
    }

    Edit applyEdit (project::Project& project, const SetTempo& edit, std::vector<Change>& changes)
    {
        const SetTempo inverse { project.tempo };
        project.tempo = edit.bpm;
        changes.push_back (TempoChanged {});
        return inverse;
    }

    Edit applyEdit (project::Project& project, const SetMeter& edit, std::vector<Change>& changes)
    {
        const SetMeter inverse { project.meter };
        project.meter = edit.meter;
        changes.push_back (TimeSignatureChanged {});
        return inverse;
    }

    Edit applyEdit (project::Project& project, const InsertTrack& edit, std::vector<Change>& changes)
    {
        for (const auto& clip : edit.track.clips)
            requireFile (project, clip.file);

        project.tracks.insert (placeAt (project.tracks, edit.index), edit.track);
        changes.push_back (TrackAdded { edit.track.id });
        return RemoveTrack { edit.track.id };
    }

    Edit applyEdit (project::Project& project, const RemoveTrack& edit, std::vector<Change>& changes)
    {
        const auto found = locateTrack (project, edit.trackId);
        InsertTrack inverse { static_cast<std::size_t> (found - project.tracks.begin()), std::move (*found) };
        project.tracks.erase (found);
        changes.push_back (TrackRemoved { edit.trackId });
        return inverse;
    }

    Edit applyEdit (project::Project& project, const MoveTrack& edit, std::vector<Change>& changes)
    {
        const auto found = locateTrack (project, edit.trackId);
        const MoveTrack inverse { edit.trackId, static_cast<std::size_t> (found - project.tracks.begin()) };
        auto track = std::move (*found);
        project.tracks.erase (found);
        project.tracks.insert (placeAt (project.tracks, edit.index), std::move (track));
        changes.push_back (TrackMoved { edit.trackId });
        return inverse;
    }

    Edit applyEdit (project::Project& project, const SetTrack& edit, std::vector<Change>& changes)
    {
        auto& track = *locateTrack (project, edit.track.id);
        const auto& to = edit.track;

        SetTrack inverse { track };
        inverse.track.clips.clear(); // only the settings are needed

        if (track.name != to.name)
            changes.push_back (TrackRenamed { track.id });

        if (track.volume != to.volume || track.pan != to.pan || track.mute != to.mute || track.solo != to.solo)
            changes.push_back (MixChanged { track.id });

        if (track.devices != to.devices)
            changes.push_back (InstrumentChanged { track.id });

        if (track.drumMap != to.drumMap)
            changes.push_back (DrumMapChanged { track.id });

        track.name = to.name;
        track.volume = to.volume;
        track.pan = to.pan;
        track.mute = to.mute;
        track.solo = to.solo;
        track.devices = to.devices;
        track.drumMap = to.drumMap;
        return inverse;
    }

    Edit applyEdit (project::Project& project, const InsertClip& edit, std::vector<Change>& changes)
    {
        requireFile (project, edit.clip.file);
        auto& track = *locateTrack (project, edit.trackId);
        track.clips.insert (placeAt (track.clips, edit.index), edit.clip);
        changes.push_back (ClipAdded { edit.clip.id });
        return RemoveClip { edit.clip.id };
    }

    Edit applyEdit (project::Project& project, const RemoveClip& edit, std::vector<Change>& changes)
    {
        const auto [track, index] = locateClip (project, edit.clipId);
        const auto at = track->clips.begin() + static_cast<std::ptrdiff_t> (index);
        InsertClip inverse { track->id, index, std::move (*at) };
        track->clips.erase (at);
        changes.push_back (ClipRemoved { edit.clipId, track->id });
        return inverse;
    }

    Edit applyEdit (project::Project& project, const SetClip& edit, std::vector<Change>& changes)
    {
        requireFile (project, edit.clip.file);
        const auto [track, index] = locateClip (project, edit.clip.id);
        auto& clip = track->clips[index];
        const SetClip inverse { clip };

        if (clip.start != edit.clip.start)
            changes.push_back (ClipMoved { clip.id });

        if (clip.file != edit.clip.file)
            changes.push_back (ClipFileChanged { clip.id });

        clip = edit.clip;
        return inverse;
    }

    //==============================================================================
    const project::Track* trackOrNull (const project::Project& project, std::string_view id)
    {
        const auto found = std::ranges::find (project.tracks, id, &project::Track::id);
        return found == project.tracks.end() ? nullptr : &*found;
    }

    void addFilesOf (const project::Track* track, Files& files)
    {
        if (track != nullptr)
            for (const auto& clip : track->clips)
                files.insert (clip.file);
    }

    void addFileOf (const project::Project& project, std::string_view clipId, Files& files)
    {
        if (const auto* clip = project::findClip (project, clipId))
            files.insert (clip->file);
    }
}

Edit apply (project::Project& project, const Edit& edit, std::vector<Change>& changes)
{
    return std::visit ([&] (const auto& e) { return applyEdit (project, e, changes); }, edit);
}

void addFilesAffected (const project::Project& project, const Edit& edit, Files& files)
{
    struct Visitor
    {
        const project::Project& project;
        Files& files;

        void operator() (const SetNotes& e) const { files.insert (e.file); }
        void operator() (const SetTempo&) const {}

        void operator() (const SetMeter&) const
        {
            for (const auto& entry : project.notesFiles)
                files.insert (entry.first);
        }

        void operator() (const InsertTrack& e) const { addFilesOf (&e.track, files); }
        void operator() (const RemoveTrack& e) const { addFilesOf (trackOrNull (project, e.trackId), files); }

        // Every track using a file names its notes alike, so the track a
        // file is written for doesn't change its text.
        void operator() (const MoveTrack&) const {}

        void operator() (const SetTrack& e) const { addFilesOf (trackOrNull (project, e.track.id), files); }
        void operator() (const InsertClip& e) const { files.insert (e.clip.file); }
        void operator() (const RemoveClip& e) const { addFileOf (project, e.clipId, files); }

        void operator() (const SetClip& e) const
        {
            files.insert (e.clip.file);
            addFileOf (project, e.clip.id, files);
        }
    };

    std::visit (Visitor { project, files }, edit);
}
}
