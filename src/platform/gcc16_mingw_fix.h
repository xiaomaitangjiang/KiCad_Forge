// GCC 16 on MinGW: C11 functions (quick_exit, at_quick_exit, timespec_get)
// are missing from MinGW's C runtime but libstdc++ <cstdlib>/<ctime> expect them.
// Declare stubs in the global namespace before any C++ standard headers.
#pragma once

#if defined(__MINGW32__) && __GNUC__ >= 16

#include <time.h>

extern "C" {
    void quick_exit(int _status);
    int at_quick_exit(void (*_func)(void));
    int timespec_get(struct timespec* _ts, int _base);
}

#endif
