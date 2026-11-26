// linux_compat.h - force-included into the runtime's C++ sources on Linux only (gpu/CMakeLists.txt), the counterpart
// of gpu/shim/gow3_win_compat.h: the two Windows spellings that shared sources use and that have a plain meaning on
// Linux, so those sources stay as they are.
#pragma once
#ifndef _WIN32
#include <cerrno>
#include <cmath>   // u2mp/session.cpp uses std::lround and gets <cmath> only through libc++ headers
#include <cstdlib>

// x86-64 has one C calling convention; the Windows keyword marks it in function pointer types.
#define __cdecl

// _putenv_s(name, value): set (or replace) one environment variable of this process.
inline int _putenv_s(const char *name, const char *value) { return setenv(name, value, 1) == 0 ? 0 : errno; }
#endif
