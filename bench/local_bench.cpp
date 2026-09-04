// Local A/B workload harness for canvas_ity.  Kept separate from
// bench/microbench.cpp (the CI inventory harness): this one runs ONE
// workload per process invocation so the driver (bench/local_ab.py) can
// interleave A/B pairs per workload, and every workload is sized so its
// local median lands at or above 10 ms (short ones are lengthened by
// repeating the same representative operation, never by changing it).
// Every workload draws a deterministic designed scene with the same
// API call counts as the random original, so renders are worth looking
// at: ./local_bench render OUTDIR writes one PNG per workload.
//
//   c++ -O2 -std=c++11 -I<tree> -c bench/local_bench.cpp
//   c++ -O2 -std=c++03 -fno-exceptions -fno-rtti -I<tree> -c bench/local_impl.cpp
//   c++ -O2 -o local_bench local_bench.o local_impl.o
//   ./local_bench 7 pattern_tiled
//
// Prints one line: "<workload> <best_ms>ms".  Best of <trials> trials,
// each trial on a fresh canvas, exactly like bench/microbench.cpp.
//
// NOTE: the library calls stay opaque to this TU (the implementation
// lives in bench/local_impl.cpp), so nothing here can be optimized away
// across the TU boundary at -O2 without LTO.

#include "canvas_ity.hpp"

#if defined( __linux__ )
#include <time.h>
#elif defined( _WIN32 )
#include <windows.h>
#elif defined( __MACH__ )
#include <mach/mach_time.h>
#else
#include <sys/time.h>
#endif
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace canvas_ity;

static double get_seconds()
{
#if defined( __linux__ )
    timespec now;
    clock_gettime( CLOCK_MONOTONIC, &now );
    return now.tv_sec + now.tv_nsec * 1.0e-9;
#elif defined( _WIN32 )
    static double rate = 0.0;
    if ( !rate )
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency( &frequency );
        rate = 1.0 / static_cast< double >( frequency.QuadPart );
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter( &now );
    return now.QuadPart * rate;
#elif defined( __MACH__ )
    static double rate = 0.0;
    if ( !rate )
    {
        static mach_timebase_info_data_t frequency;
        mach_timebase_info( &frequency );
        rate = frequency.numer * 1.0e-9 / frequency.denom;
    }
    return mach_absolute_time() * rate;
#else
    timeval now;
    gettimeofday( &now, 0 );
    return now.tv_sec + now.tv_usec * 1.0e-6;
#endif
}

static unsigned long long rng_state = 0x243f6a8885a308d3ULL;
static float frand()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return static_cast< float >( rng_state & 0xffffffULL ) /
           static_cast< float >( 0x1000000ULL );
}


typedef void ( *workload )( canvas &that, int width, int height );

struct entry { char const *name; workload call; int width; int height; };

// Workloads -----------------------------------------------------------------
// Same operations as bench/microbench.cpp; repeat counts are raised where
// the local median fell below 10 ms so every workload can gate.

// Shared five-ink palette for the designed scenes (all workloads keep
// their original API call counts; only placement and colors change).
static float const ink[ 6 ][ 4 ] = {
    { 0.13f, 0.23f, 0.38f, 1.0f }, // indigo
    { 0.91f, 0.35f, 0.13f, 1.0f }, // persimmon
    { 0.87f, 0.62f, 0.13f, 1.0f }, // ochre
    { 0.13f, 0.55f, 0.55f, 1.0f }, // teal
    { 0.45f, 0.27f, 0.49f, 1.0f }, // plum
    { 0.94f, 0.93f, 0.88f, 1.0f }, // paper
};

// One lattice-string-art repeat (50 subpaths); path_construction runs
// this 1200 times for timing, then paints a small six-subpath motif so
// the report shows what the construction builds.
//
static void lattice_repeat( canvas &that, int repeat )
{
    that.begin_path();
    for ( int subpath = 0; subpath < 50; ++subpath )
    {
        float base = (float)( ( repeat * 131 + subpath * 17 ) % 64 );
        float base_x = base * 8.0f;
        float base_y = (float)( ( repeat * 37 + subpath * 41 ) % 64 ) * 8.0f;
        that.move_to( base_x, base_y );
        for ( int step = 0; step < 10; ++step )
        {
            float far_x =
                (float)( ( repeat * 91 + subpath * 53 + step * 29 ) % 64 ) *
                8.0f;
            float far_y =
                (float)( ( repeat * 71 + subpath * 11 + step * 47 ) % 64 ) *
                8.0f;
            that.line_to( far_x, far_y );
            that.bezier_curve_to(
                (float)( ( repeat * 13 + step * 31 ) % 64 ) * 8.0f,
                (float)( ( subpath * 7 + step * 59 ) % 64 ) * 8.0f,
                (float)( ( repeat * 43 + step * 19 ) % 64 ) * 8.0f,
                (float)( ( subpath * 23 + step * 5 ) % 64 ) * 8.0f,
                far_x, far_y );
        }
    }
}

static void path_construction( canvas &that, int, int )
{
    // Same call counts as before (1320 x 50 subpaths x 11 segments),
    // but lattice points from cheap integer hashes instead of RNG so
    // the tangle reads as geometric string art; one small motif fill
    // at the end makes it visible without moving the cost needle.
    for ( int repeat = 0; repeat < 1320; ++repeat )
        lattice_repeat( that, repeat );
    that.begin_path();
    for ( int subpath = 0; subpath < 6; ++subpath )
    {
        float base_x = 64.0f + subpath * 64.0f;
        that.move_to( base_x, 64.0f );
        for ( int step = 0; step < 10; ++step )
        {
            float far_x = 64.0f +
                (float)( ( subpath * 53 + step * 29 ) % 48 ) * 8.0f;
            float far_y = 64.0f +
                (float)( ( subpath * 11 + step * 47 ) % 48 ) * 8.0f;
            that.line_to( far_x, far_y );
            that.bezier_curve_to(
                64.0f + (float)( ( step * 31 ) % 48 ) * 8.0f,
                64.0f + (float)( ( subpath * 7 + step * 59 ) % 48 ) * 8.0f,
                64.0f + (float)( ( step * 19 ) % 48 ) * 8.0f,
                64.0f + (float)( ( subpath * 23 + step * 5 ) % 48 ) * 8.0f,
                far_x, far_y );
        }
    }
    that.set_color( fill_style, ink[ 0 ][ 0 ], ink[ 0 ][ 1 ],
                    ink[ 0 ][ 2 ], ink[ 0 ][ 3 ] );
    that.fill();
}

static void flattening_fill( canvas &that, int width, int height )
{
    // One path of many highly-curved beziers: dominated by flattening.
    // Wavy concentric rings make a moire flower.
    float const center_x = 0.5f * width;
    float const center_y = 0.5f * height;
    that.begin_path();
    for ( int subpath = 0; subpath < 120; ++subpath )
    {
        float const radius = 6.0f + 2.1f * subpath;
        float const wave = 0.30f * radius + 4.0f;
        float const phase = 0.7f * subpath;
        float dir = ( subpath % 2 == 0 ) ? 1.0f : -1.0f;
        for ( int step = 0; step <= 12; ++step )
        {
            float a0 = dir * 6.2832f * step / 12.0f;
            float a1 = dir * 6.2832f * ( step + 0.5f ) / 12.0f;
            float r0 = radius + wave * sinf( 7.0f * a0 + phase );
            float lift = ( step % 2 == 0 ? 1.0f : -1.0f ) *
                         ( 0.5f * wave + 30.0f );
            float r1 = radius + wave * sinf( 7.0f * a1 + phase );
            float px = center_x + r0 * cosf( a0 );
            float py = center_y + r0 * sinf( a0 );
            float cx = center_x + r1 * cosf( a1 ) - lift * sinf( a1 );
            float cy = center_y + r1 * sinf( a1 ) + lift * cosf( a1 );
            if ( step == 0 )
                that.move_to( px, py );
            else
                that.bezier_curve_to( cx, cy, cx, cy, px, py );
        }
        that.close_path();
    }
    that.fill();
}

static void fill_small( canvas &that, int width, int height )
{
    // 2500 small dots on a jittered grid, one ink per batch of 500.
    for ( int batch = 0; batch < 5; ++batch )
    {
        that.set_color( fill_style, ink[ batch ][ 0 ], ink[ batch ][ 1 ],
                        ink[ batch ][ 2 ], ink[ batch ][ 3 ] );
        for ( int step = 0; step < 500; ++step )
        {
            int dot = batch * 500 + step;
            float cx = ( dot % 50 ) * ( width / 50.0f ) +
                       frand() * ( width / 50.0f );
            float cy = ( dot / 50 ) * ( height / 50.0f ) +
                       frand() * ( height / 50.0f );
            that.begin_path();
            that.arc( cx, cy, 2.0f + frand() * 6.0f,
                      0.0f, 6.2832f, false );
            that.fill();
        }
    }
}

static void fill_large( canvas &that, int width, int height )
{
    that.set_color( fill_style, ink[ 0 ][ 0 ], ink[ 0 ][ 1 ],
                    ink[ 0 ][ 2 ], ink[ 0 ][ 3 ] );
    for ( int step = 0; step < 24; ++step )
        that.fill_rectangle( -10.0f, -10.0f,
                             static_cast< float >( width ) + 20.0f,
                             static_cast< float >( height ) + 20.0f );
}

static void fill_zone_plate( canvas &that, int width, int height )
{
    // Concentric rings: extreme anti-aliasing workload.  Rings
    // alternate indigo and persimmon for a bullseye.
    for ( int ring = 0; ring < 132; ++ring )
    {
        int pen = ( ring % 2 == 0 ) ? 0 : 1;
        that.set_color( fill_style, ink[ pen ][ 0 ], ink[ pen ][ 1 ],
                        ink[ pen ][ 2 ], ink[ pen ][ 3 ] );
        float radius = 3.0f + 3.0f * ring;
        that.begin_path();
        that.arc( 0.5f * width, 0.5f * height, radius + 1.5f,
                  0.0f, 6.2832f, false );
        that.move_to( 0.5f * width + radius - 1.5f, 0.5f * height );
        that.arc( 0.5f * width, 0.5f * height, radius - 1.5f,
                  6.2832f, 0.0f, true );
        that.fill();
    }
}

static void stroke_many( canvas &that, int width, int height )
{
    // 400 short thin curves drifting across the canvas.
    that.set_line_width( 1.5f );
    that.begin_path();
    for ( int step = 0; step < 400; ++step )
    {
        float x0 = (float)( ( step * 37 ) % 412 ) - 10.0f;
        float y0 = ( step + 0.5f ) * ( height / 400.0f );
        float wobble = 80.0f * sinf( 0.35f * step );
        that.move_to( x0, y0 );
        that.bezier_curve_to( x0 + 55.0f, y0 + wobble,
                              x0 + 115.0f, y0 - wobble,
                              x0 + 170.0f, y0 );
    }
    that.stroke();
}

static void stroke_wide( canvas &that, int width, int height )
{
    // 100 wide rounded ribbons in four inks: same 600 beziers as
    // before, stroked in four passes of 25 subpaths.
    that.set_line_width( 12.0f );
    that.line_join = rounded;
    for ( int batch = 0; batch < 4; ++batch )
    {
        that.set_color( stroke_style, ink[ batch ][ 0 ], ink[ batch ][ 1 ],
                        ink[ batch ][ 2 ], ink[ batch ][ 3 ] );
        that.begin_path();
        for ( int step = 0; step < 25; ++step )
        {
            int ribbon = batch * 25 + step;
            float y0 = ( ribbon + 0.5f ) * ( height / 100.0f );
            float amp = 30.0f + 1.5f * ( ribbon % 17 );
            that.move_to( -10.0f, y0 );
            for ( int part = 0; part < 6; ++part )
            {
                float x0 = ( part + 0.5f ) * ( width / 6.0f );
                float s = sinf( 0.11f * ribbon + 0.9f * part );
                that.bezier_curve_to( x0 - 0.2f * width, y0 + amp * s,
                                      x0 + 0.2f * width, y0 - amp * s,
                                      x0 + 0.3f * width, y0 );
            }
        }
        that.stroke();
    }
}

static void gradient_linear( canvas &that, int width, int height )
{
    that.set_linear_gradient( fill_style, 0.0f, 0.0f,
                              static_cast< float >( width ),
                              static_cast< float >( height ) );
    that.add_color_stop( fill_style, 0.0f, 1.0f, 0.2f, 0.1f, 1.0f );
    that.add_color_stop( fill_style, 0.5f, 0.1f, 0.9f, 0.3f, 1.0f );
    that.add_color_stop( fill_style, 1.0f, 0.2f, 0.1f, 1.0f, 1.0f );
    for ( int step = 0; step < 10; ++step )
        that.fill_rectangle( 0.0f, 0.0f,
                             static_cast< float >( width ),
                             static_cast< float >( height ) );
}

static void gradient_radial( canvas &that, int width, int height )
{
    that.set_radial_gradient( fill_style,
                              0.3f * width, 0.4f * height, 2.0f,
                              0.5f * width, 0.5f * height,
                              0.6f * width );
    that.add_color_stop( fill_style, 0.0f, 1.0f, 0.9f, 0.2f, 1.0f );
    that.add_color_stop( fill_style, 0.7f, 0.2f, 0.5f, 0.9f, 0.8f );
    that.add_color_stop( fill_style, 1.0f, 0.1f, 0.1f, 0.4f, 0.2f );
    for ( int step = 0; step < 10; ++step )
        that.fill_rectangle( 0.0f, 0.0f,
                             static_cast< float >( width ),
                             static_cast< float >( height ) );
}

static void pattern_tiled( canvas &that, int width, int height )
{
    // A 64x64 indigo diamond tile on slate instead of byte noise.
    std::vector< unsigned char > image( 64 * 64 * 4 );
    for ( int y = 0; y < 64; ++y )
        for ( int x = 0; x < 64; ++x )
        {
            int diamond = abs( x - 32 ) + abs( y - 32 );
            float edge = diamond < 20 ? 1.0f :
                         diamond < 24 ? 0.5f : 0.0f;
            float dot = ( x - 32 ) * ( x - 32 ) +
                        ( y - 32 ) * ( y - 32 ) < 36.0f ? 1.0f : 0.0f;
            size_t at = ( (size_t)y * 64 + (size_t)x ) * 4;
            image[ at + 0 ] = static_cast< unsigned char >(
                20.0f + 200.0f * edge + 35.0f * dot );
            image[ at + 1 ] = static_cast< unsigned char >(
                30.0f + 60.0f * edge + 35.0f * dot );
            image[ at + 2 ] = static_cast< unsigned char >(
                60.0f + 40.0f * edge + 35.0f * dot );
            image[ at + 3 ] = 255;
        }
    that.set_pattern( fill_style, &image.front(), 64, 64, 64 * 4, repeat );
    for ( int step = 0; step < 10; ++step )
        that.fill_rectangle( 0.0f, 0.0f,
                             static_cast< float >( width ),
                             static_cast< float >( height ) );
}

static void image_scaled( canvas &that, int width, int height )
{
    // A smooth 128x96 color field with a dark grid instead of byte noise.
    std::vector< unsigned char > image( 128 * 96 * 4 );
    for ( int y = 0; y < 96; ++y )
        for ( int x = 0; x < 128; ++x )
        {
            float u = x / 128.0f;
            float v = y / 96.0f;
            float grid = ( x % 16 == 0 || y % 16 == 0 ) ? 0.25f : 1.0f;
            size_t at = ( (size_t)y * 128 + (size_t)x ) * 4;
            image[ at + 0 ] = static_cast< unsigned char >(
                255.0f * ( 0.15f + 0.7f * u ) * grid );
            image[ at + 1 ] = static_cast< unsigned char >(
                255.0f * ( 0.15f + 0.7f * v ) * grid );
            image[ at + 2 ] = static_cast< unsigned char >(
                255.0f * ( 0.9f - 0.6f * u * v ) * grid );
            image[ at + 3 ] = 255;
        }
    for ( int step = 0; step < 10; ++step )
        that.draw_image( &image.front(), 128, 96, 128 * 4,
                         0.0f, 0.0f, static_cast< float >( width ),
                         static_cast< float >( height ) );
}

static void clip_heavy( canvas &that, int width, int height )
{
    // An eight-petal rosette clip with 150 concentric-ring fills.
    that.begin_path();
    for ( int step = 0; step < 8; ++step )
    {
        float angle = 6.2832f * step / 8.0f;
        float cx = 0.5f * width + 0.12f * width * cosf( angle );
        float cy = 0.5f * height + 0.12f * height * sinf( angle );
        that.arc( cx, cy, 0.3f * width, angle, angle + 6.2832f, false );
    }
    that.clip();
    for ( int step = 0; step < 150; ++step )
    {
        int pen = step % 5;
        that.set_color( fill_style, ink[ pen ][ 0 ], ink[ pen ][ 1 ],
                        ink[ pen ][ 2 ], ink[ pen ][ 3 ] );
        float radius = 4.0f + 1.6f * step;
        that.begin_path();
        that.arc( 0.5f * width, 0.5f * height, radius + 1.6f,
                  0.0f, 6.2832f, false );
        that.move_to( 0.5f * width + radius - 1.6f, 0.5f * height );
        that.arc( 0.5f * width, 0.5f * height, radius - 1.6f,
                  6.2832f, 0.0f, true );
        that.fill();
    }
}

static void composite_ops( canvas &that, int width, int height )
{
    for ( int round = 0; round < 7; ++round )
        for ( int op = 1; op <= 15; ++op )
        {
            if ( op == 5 || op == 6 || op == 8 || op == 9 )
                continue;
            that.global_composite_operation =
                static_cast< composite_operation >( op );
            that.set_color( fill_style, 0.9f, 0.5f, 0.2f, 0.6f );
            that.fill_rectangle( 0.1f * width, 0.1f * height,
                                 0.6f * width, 0.6f * height );
        }
}

static void shadow_blurred( canvas &that, int width, int height )
{
    that.set_shadow_color( 0.0f, 0.0f, 0.0f, 0.6f );
    that.set_color( fill_style, ink[ 0 ][ 0 ], ink[ 0 ][ 1 ],
                    ink[ 0 ][ 2 ], ink[ 0 ][ 3 ] );
    for ( int step = 0; step < 20; ++step )
    {
        that.set_shadow_blur( 4.0f + ( step % 5 ) * 4.0f );
        that.shadow_offset_x = 5.0f;
        that.shadow_offset_y = 7.0f;
        that.begin_path();
        that.rectangle( 0.1f * width, 0.1f * height,
                        0.5f * width, 0.4f * height );
        that.fill();
    }
}

static void transforms( canvas &that, int width, int height )
{
    // A hundred rotated fills cycling five inks: a rosette.
    for ( int step = 0; step < 112; ++step )
    {
        that.save();
        that.translate( 0.5f * width, 0.5f * height );
        that.rotate( 0.1f * step );
        that.scale( 1.0f + 0.01f * step, 0.9f );
        that.translate( -0.5f * width, -0.5f * height );
        int pen = step % 5;
        that.set_color( fill_style, ink[ pen ][ 0 ], ink[ pen ][ 1 ],
                        ink[ pen ][ 2 ], ink[ pen ][ 3 ] );
        that.begin_path();
        that.arc( 0.4f * width, 0.4f * height, 0.3f * width,
                  0.0f, 6.2832f, false );
        that.fill();
        that.restore();
    }
}

static void many_primitives( canvas &that, int width, int height )
{
    // Twenty thousand tiny rects in diagonal mosaic bands.
    for ( int step = 0; step < 22000; ++step )
    {
        float x = frand() * width;
        float y = frand() * height;
        int band = ( static_cast< int >( x * 0.05f + y * 0.03f ) % 6 + 6 ) % 6;
        that.set_color( fill_style, ink[ band ][ 0 ], ink[ band ][ 1 ],
                        ink[ band ][ 2 ], ink[ band ][ 3 ] );
        that.fill_rectangle( x, y,
                             1.0f + frand() * 8.0f, 1.0f + frand() * 8.0f );
    }
}

static void complex_scene( canvas &that, int width, int height )
{
    // A dense synthetic scene mixing everything.
    for ( int layer = 0; layer < 10; ++layer )
    {
        that.save();
        that.translate( frand() * 0.2f * width, frand() * 0.2f * height );
        that.rotate( frand() * 6.28f );
        if ( layer % 3 == 0 )
        {
            that.set_linear_gradient( fill_style, 0.0f, 0.0f,
                                      0.5f * width, 0.5f * height );
            that.add_color_stop( fill_style, 0.0f, 1.0f, 0.0f, 0.0f, 0.9f );
            that.add_color_stop( fill_style, 1.0f, 0.0f, 0.0f, 1.0f, 0.5f );
        }
        else
        {
            int pen = layer % 5;
            that.set_color( fill_style, ink[ pen ][ 0 ], ink[ pen ][ 1 ],
                            ink[ pen ][ 2 ], 0.25f + 0.1f * ( layer % 5 ) );
        }
        that.begin_path();
        for ( int subpath = 0; subpath < 12; ++subpath )
        {
            that.move_to( frand() * width, frand() * height );
            for ( int step = 0; step < 8; ++step )
                that.bezier_curve_to( frand() * width, frand() * height,
                                      frand() * width, frand() * height,
                                      frand() * width, frand() * height );
            that.close_path();
        }
        that.fill();
        that.set_line_width( 1.0f + frand() * 3.0f );
        int pen = ( layer + 2 ) % 5;
        that.set_color( stroke_style, ink[ pen ][ 0 ], ink[ pen ][ 1 ],
                        ink[ pen ][ 2 ], ink[ pen ][ 3 ] );
        that.stroke();
        that.restore();
    }
}

static entry const tests[] = {
    { "path_construction", path_construction, 512, 512 },
    { "flattening_fill", flattening_fill, 512, 512 },
    { "fill_small", fill_small, 512, 512 },
    { "fill_large", fill_large, 1024, 1024 },
    { "fill_zone_plate", fill_zone_plate, 512, 512 },
    { "stroke_many", stroke_many, 512, 512 },
    { "stroke_wide", stroke_wide, 512, 512 },
    { "gradient_linear", gradient_linear, 512, 512 },
    { "gradient_radial", gradient_radial, 512, 512 },
    { "pattern_tiled", pattern_tiled, 512, 512 },
    { "image_scaled", image_scaled, 512, 512 },
    { "clip_heavy", clip_heavy, 512, 512 },
    { "composite_ops", composite_ops, 512, 512 },
    { "shadow_blurred", shadow_blurred, 512, 512 },
    { "transforms", transforms, 512, 512 },
    { "many_primitives", many_primitives, 512, 512 },
    { "complex_scene", complex_scene, 512, 512 },
};

// Minimal PNG writer (uncompressed deflate storage), same format as
// test/test.cpp's write_png.  Used only by the "render" mode below.
//
static void write_png(
    std::string const &filename,
    unsigned char const *image,
    int width,
    int height )
{
    std::ofstream output( filename.c_str(), std::ios::binary );
    unsigned table[ 256 ];
    for ( unsigned index = 0; index < 256; ++index )
    {
        unsigned value = index;
        for ( int step = 0; step < 8; ++step )
            value = ( value & 1 ? 0xedb88320u : 0u ) ^ ( value >> 1 );
        table[ index ] = value;
    }
    int idat_size = 6 + height * ( 6 + width * 4 );
    unsigned char header[] =
    {
        /*  0 */ 137, 80, 78, 71, 13, 10, 26, 10,
        /*  8 */ 0, 0, 0, 13, 73, 72, 68, 82,
        /* 16 */ static_cast< unsigned char >( width  >> 24 ),
        /* 17 */ static_cast< unsigned char >( width  >> 16 ),
        /* 18 */ static_cast< unsigned char >( width  >>  8 ),
        /* 19 */ static_cast< unsigned char >( width  >>  0 ),
        /* 20 */ static_cast< unsigned char >( height >> 24 ),
        /* 21 */ static_cast< unsigned char >( height >> 16 ),
        /* 22 */ static_cast< unsigned char >( height >>  8 ),
        /* 23 */ static_cast< unsigned char >( height >>  0 ),
        /* 24 */ 8, 6, 0, 0, 0,
        /* 29 */ 0, 0, 0, 0,
        /* 33 */ 0, 0, 0, 1, 115, 82, 71, 66,
        /* 41 */ 0,
        /* 42 */ 174, 206, 28, 233,
        /* 46 */ static_cast< unsigned char >( idat_size >> 24 ),
        /* 47 */ static_cast< unsigned char >( idat_size >> 16 ),
        /* 48 */ static_cast< unsigned char >( idat_size >>  8 ),
        /* 49 */ static_cast< unsigned char >( idat_size >>  0 ),
        /* 50 */ 73, 68, 65, 84,
        /* 54 */ 120, 1,
    };
    unsigned crc = ~0u;
    for ( int index = 12; index < 29; ++index )
        crc = table[ ( crc ^ header[ index ] ) & 0xff ] ^ ( crc >> 8 );
    header[ 29 ] = static_cast< unsigned char >( ~crc >> 24 );
    header[ 30 ] = static_cast< unsigned char >( ~crc >> 16 );
    header[ 31 ] = static_cast< unsigned char >( ~crc >>  8 );
    header[ 32 ] = static_cast< unsigned char >( ~crc >>  0 );
    output.write( reinterpret_cast< char * >( header ), 56 );
    crc = ~0u;
    for ( int index = 50; index < 56; ++index )
        crc = table[ ( crc ^ header[ index ] ) & 0xff ] ^ ( crc >> 8 );
    int check_1 = 1;
    int check_2 = 0;
    int row_size = 1 + width * 4;
    for ( int y = 0; y < height; ++y, image += width * 4 )
    {
        unsigned char prefix[] = {
            /* 0 */ static_cast< unsigned char >( y + 1 == height ),
            /* 1 */ static_cast< unsigned char >(  ( row_size >> 0 ) ),
            /* 2 */ static_cast< unsigned char >(  ( row_size >> 8 ) ),
            /* 3 */ static_cast< unsigned char >( ~( row_size >> 0 ) ),
            /* 4 */ static_cast< unsigned char >( ~( row_size >> 8 ) ),
            /* 5 */ 0,
        };
        output.write( reinterpret_cast< char * >( prefix ), 6 );
        for ( int index = 0; index < 6; ++index )
            crc = table[ ( crc ^ prefix[ index ] ) & 0xff ] ^ ( crc >> 8 );
        output.write( reinterpret_cast< char const * >( image ), width * 4 );
        check_2 = ( check_2 + check_1 ) % 65521;
        for ( int index = 0; index < width * 4; ++index )
        {
            check_1 = ( check_1 + image[ index ] ) % 65521;
            check_2 = ( check_2 + check_1 ) % 65521;
            crc = table[ ( crc ^ image[ index ] ) & 0xff ] ^ ( crc >> 8 );
        }
    }
    unsigned char footer[] = {
        /*  0 */ static_cast< unsigned char >( check_2 >> 8 ),
        /*  1 */ static_cast< unsigned char >( check_2 >> 0 ),
        /*  2 */ static_cast< unsigned char >( check_1 >> 8 ),
        /*  3 */ static_cast< unsigned char >( check_1 >> 0 ),
        /*  4 */ 0, 0, 0, 0,
        /*  8 */ 0, 0, 0, 0, 73, 69, 78, 68,
        /* 16 */ 174, 66, 96, 130,
    };
    for ( int index = 0; index < 4; ++index )
        crc = table[ ( crc ^ footer[ index ] ) & 0xff ] ^ ( crc >> 8 );
    footer[ 4 ] = static_cast< unsigned char >( ~crc >> 24 );
    footer[ 5 ] = static_cast< unsigned char >( ~crc >> 16 );
    footer[ 6 ] = static_cast< unsigned char >( ~crc >>  8 );
    footer[ 7 ] = static_cast< unsigned char >( ~crc >>  0 );
    output.write( reinterpret_cast< char * >( footer ), 20 );
}

// Render every workload once to OUTDIR/<name>.png.  Uses the exact
// per-workload seed of the timing path, so the picture is what the
// timer measures.  Usage: ./local_bench render OUTDIR
//
static int render_all( char const *outdir )
{
    int count = sizeof( tests ) / sizeof( tests[ 0 ] );
    for ( int index = 0; index < count; ++index )
    {
        entry const &item = tests[ index ];
        rng_state =
            0x243f6a8885a308d3ULL + index * 0x9e3779b97f4a7c15ULL;
        canvas that( item.width, item.height );
        item.call( that, item.width, item.height );
        std::vector< unsigned char > image(
            static_cast< size_t >( item.width ) *
            static_cast< size_t >( item.height ) * 4 );
        that.get_image_data( &image.front(), item.width, item.height,
                             item.width * 4, 0, 0 );
        std::string path = std::string( outdir ) + "/" + item.name + ".png";
        write_png( path, &image.front(), item.width, item.height );
    }
    return 0;
}

int main( int argc, char **argv )
{
    if ( argc > 1 && strcmp( argv[ 1 ], "render" ) == 0 && argc > 2 )
        return render_all( argv[ 2 ] );
    int trials = argc > 1 ? atoi( argv[ 1 ] ) : 7;
    if ( trials < 1 )
        trials = 1;
    char const *filter = argc > 2 ? argv[ 2 ] : 0;
    int count = sizeof( tests ) / sizeof( tests[ 0 ] );
    for ( int index = 0; index < count; ++index )
    {
        entry const &item = tests[ index ];
        if ( filter && strcmp( filter, item.name ) )
            continue;
        // Reseed before EVERY trial so best-of-N compares identical
        // work; otherwise trial-to-trial RNG drift (e.g. rotation-heavy
        // seeds in complex_scene) masquerades as machine noise.
        double best = 1.0e100;
        for ( int trial = 0; trial < trials; ++trial )
        {
            rng_state =
                0x243f6a8885a308d3ULL + index * 0x9e3779b97f4a7c15ULL;
            canvas that( item.width, item.height );
            double start = get_seconds();
            item.call( that, item.width, item.height );
            double end = get_seconds();
            if ( end - start < best )
                best = end - start;
        }
        printf( "%-20s %9.3fms\n", item.name, best * 1000.0 );
    }
    return 0;
}
