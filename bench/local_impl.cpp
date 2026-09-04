// Library translation unit for the local A/B harness.
//
// This file is the ONLY translation unit that defines
// CANVAS_ITY_IMPLEMENTATION.  It is compiled with -std=c++03 -O2
// -fno-exceptions -fno-rtti against one snapshot include tree, so the
// measured code is exactly the portable C++03 library the project ships.
// See bench/local_ab.py (driver) and bench/local_bench.cpp (harness).
//
#define CANVAS_ITY_IMPLEMENTATION
#include "canvas_ity.hpp"
