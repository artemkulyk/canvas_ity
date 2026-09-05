// Blend2D port of the canvas_ity microbenchmark suite.  The workloads,
// their RNG streams, scene geometry, and the timing methodology (best
// trial time on a fresh context, repeated) match bench/microbench.cpp
// exactly so the results are directly comparable.
//
// API-mapping notes and caveats:
// - Blend2D is immediate-mode like canvas_ity, so most workloads map
//   one-to-one.  Colors use straight-alpha BLRgba and let Blend2D do
//   its normal conversions.
// - shadow_blurred is omitted: Blend2D has no shadow equivalent.
// - clip_heavy is omitted: Blend2D supports rect clipping only, and
//   there is no path-based clip.
// - image_scaled uses a BLPattern with a scaling transform (the
//   idiomatic Blend2D scaled-image draw); Blend2D's default filter is
//   a bilinear-style lookup rather than canvas_ity's bicubic.
//
//   clang++ -O2 -I<blend2d-include-dir> -o microbench_blend2d \
//       microbench_blend2d.cpp libblend2d.a
//
//   ./microbench_blend2d [trials] [threads]
// Threads: 0 = synchronous single-threaded (default); N > 0 uses
// Blend2D's async job system with the user thread + N-1 pool workers.
// Renders are flushed with ctx.end() before the timer stops, so async
// completion is included in the measurement.

#define BLEND2D_STATIC
#include <blend2d.h>

#if defined( __linux__ )
#include <time.h>
#elif defined( _WIN32 )
#include <windows.h>
#elif defined( __MACH__ )
#include <mach/mach_time.h>
#else
#include <sys/time.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

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
    static mach_timebase_info_data_t frequency;
    mach_timebase_info( &frequency );
    return mach_absolute_time() * frequency.numer * 1.0e-9 /
           frequency.denom;
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

typedef void ( *workload )( BLContext &, int, int );
static BLRgba64 stop_color( double r, double g, double b, double a )
{
    return BLRgba64( static_cast< uint32_t >( r * 65535.0 ),
                     static_cast< uint32_t >( g * 65535.0 ),
                     static_cast< uint32_t >( b * 65535.0 ),
                     static_cast< uint32_t >( a * 65535.0 ) );
}


struct entry { char const *name; workload call; int width; int height; };

// Workloads -----------------------------------------------------------------

static void path_construction( BLContext &, int, int )
{
    BLPath path;
    // canvas_ity's begin_path() clears the path, so each repeat here
    // builds 50 subpaths into a cleared path.
    for ( int repeat = 0; repeat < 200; ++repeat )
    {
        path.clear();
        for ( int subpath = 0; subpath < 50; ++subpath )
        {
            path.move_to( frand() * 512.0, frand() * 512.0 );
            for ( int step = 0; step < 10; ++step )
            {
                path.line_to( frand() * 512.0, frand() * 512.0 );
                path.cubic_to( frand() * 512.0, frand() * 512.0,
                               frand() * 512.0, frand() * 512.0,
                               frand() * 512.0, frand() * 512.0 );
            }
        }
    }
}

static void flattening_fill( BLContext &ctx, int width, int height )
{
    BLPath path;
    for ( int subpath = 0; subpath < 60; ++subpath )
    {
        path.move_to( frand() * width, frand() * height );
        for ( int step = 0; step < 12; ++step )
            path.cubic_to( frand() * width, frand() * height,
                           frand() * width, frand() * height,
                           frand() * width, frand() * height );
        path.close();
    }
    ctx.set_fill_style( BLRgba( 0.2, 0.4, 0.8, 1.0 ) );
    ctx.fill_path( path );
}

static void fill_small( BLContext &ctx, int width, int height )
{
    ctx.set_fill_style( BLRgba( 0.2, 0.4, 0.8, 1.0 ) );
    for ( int step = 0; step < 2000; ++step )
    {
        float x = frand() * width;
        float y = frand() * height;
        float radius = 2.0f + frand() * 6.0f;
        BLPath path;
        path.add_ellipse( BLEllipse( x, y, radius, radius ) );
        ctx.fill_path( path );
    }
}

static void fill_large( BLContext &ctx, int width, int height )
{
    ctx.set_fill_style( BLRgba( 0.2, 0.4, 0.8, 1.0 ) );
    for ( int step = 0; step < 20; ++step )
        ctx.fill_rect( -10.0, -10.0, width + 20.0, height + 20.0 );
}

static void fill_zone_plate( BLContext &ctx, int width, int height )
{
    ctx.set_fill_style( BLRgba( 0.2, 0.4, 0.8, 1.0 ) );
    for ( int ring = 0; ring < 90; ++ring )
    {
        float cx = 0.5f * width;
        float cy = 0.5f * height;
        float radius = 3.0f + 3.0f * ring;
        BLPath path;
        path.add_ellipse( BLEllipse( cx, cy, radius, radius ) );
        ctx.fill_path( path );
    }
}

static void stroke_many( BLContext &ctx, int width, int height )
{
    ctx.set_stroke_width( 1.5 );
    ctx.set_stroke_style( BLRgba( 0.2, 0.4, 0.8, 1.0 ) );
    BLPath path;
    for ( int step = 0; step < 400; ++step )
    {
        path.move_to( frand() * width, frand() * height );
        path.cubic_to( frand() * width, frand() * height,
                       frand() * width, frand() * height,
                       frand() * width, frand() * height );
    }
    ctx.stroke_path( path );
}

static void stroke_wide( BLContext &ctx, int width, int height )
{
    ctx.set_stroke_width( 12.0 );
    ctx.set_stroke_join( BL_STROKE_JOIN_ROUND );
    ctx.set_stroke_style( BLRgba( 0.2, 0.4, 0.8, 1.0 ) );
    BLPath path;
    for ( int step = 0; step < 100; ++step )
    {
        path.move_to( frand() * width, frand() * height );
        for ( int part = 0; part < 6; ++part )
            path.cubic_to( frand() * width, frand() * height,
                           frand() * width, frand() * height,
                           frand() * width, frand() * height );
    }
    ctx.stroke_path( path );
}

static void gradient_linear( BLContext &ctx, int width, int height )
{
    BLGradient brush( BLLinearGradientValues( 0.0, 0.0, width, height ) );
    brush.add_stop( 0.0, stop_color( 1.0, 0.2, 0.1, 1.0 ) );
    brush.add_stop( 0.5, stop_color( 0.1, 0.9, 0.3, 1.0 ) );
    brush.add_stop( 1.0, stop_color( 0.2, 0.1, 1.0, 1.0 ) );
    ctx.set_fill_style( brush );
    for ( int step = 0; step < 10; ++step )
        ctx.fill_rect( 0.0, 0.0, width, height );
}

static void gradient_radial( BLContext &ctx, int width, int height )
{
    BLGradient brush( BLRadialGradientValues(
        0.5 * width, 0.5 * height, 0.3 * width, 0.4 * height, 2.0 ) );
    brush.add_stop( 0.0, stop_color( 1.0, 0.9, 0.2, 1.0 ) );
    brush.add_stop( 0.7, stop_color( 0.2, 0.5, 0.9, 0.8 ) );
    brush.add_stop( 1.0, stop_color( 0.1, 0.1, 0.4, 0.2 ) );
    ctx.set_fill_style( brush );
    for ( int step = 0; step < 10; ++step )
        ctx.fill_rect( 0.0, 0.0, width, height );
}

static void pattern_tiled( BLContext &ctx, int, int )
{
    // canvas_ity consumes no RNG for the pattern bytes; they are
    // deterministic, so the streams stay aligned either way.
    BLImage tile( 64, 64, BL_FORMAT_PRGB32 );
    {
        BLImageData data;
        tile.get_data( &data );
        for ( int y = 0; y < 64; ++y )
            for ( int x = 0; x < 64; ++x )
            {
                int index = y * 64 + x;
                unsigned char *pixel = static_cast< unsigned char * >(
                    data.pixel_data ) + y * data.stride + x * 4;
                pixel[ 0 ] = static_cast< unsigned char >( index * 28 + 0 );
                pixel[ 1 ] = static_cast< unsigned char >( index * 28 + 1 );
                pixel[ 2 ] = static_cast< unsigned char >( index * 28 + 2 );
                pixel[ 3 ] = static_cast< unsigned char >( index * 28 + 3 );
            }
    }
    BLPattern brush( tile, BL_EXTEND_MODE_REPEAT );
    ctx.set_fill_style( brush );
    for ( int step = 0; step < 10; ++step )
        ctx.fill_rect( 0.0, 0.0, 512.0, 512.0 );
}

static void image_scaled( BLContext &ctx, int width, int height )
{
    BLImage source( 128, 96, BL_FORMAT_PRGB32 );
    {
        BLImageData data;
        source.get_data( &data );
        for ( int y = 0; y < 96; ++y )
            for ( int x = 0; x < 128; ++x )
            {
                int index = y * 128 + x;
                unsigned char *pixel = static_cast< unsigned char * >(
                    data.pixel_data ) + y * data.stride + x * 4;
                pixel[ 0 ] = static_cast< unsigned char >( index * 124 + 0 );
                pixel[ 1 ] = static_cast< unsigned char >( index * 124 + 1 );
                pixel[ 2 ] = static_cast< unsigned char >( index * 124 + 2 );
                pixel[ 3 ] = static_cast< unsigned char >( index * 124 + 3 );
            }
    }
    BLPattern brush( source, BL_EXTEND_MODE_PAD );
    brush.set_transform( BLMatrix2D::make_scaling(
        width / 128.0, height / 96.0 ) );
    ctx.set_fill_style( brush );
    for ( int step = 0; step < 10; ++step )
        ctx.fill_rect( 0.0, 0.0, width, height );
}

static void composite_ops( BLContext &ctx, int width, int height )
{
    // The canvas_ity workload cycles composite operation values
    // 1..15 skipping 5, 6, 8, and 9.  All eleven map onto Blend2D's
    // BLCompOp set.
    static const BLCompOp operations[] = {
        BL_COMP_OP_SRC_IN,         // canvas_ity source_in
        BL_COMP_OP_SRC_COPY,       // canvas_ity source_copy
        BL_COMP_OP_SRC_OUT,        // canvas_ity source_out
        BL_COMP_OP_DST_IN,         // canvas_ity destination_in
        BL_COMP_OP_DST_ATOP,       // canvas_ity destination_atop
        BL_COMP_OP_PLUS,           // canvas_ity lighter
        BL_COMP_OP_DST_OVER,       // canvas_ity destination_over
        BL_COMP_OP_DST_OUT,        // canvas_ity destination_out
        BL_COMP_OP_SRC_ATOP,       // canvas_ity source_atop
        BL_COMP_OP_SRC_OVER,       // canvas_ity source_over
        BL_COMP_OP_XOR,            // canvas_ity exclusive_or
    };
    for ( size_t op = 0;
          op < sizeof( operations ) / sizeof( operations[ 0 ] ); ++op )
    {
        ctx.set_comp_op( operations[ op ] );
        ctx.set_fill_style( BLRgba( 0.9, 0.5, 0.2, 0.6 ) );
        ctx.fill_rect( 0.1 * width, 0.1 * height, 0.6 * width,
                       0.6 * height );
    }
}

static void transforms( BLContext &ctx, int width, int height )
{
    ctx.set_fill_style( BLRgba( 0.2, 0.4, 0.8, 1.0 ) );
    for ( int step = 0; step < 112; ++step )
    {
        ctx.save();
        ctx.translate( 0.5 * width, 0.5 * height );
        ctx.rotate( 0.1 * step );
        ctx.scale( 1.0 + 0.01 * step, 0.9 );
        ctx.translate( -0.5 * width, -0.5 * height );
        float cx = 0.4f * width;
        float cy = 0.4f * height;
        float radius = 0.3f * width;
        BLPath path;
        path.add_ellipse( BLEllipse( cx, cy, radius, radius ) );
        ctx.fill_path( path );
        ctx.restore();
    }
}

static void many_primitives( BLContext &ctx, int width, int height )
{
    // Canonical banded-ink scene: 4 frand() calls per primitive
    // (x, y, width, height); colors come from a fixed table.
    static float const ink[ 6 ][ 4 ] = {
        { 0.13f, 0.23f, 0.38f, 1.0f }, { 0.91f, 0.35f, 0.13f, 1.0f },
        { 0.87f, 0.62f, 0.13f, 1.0f }, { 0.13f, 0.55f, 0.55f, 1.0f },
        { 0.45f, 0.27f, 0.49f, 1.0f }, { 0.94f, 0.93f, 0.88f, 1.0f } };
    for ( int step = 0; step < 22000; ++step )
    {
        float x = frand() * width;
        float y = frand() * height;
        int band = ( ( int )( x * 0.05f + y * 0.03f ) % 6 + 6 ) % 6;
        ctx.set_fill_style( BLRgba( ink[ band ][ 0 ], ink[ band ][ 1 ],
                                    ink[ band ][ 2 ], ink[ band ][ 3 ] ) );
        ctx.fill_rect( x, y, 1.0f + frand() * 8.0f,
                       1.0f + frand() * 8.0f );
    }
}

static void complex_scene( BLContext &ctx, int width, int height )
{
    for ( int layer = 0; layer < 10; ++layer )
    {
        ctx.save();
        ctx.translate( frand() * 0.2 * width, frand() * 0.2 * height );
        ctx.rotate( frand() * 6.28 );
        if ( layer % 3 == 0 )
        {
            BLGradient brush( BLLinearGradientValues(
                0.0, 0.0, 0.5 * width, 0.5 * height ) );
            brush.add_stop( 0.0, stop_color( 1.0, 0.0, 0.0, 0.9 ) );
            brush.add_stop( 1.0, stop_color( 0.0, 0.0, 1.0, 0.5 ) );
            ctx.set_fill_style( brush );
        }
        else
            ctx.set_fill_style(
                BLRgba( frand(), frand(), frand(), frand() ) );
        BLPath path;
        for ( int subpath = 0; subpath < 12; ++subpath )
        {
            path.move_to( frand() * width, frand() * height );
            for ( int step = 0; step < 8; ++step )
                path.cubic_to( frand() * width, frand() * height,
                               frand() * width, frand() * height,
                               frand() * width, frand() * height );
            path.close();
        }
        ctx.fill_path( path );
        ctx.set_stroke_width( 1.0 + frand() * 3.0 );
        ctx.set_stroke_style( BLRgba( frand(), frand(), frand(), frand() ) );
        ctx.stroke_path( path );
        ctx.restore();
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
    { "clip_heavy", 0, 512, 512 },
    { "composite_ops", composite_ops, 512, 512 },
    { "shadow_blurred", 0, 512, 512 },
    { "transforms", transforms, 512, 512 },
    { "many_primitives", many_primitives, 512, 512 },
    { "complex_scene", complex_scene, 512, 512 },
};

int main( int argc, char **argv )
{
    setvbuf( stdout, nullptr, _IONBF, 0 );
    int trials = argc > 1 ? atoi( argv[ 1 ] ) : 15;
    int threads = argc > 2 ? atoi( argv[ 2 ] ) : 0;
    bool audit = getenv( "RNG_AUDIT" ) != 0;
    const int warmups = 3;
    int count = sizeof( tests ) / sizeof( tests[ 0 ] );
    double geo = 0.0;
    int supported = 0;
    bool announced = false;
    for ( int index = 0; index < count; ++index )
    {
        entry const &item = tests[ index ];
        if ( !item.call )
        {
            printf( "%-20s %9s\n", item.name, "n/a" );
            continue;
        }
        unsigned long long seed =
            0x243f6a8885a308d3ULL + index * 0x9e3779b97f4a7c15ULL;
        if ( audit )
        {
            rng_state = seed;
            BLImage target( item.width, item.height, BL_FORMAT_PRGB32 );
            BLContext ctx;
            BLContextCreateInfo cci{};
            cci.thread_count = static_cast< uint32_t >(
                threads < 0 ? 0 : threads );
            ctx.begin( target, cci );
            item.call( ctx, item.width, item.height );
            ctx.end();
            printf( "AUDIT %-18s %016llx\n", item.name, rng_state );
            continue;
        }
        std::vector< double > times;
        times.reserve( trials );
        for ( int trial = -warmups; trial < trials; ++trial )
        {
            rng_state = seed;   // identical scene every trial
            BLImage target( item.width, item.height, BL_FORMAT_PRGB32 );
            BLContext ctx;
            BLContextCreateInfo cci{};
            cci.thread_count = static_cast< uint32_t >(
                threads < 0 ? 0 : threads );
            ctx.begin( target, cci );
            if ( !announced )
            {
                fprintf( stderr, "threads requested=%d effective=%u\n",
                         threads, ctx.thread_count() );
                announced = true;
            }
            double start = get_seconds();
            item.call( ctx, item.width, item.height );
            // In async (threaded) mode rendering is queued: end()
            // performs the SYNC flush, so it must precede the timer.
            // In sync mode end() is a trivial detach.
            ctx.end();
            double end = get_seconds();
            if ( trial >= 0 )
                times.push_back( end - start );
        }
        std::sort( times.begin(), times.end() );
        geo += log( times.front() );
        ++supported;
        printf( "%-20s %9.3f %9.3f\n", item.name,
                times.front() * 1000.0,
                times[ times.size() / 2 ] * 1000.0 );
    }
    printf( "%-20s %9.3f\n", "geo mean",
            exp( geo / supported ) * 1000.0 );
    return 0;
}
