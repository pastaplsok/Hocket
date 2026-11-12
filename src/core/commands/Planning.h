#pragma once

// Validating each command against the project, and planning the edits that
// carry it out. Internal to the bus, which records what a plan does.

#include "core/commands/Edits.h"

namespace hocket::commands::planning
{
    edits::Plan plan (const project::Project& project, const WriteClip& command);
}
