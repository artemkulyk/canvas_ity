#!/bin/sh
# Clean 6-way microbenchmark build (identical 17-workload scenes,
# per-trial RNG reset, warm-up + best/median reporting).
#
# canvas_ity default vs the experimental cell rasterizer are two builds
# of the SAME src/canvas_ity.hpp: the latter adds -DCELL_PROTO, which
# routes plain solid fills through the analytic cell sweep inside the
# header.  See bench/ports/ci_clean_driver.cpp for the methodology.
#
# Machine-local paths for the four external libraries are set below;
# edit them for your system.  Artifacts land in bench/build/cleanbench/
# (ignored by bench/.gitignore).  Raw results are written next to the
# binaries under results/.
set -e
cd "$(dirname "$0")/build"
ROOT=/Users/artemkulyk/projects/ai_opti
CITREE="$ROOT/canvas_ity"
mkdir -p cleanbench/results

CXX=c++
CXXFLAGS="-O2"
DRIVERS="$CITREE/bench/ports"

$CXX $CXXFLAGS -std=c++11 -I "$CITREE/src" \
    -o cleanbench/mb_ci "$DRIVERS/ci_clean_driver.cpp"

$CXX $CXXFLAGS -std=c++11 -DCELL_PROTO -I "$CITREE/src" \
    -o cleanbench/mb_ci_cell "$DRIVERS/ci_clean_driver.cpp"

# Blend2D's prebuilt static lib contains LTO bitcode from LLVM 22.1.8
# (built with llvm@22), so its binary must be compiled and linked with
# the matching compiler.  The library is prebuilt either way; the JIT
# pixel pipelines are generated at runtime, so the host compiler only
# affects the tiny driver code.
/opt/homebrew/opt/llvm@22/bin/clang++ $CXXFLAGS -std=c++17 \
    -I "$ROOT/blend2d/blend2d" -I "$ROOT/blend2d" \
    -o cleanbench/mb_bl2d "$DRIVERS/microbench_blend2d.cpp" \
    "$ROOT/blend2d/build/libblend2d.a"

$CXX $CXXFLAGS -I "$ROOT/thorvg/inc" \
    -o cleanbench/mb_tvg "$DRIVERS/microbench_thorvg.cpp" \
    -L "$ROOT/thorvg/build-simd/src" -lthorvg-1 \
    -Wl,-rpath,"$ROOT/thorvg/build-simd/src"

$CXX $CXXFLAGS -I "$ROOT/agg-2.6/agg-src/include" \
    -o cleanbench/mb_agg26 "$DRIVERS/microbench_agg26.cpp" \
    "$ROOT"/agg-2.6/agg-src/src/*.cpp

$CXX $CXXFLAGS -std=c++17 -F/opt/homebrew/opt/qtbase/Frameworks \
    -o cleanbench/mb_qt6 "$DRIVERS/microbench_qt.cpp" \
    -framework QtGui -framework QtCore

echo "all six binaries built in bench/build/cleanbench/"
echo "run e.g.:  bench/build/cleanbench/mb_ci 15   and   bench/build/cleanbench/mb_ci_cell 15"
