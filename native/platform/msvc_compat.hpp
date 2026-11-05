// SPDX-License-Identifier: MIT
// TASK-180: the two Microsoft C runtime calls that pinned, hash-checked donor-derived sources make (the port main.cpp:
// _putenv_s("PS2_GS", "gpu")). Those files cannot be edited, so non-Windows builds force-include this header for them
// only (android/native/CMakeLists.txt). Native code calls gt4::platform::set_environment instead.
#pragma once

#ifndef _WIN32
#include <cstdlib>

inline int _putenv_s(const char *name, const char *value) { return setenv(name, value, 1); }
#endif
