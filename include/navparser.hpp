#pragma once

#include <memory>
#include <vector>
#include "CNavFile.h"

namespace navparser
{
namespace NavEngine
{

// Is the Nav engine ready to run?
bool isReady();
// Are we currently pathing?
bool isPathing();
CNavFile *getNavFile();
bool navTo(const Vector &destination, int priority = 5, bool should_repath = true, bool nav_to_local = true, bool is_repath = true);
// Use when something unexpected happens, e.g. vischeck fails
void abandonPath();
// Use to cancel pathing completely
void cancelPath();

extern int current_priority;
} // namespace NavEngine
} // namespace navparser
