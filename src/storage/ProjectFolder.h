#pragma once

// A project's folder on disk, and saving into it (SPEC §4.5).
//
// A save writes only the files whose text changed. Each goes to a temporary
// file beside it first, all of them before any is swapped in; then they're
// swapped in, clip files before project.json. So a crash at any point leaves
// each file whole, old or new, and never leaves project.json naming a clip
// file that isn't there; and the next open finishes a save a crash stopped
// part-way, all of it or none, so the folder is never left part old and part
// new (below, finishInterruptedSave). A swap that fails puts back the files
// already swapped, so a failed save leaves the folder as it was.
//
// Nothing here deletes a file whose text exists nowhere else: not on disk,
// and not in the session whose save wrote it. Such a file is set aside in
// unused/ (CLAUDE.md).
//
// Before a save changes anything, it keeps the version on disk whole, in
// .hocket/saves/<number> <time> <author>[ <name>]/: project.json, its clip
// files, and .hocket/save.json, which says who saved that version and when.
// Each kept save is a project folder of its own, so it loads, and restores
// by hand, by itself. Nothing in .hocket/ is ever part of the project.
//
// Clip files that were in the project, but that no clip uses any more, are
// moved to unused/, never deleted. Files Hocket never had in the project are
// never touched.
//
// It notes each project file's stamp (FileStamp) whenever Hocket reads or
// writes it. A save that would overwrite a file changed on disk since then,
// or a file Hocket never had in the project, writes nothing and names it;
// with overwrite, it keeps the version on disk first, then saves.

#include "core/commands/Storage.h"
#include "core/project/FileStamp.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace hocket::storage
{
    // Where a save moves clip files no clip uses any more, keeping their
    // paths: "unused/clips/verse-2.notes".
    inline constexpr std::string_view unusedFolderName = "unused";

    // Added to a file's name for the temporary file a save writes beside it:
    // "clips/verse.notes.hocket-saving".
    inline constexpr std::string_view temporarySuffix = ".hocket-saving";

    // Who saved the files beside it, and when: in a project folder, and in
    // each kept save's.
    inline constexpr std::string_view saveInfoFile = ".hocket/save.json";

    // The kept saves, a folder each.
    inline constexpr std::string_view keptSavesFolder = ".hocket/saves";

    // How long a kept save's folder name may be, in UTF-16 code units, so its
    // files' paths stay well inside Windows' 260.
    inline constexpr std::size_t maxKeptSaveNameLength = 64;

    // "4 2026-11-20 14-02 Lee before the bridge experiment": the number, the
    // local time it was saved, the author ("before Hocket" for a version Hocket
    // didn't save) and its name, if any. Valid on Windows whatever the author
    // or name hold: characters Windows refuses in a name (<>:"/\|?* and control
    // characters) become spaces, runs of spaces one, invalid UTF-8 is dropped,
    // it's cut short to maxKeptSaveNameLength, and trailing dots and spaces
    // go. Starting with the number, it's never a reserved name like CON or NUL.
    std::string keptSaveFolderName (int number, const commands::SaveInfo& info);

    class ProjectFolder final : public commands::Storage
    {
    public:
        // When a save happens; the system clock, unless a test says otherwise.
        using Clock = std::function<std::chrono::system_clock::time_point()>;

        // Loads the project in `folder`, noting each file's stamp just before
        // reading it. The folder is there when the project loaded.
        static std::pair<project::LoadResult, std::optional<ProjectFolder>> open (std::filesystem::path folder, Clock clock = {});

        // A folder whose project doesn't load (a hand edit broke project.json,
        // say), to save a session started from its recovery copy into
        // (SPEC §4.5): it knows project.json and every clip file in clips/ as
        // they are on disk, so the first save keeps them all before writing,
        // and moves those the session doesn't use to unused/.
        static ProjectFolder asItIs (std::filesystem::path folder, Clock clock = {});

        // `loaded` is the project as just read from the folder, its files'
        // stamps taken now; or an empty project for a new folder.
        ProjectFolder (std::filesystem::path folder, const project::Project& loaded, Clock clock = {});

        const std::filesystem::path& path() const { return folder; }

        // A save that changes no file, but has a name, names the version on
        // disk: only its save.json is written, and nothing is kept.
        commands::SaveResult save (const project::Project& project, const commands::Save& request, const commands::Author& author) override;

        std::vector<commands::KeptSave> keptSaves() override;
        project::LoadResult loadKeptSave (const commands::KeptSave& save) override;

        // A project file ("project.json", "clips/bass.notes") as Hocket last
        // read or wrote it: its stamp then, and the canonical text the session
        // had for it, which the file matched. nullopt for a file Hocket never had.
        std::optional<project::FileStamp> knownStamp (std::string_view file) const;
        std::optional<std::string> knownText (std::string_view file) const;

        // The session took in a project file changed outside Hocket (--watch):
        // what was on disk at this stamp, which the session's text now matches.
        void noteRead (const std::string& file, const project::FileStamp& stamp, std::string text);

        // Whether each of these files (project::write's) has the text Hocket
        // last read or wrote for it: then the session holds nothing that isn't
        // on disk, as far as Hocket knows, so a recovery copy has nothing to keep.
        bool matchesKnown (const std::map<std::string, std::string>& files) const;

        // Files a recovery copy away with the kept saves, labelled as a
        // discarded recovery copy and timed by when it was written, so revert
        // can still bring it back (--play --discard-recovery, SPEC §4.5). Its
        // files are copied as they are, whether or not they read as a project.
        // nullopt, with the problem, if it couldn't be kept.
        std::optional<commands::KeptSave> keepRecoveryCopy (const std::filesystem::path& copy, commands::SaveTime written, std::string& problem);

        // For tests of a crash or a failure part-way through a save, or through
        // finishing one (finishInterruptedSave): each step it takes on the
        // disk, in order.
        struct SaveStep
        {
            enum class Kind
            {
                setAside,       // moving a file or folder an earlier save left, `path`, to unused/
                writeTemporary, // writing a file's new text beside it, `path` (the temporary file)
                keepFile,       // writing a file of the kept save's copy, `path`
                nameKept,       // renaming the kept save's copy to its name, `path`
                readBack,       // reading a new kept save back at its final name, `path`
                writeMarker,    // writing .hocket/saving.json beside its place, `path` (the temporary file)
                nameMarker,     // putting it in place, `path`: from here the save is finished, even if it's stopped
                swap,           // putting a file's new text in place, `path` (the file)
                putBack,        // a failed save putting a file it replaced back as it was, `path` (the file)
                moveUnused,     // moving a file no clip uses to unused/, `path` (the file)
                removeMarker    // removing .hocket/saving.json, `path`: the save is done
            };

            Kind kind;
            std::filesystem::path path;
        };

        // What a test's step hook makes of a step: it happens; or a crash
        // stops everything there, and nothing after it happens, nothing
        // cleaned up; or it fails as if the OS had refused it, and the save or
        // finishing goes on as it does when one fails.
        enum class StepOutcome
        {
            proceed,
            crash,
            fail
        };

        using StepHook = std::function<StepOutcome (const SaveStep&)>;

        // Called before each step. For tests only.
        void setStepHook (StepHook hook) { stepHook = std::move (hook); }

    private:
        struct Known
        {
            project::FileStamp stamp;
            std::string text;
        };

        std::filesystem::path folder;
        Clock clock;
        StepHook stepHook;

        StepOutcome step (SaveStep::Kind kind, const std::filesystem::path& path) const
        {
            return stepHook ? stepHook (SaveStep { kind, path }) : StepOutcome::proceed;
        }

        // project.json and every clip file Hocket has had in the project here,
        // loaded or saved, that may still be on disk. The clip files are the
        // only ones a save may move to unused/.
        std::map<std::string, Known, std::less<>> known;

        // .hocket/saving.json as this session's last save left it there: it
        // couldn't be removed once the files were in place, or it describes a
        // failed save that couldn't put every file back. A later save may
        // replace it; any other marker stops a save (an older save's,
        // interrupted).
        std::optional<project::FileStamp> ownMarker;

        // Copies the version on disk into a new kept save: project.json if
        // it's there, its clip files, and every file in `overwrites`, the
        // files on disk the save replaces. A problem if any of those can't be
        // copied, so the save writes nothing. nullopt with no problem when
        // `overwrites` is empty: the save replaces nothing. With files changed
        // outside Hocket, it's labelled so, at the latest change's time.
        std::optional<commands::KeptSave> keepVersionOnDisk (const std::vector<std::string>& overwrites, const std::vector<std::string>& changedOutside,
                                                             std::string& problem);

        // Who the version on disk is by, and when (SPEC §4.5), from its files'
        // texts (project.json and the clip files it names) and the files this
        // session saw change outside Hocket: as its save.json says, if that
        // describes these files, and then `saveJson` is that file's text to
        // keep as it is; "changed outside Hocket" at the latest change, if
        // they aren't the files it describes; "before Hocket" at
        // project.json's date, if there's no save.json.
        commands::SaveInfo describeVersionOnDisk (const std::map<std::string, std::string>& version, const std::vector<std::string>& changedOutside,
                                                  std::optional<std::string>& saveJson) const;

        // Writes these files, relative paths to their text (.hocket/save.json
        // included), as the next kept save: whole, or not at all.
        std::optional<commands::KeptSave> keepFiles (const std::map<std::string, std::string>& texts, const commands::SaveInfo& info,
                                                     std::string& problem);
    };

    // The kept saves in a project folder, oldest first: ProjectFolder::keptSaves,
    // for a folder whose project may not load.
    std::vector<commands::KeptSave> keptSavesIn (const std::filesystem::path& folder);

    // Saves an empty project (120 BPM, 4/4, no tracks) in `folder`, which
    // must be new or empty; a new folder is made, with any folders above it.
    // Returns what went wrong, or empty.
    std::string createProject (const std::filesystem::path& folder, const commands::Author& author);

    //==============================================================================
    // A save interrupted part-way (SPEC §4.5). A save writes every file's new
    // text beside it and keeps the version it replaces, then puts
    // .hocket/saving.json in place (written beside it first, so it's whole or
    // not there), then puts the files in place, moves unused ones to unused/,
    // and removes .hocket/saving.json. So a save stopped by a crash before the
    // marker changed nothing but leftovers; one stopped after it has every new
    // text on disk, ready to put in place.

    // There while a save puts its files in place.
    inline constexpr std::string_view savingMarkerFile = ".hocket/saving.json";

    struct FinishedSave
    {
        enum class Outcome
        {
            nothing,  // there was no interrupted save
            setAside, // a save interrupted before its marker: what it left is set aside in unused/
            finished, // a save interrupted after its marker: finished, as it meant
            blocked,  // one that can't be finished: nothing on disk was changed
            failed    // finishing, or setting aside, failed part-way: the next open tries again
        };

        Outcome outcome = Outcome::nothing;
        std::string message; // what was done, or why not and where the versions are; empty for nothing

        // Whether a session may open the folder: not after blocked or failed.
        bool proceed() const { return outcome != Outcome::blocked && outcome != Outcome::failed; }
    };

    // Before a session opens the folder (SPEC §4.5): finishes a save a crash
    // interrupted after its marker, all of it or none. Only when the marker
    // reads and parses, every file the save still has to put in place is
    // beside its place as the save wrote it, every file it has put in place is
    // as it put it, and every file it replaces is as the save found it and is
    // in the kept save. Otherwise it changes nothing (blocked), and the message
    // says why, and where the version before the save (the kept save) and the
    // save's (the temporary files) are. A step that fails stops it there
    // (failed): what's done stays done, the marker stays, and the next open
    // finishes the rest. What a save stopped before its marker left (its
    // temporary files, a part-made copy) is set aside in unused/, never
    // deleted. The step hook is for tests, as ProjectFolder's.
    FinishedSave finishInterruptedSave (const std::filesystem::path& folder, const ProjectFolder::StepHook& stepHook = {});

    // For --check, which changes nothing: what finishInterruptedSave would
    // find, said as a note, or empty when there's nothing.
    std::string describeInterruptedSave (const std::filesystem::path& folder);

    // Whether .hocket/saving.json is there: a save was interrupted after it
    // began putting files in place, or something else put it there.
    bool hasInterruptedSave (const std::filesystem::path& folder);
}
