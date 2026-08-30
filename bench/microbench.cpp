// Microbenchmark suite for canvas_ity.  Used locally and by
// .github/workflows/bench.yml (via bench/ci_bench.py).
// Each workload is an isolated, representative rendering operation run
// repeatedly on a fresh canvas; the best time per iteration is reported.
//
//   c++ -O2 -std=c++11 -Isrc -o microbench bench/microbench.cpp
//   ./microbench [trials] [workload]

#define CANVAS_ITY_IMPLEMENTATION
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
static float frange( float low, float high )
{
    return low + frand() * ( high - low );
}

typedef void ( *workload )( canvas &that, int width, int height );

struct entry { char const *name; workload call; int width; int height; };

// Workloads -----------------------------------------------------------------

static void path_construction( canvas &that, int, int )
{
    for ( int repeat = 0; repeat < 200; ++repeat )
    {
        that.begin_path();
        for ( int subpath = 0; subpath < 50; ++subpath )
        {
            that.move_to( frand() * 512.0f, frand() * 512.0f );
            for ( int step = 0; step < 10; ++step )
            {
                that.line_to( frand() * 512.0f, frand() * 512.0f );
                that.bezier_curve_to( frand() * 512.0f, frand() * 512.0f,
                                      frand() * 512.0f, frand() * 512.0f,
                                      frand() * 512.0f, frand() * 512.0f );
            }
        }
    }
}

static void flattening_fill( canvas &that, int width, int height )
{
    // One path of many highly-curved beziers: dominated by flattening.
    that.begin_path();
    for ( int subpath = 0; subpath < 60; ++subpath )
    {
        that.move_to( frand() * width, frand() * height );
        for ( int step = 0; step < 12; ++step )
            that.bezier_curve_to( frand() * width, frand() * height,
                                  frand() * width, frand() * height,
                                  frand() * width, frand() * height );
        that.close_path();
    }
    that.fill();
}

static void fill_small( canvas &that, int width, int height )
{
    for ( int step = 0; step < 2000; ++step )
    {
        that.begin_path();
        that.arc( frand() * width, frand() * height, 2.0f + frand() * 6.0f,
                  0.0f, 6.2832f, false );
        that.fill();
    }
}

static void fill_large( canvas &that, int width, int height )
{
    that.set_color( fill_style, 0.2f, 0.4f, 0.8f, 1.0f );
    for ( int step = 0; step < 20; ++step )
        that.fill_rectangle( -10.0f, -10.0f,
                             static_cast< float >( width ) + 20.0f,
                             static_cast< float >( height ) + 20.0f );
}

static void fill_zone_plate( canvas &that, int width, int height )
{
    // Concentric rings: extreme anti-aliasing workload.
    for ( int ring = 0; ring < 90; ++ring )
    {
        that.begin_path();
        that.arc( 0.5f * width, 0.5f * height, 3.0f + 3.0f * ring,
                  0.0f, 6.2832f, false );
        that.fill();
    }
}

static void stroke_many( canvas &that, int width, int height )
{
    that.set_line_width( 1.5f );
    that.begin_path();
    for ( int step = 0; step < 400; ++step )
    {
        that.move_to( frand() * width, frand() * height );
        that.bezier_curve_to( frand() * width, frand() * height,
                              frand() * width, frand() * height,
                              frand() * width, frand() * height );
    }
    that.stroke();
}

static void stroke_wide( canvas &that, int width, int height )
{
    that.set_line_width( 12.0f );
    that.line_join = rounded;
    that.begin_path();
    for ( int step = 0; step < 100; ++step )
    {
        that.move_to( frand() * width, frand() * height );
        for ( int part = 0; part < 6; ++part )
            that.bezier_curve_to( frand() * width, frand() * height,
                                  frand() * width, frand() * height,
                                  frand() * width, frand() * height );
    }
    that.stroke();
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
    std::vector< unsigned char > image( 64 * 64 * 4 );
    for ( size_t index = 0; index < image.size(); ++index )
        image[ index ] = static_cast< unsigned char >( index * 7 );
    that.set_pattern( fill_style, &image.front(), 64, 64, 64 * 4, repeat );
    for ( int step = 0; step < 10; ++step )
        that.fill_rectangle( 0.0f, 0.0f,
                             static_cast< float >( width ),
                             static_cast< float >( height ) );
}

static void image_scaled( canvas &that, int width, int height )
{
    std::vector< unsigned char > image( 128 * 96 * 4 );
    for ( size_t index = 0; index < image.size(); ++index )
        image[ index ] = static_cast< unsigned char >( index * 31 );
    for ( int step = 0; step < 10; ++step )
        that.draw_image( &image.front(), 128, 96, 128 * 4,
                         0.0f, 0.0f, static_cast< float >( width ),
                         static_cast< float >( height ) );
}

static void clip_heavy( canvas &that, int width, int height )
{
    that.begin_path();
    for ( int step = 0; step < 8; ++step )
    {
        float cx = frange( 0.25f, 0.75f ) * width;
        float cy = frange( 0.25f, 0.75f ) * height;
        that.arc( cx, cy, 0.3f * width, frand() * 6.28f,
                  frand() * 6.28f + 6.28f, frand() < 0.5f );
    }
    that.clip();
    for ( int step = 0; step < 30; ++step )
    {
        that.set_color( fill_style, frand(), frand(), frand(), frand() );
        that.begin_path();
        that.arc( frand() * width, frand() * height,
                  frand() * 0.4f * width, 0.0f, 6.2832f, false );
        that.fill();
    }
}

static void composite_ops( canvas &that, int width, int height )
{
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
    for ( int step = 0; step < 40; ++step )
    {
        that.save();
        that.translate( 0.5f * width, 0.5f * height );
        that.rotate( 0.1f * step );
        that.scale( 1.0f + 0.01f * step, 0.9f );
        that.translate( -0.5f * width, -0.5f * height );
        that.begin_path();
        that.arc( 0.4f * width, 0.4f * height, 0.3f * width,
                  0.0f, 6.2832f, false );
        that.fill();
        that.restore();
    }
}

static void many_primitives( canvas &that, int width, int height )
{
    for ( int step = 0; step < 1500; ++step )
    {
        that.set_color( fill_style, frand(), frand(), frand(), frand() );
        that.fill_rectangle( frand() * width, frand() * height,
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
            that.set_color( fill_style, frand(), frand(), frand(),
                            frand() );
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
        that.set_color( stroke_style, frand(), frand(), frand(), frand() );
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

int main( int argc, char **argv )
{
    int trials = argc > 1 ? atoi( argv[ 1 ] ) : 7;
    char const *filter = argc > 2 ? argv[ 2 ] : 0;
    int count = sizeof( tests ) / sizeof( tests[ 0 ] );
    double geo = 0.0;
    int measured = 0;
    for ( int index = 0; index < count; ++index )
    {
        entry const &item = tests[ index ];
        if ( filter && strcmp( filter, item.name ) )
            continue;
        rng_state = 0x243f6a8885a308d3ULL + index * 0x9e3779b97f4a7c15ULL;
        double best = 1.0e100;
        for ( int trial = 0; trial < trials; ++trial )
        {
            canvas that( item.width, item.height );
            double start = get_seconds();
            item.call( that, item.width, item.height );
            double end = get_seconds();
            if ( end - start < best )
                best = end - start;
        }
        geo += log( best );
        ++measured;
        printf( "%-20s %9.3fms\n", item.name, best * 1000.0 );
    }
    if ( !measured )
    {
        fprintf( stderr, "unknown workload: %s\n", filter );
        return 1;
    }
    printf( "%-20s %9.3fms\n", "geo mean",
            exp( geo / measured ) * 1000.0 );
    return 0;
}
