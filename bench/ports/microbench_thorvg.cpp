// ThorVG port of the canvas_ity microbenchmark suite.  The workloads,
// their RNG streams, scene geometry, and the timing methodology (best
// trial time on a fresh canvas, repeated) match bench/microbench.cpp
// exactly so the results are directly comparable.
//
// API-mapping notes and caveats:
// - ThorVG is a retained-mode scene graph: a fresh SwCanvas is created
//   per trial (matching the fresh canvas per trial of the original
//   harness) and each workload builds its whole scene, then renders
//   with one draw()+sync() pair inside the timed region.
// - Single-threaded: Initializer::init(0) and no setThreads calls, so
//   all three libraries are compared single-threaded.
// - shadow_blurred is omitted: no shadow equivalent in the public API.
// - clip_heavy uses ThorVG's alpha masking (Paint::mask with a scene of
//   circles) as the closest equivalent to a clip region; the arcs of
//   the original become full circles since ThorVG has no partial arcs
//   in its path API (RNG calls are consumed identically to keep the
//   geometry stream aligned).
// - pattern_tiled has no pattern paint in ThorVG; it is implemented as
//   64 picture tiles (one 64x64 tile repeated) per full-canvas fill,
//   i.e., ten layers of 64 pictures.
// - image_scaled draws ten pictures scaled to the canvas with ThorVG's
//   default Bilinear filter; canvas_ity uses a bicubic filter.
// - composite_ops: ThorVG only supports SVG/CSS blend modes, not the
//   Porter-Duff set.  Only canvas_ity's source_over (Normal) and
//   lighter (Add) map; the other nine are skipped.
//
//   clang++ -O2 -I<thorvg-inc-dir> -o microbench_thorvg \
//       microbench_thorvg.cpp -L<thorvg-lib-dir> -lthorvg-1
//
//   ./microbench_thorvg [trials]

#include "thorvg.h"

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

using namespace tvg;

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

typedef void ( *workload )( SwCanvas &, int, int );

struct entry { char const *name; workload call; int width; int height; };

// Helpers -------------------------------------------------------------------

// Fill a shape with a three-stop linear gradient from canvas_ity-style
// normalized color stops.  The fill takes ownership of the gradient.
static Fill *make_linear( float x1, float y1, float x2, float y2,
                          const float ( &stops )[ 3 ][ 5 ] )
{
    auto *brush = LinearGradient::gen();
    brush->linear( x1, y1, x2, y2 );
    Fill::ColorStop list[ 3 ];
    for ( int index = 0; index < 3; ++index )
    {
        list[ index ].offset = stops[ index ][ 0 ];
        list[ index ].r = static_cast< uint8_t >( stops[ index ][ 1 ] *
                                                  255.0f );
        list[ index ].g = static_cast< uint8_t >( stops[ index ][ 2 ] *
                                                  255.0f );
        list[ index ].b = static_cast< uint8_t >( stops[ index ][ 3 ] *
                                                  255.0f );
        list[ index ].a = static_cast< uint8_t >( stops[ index ][ 4 ] *
                                                  255.0f );
    }
    brush->colorStops( list, 3 );
    return brush;
}

static Fill *make_radial( float cx, float cy, float radius, float fx,
                          float fy, float fradius,
                          const float ( &stops )[ 3 ][ 5 ] )
{
    auto *brush = RadialGradient::gen();
    brush->radial( cx, cy, radius, fx, fy, fradius );
    Fill::ColorStop list[ 3 ];
    for ( int index = 0; index < 3; ++index )
    {
        list[ index ].offset = stops[ index ][ 0 ];
        list[ index ].r = static_cast< uint8_t >( stops[ index ][ 1 ] *
                                                  255.0f );
        list[ index ].g = static_cast< uint8_t >( stops[ index ][ 2 ] *
                                                  255.0f );
        list[ index ].b = static_cast< uint8_t >( stops[ index ][ 3 ] *
                                                  255.0f );
        list[ index ].a = static_cast< uint8_t >( stops[ index ][ 4 ] *
                                                  255.0f );
    }
    brush->colorStops( list, 3 );
    return brush;
}

// 0-255 color from canvas_ity-style normalized components.
static uint8_t channel( float value )
{
    return static_cast< uint8_t >( value * 255.0f );
}

// The affine matrix M = T(a) . R(deg) . S(sx, sy) . T(b) in ThorVG's
// convention (x' = e11 x + e12 y + e13), matching the canvas_ity
// save/translate/rotate/scale/translate sequence and its y-down,
// clockwise-positive rotation direction.
static Matrix compose( float ax, float ay, float degrees, float sx,
                       float sy, float bx, float by )
{
    float radians = degrees * 3.14159265f / 180.0f;
    float c = cosf( radians ), s = sinf( radians );
    Matrix matrix;
    matrix.e11 = c * sx;
    matrix.e12 = -s * sx;
    matrix.e21 = s * sy;
    matrix.e22 = c * sy;
    matrix.e13 = c * sx * ax - s * sx * ay + bx;
    matrix.e23 = s * sy * ax + c * sy * ay + by;
    return matrix;
}

// The affine matrix M = T(a) . R(deg) in ThorVG's convention.
static Matrix translate_rotate( float ax, float ay, float degrees )
{
    float radians = degrees * 3.14159265f / 180.0f;
    float c = cosf( radians ), s = sinf( radians );
    Matrix matrix;
    matrix.e11 = c;
    matrix.e12 = -s;
    matrix.e21 = s;
    matrix.e22 = c;
    matrix.e13 = ax;
    matrix.e23 = ay;
    return matrix;
}

// Workloads -----------------------------------------------------------------

static void path_construction( SwCanvas &, int, int )
{
    // canvas_ity's begin_path() clears the path, so each repeat builds
    // 50 subpaths into a fresh shape here as well.
    for ( int repeat = 0; repeat < 200; ++repeat )
    {
        Shape *shape = Shape::gen();
        for ( int subpath = 0; subpath < 50; ++subpath )
        {
            shape->moveTo( frand() * 512.0f, frand() * 512.0f );
            for ( int step = 0; step < 10; ++step )
            {
                shape->lineTo( frand() * 512.0f, frand() * 512.0f );
                shape->cubicTo( frand() * 512.0f, frand() * 512.0f,
                                frand() * 512.0f, frand() * 512.0f,
                                frand() * 512.0f, frand() * 512.0f );
            }
        }
        shape->unref();
    }
}

static void flattening_fill( SwCanvas &canvas, int width, int height )
{
    Shape *shape = Shape::gen();
    for ( int subpath = 0; subpath < 60; ++subpath )
    {
        shape->moveTo( frand() * width, frand() * height );
        for ( int step = 0; step < 12; ++step )
            shape->cubicTo( frand() * width, frand() * height,
                            frand() * width, frand() * height,
                            frand() * width, frand() * height );
        shape->close();
    }
    shape->fill( 51, 102, 204, 255 );
    canvas.add( shape );
}

static void fill_small( SwCanvas &canvas, int width, int height )
{
    for ( int step = 0; step < 2000; ++step )
    {
        float x = frand() * width;
        float y = frand() * height;
        float radius = 2.0f + frand() * 6.0f;
        Shape *shape = Shape::gen();
        shape->appendCircle( x, y, radius, radius );
        shape->fill( 51, 102, 204, 255 );
        canvas.add( shape );
    }
}

static void fill_large( SwCanvas &canvas, int width, int height )
{
    for ( int step = 0; step < 20; ++step )
    {
        Shape *shape = Shape::gen();
        shape->appendRect( -10.0f, -10.0f, width + 20.0f, height + 20.0f );
        shape->fill( 51, 102, 204, 255 );
        canvas.add( shape );
    }
}

static void fill_zone_plate( SwCanvas &canvas, int width, int height )
{
    for ( int ring = 0; ring < 90; ++ring )
    {
        float cx = 0.5f * width;
        float cy = 0.5f * height;
        float radius = 3.0f + 3.0f * ring;
        Shape *shape = Shape::gen();
        shape->appendCircle( cx, cy, radius, radius );
        shape->fill( 51, 102, 204, 255 );
        canvas.add( shape );
    }
}

static void stroke_many( SwCanvas &canvas, int width, int height )
{
    Shape *shape = Shape::gen();
    for ( int step = 0; step < 400; ++step )
    {
        shape->moveTo( frand() * width, frand() * height );
        shape->cubicTo( frand() * width, frand() * height,
                        frand() * width, frand() * height,
                        frand() * width, frand() * height );
    }
    shape->strokeWidth( 1.5f );
    shape->strokeFill( 51, 102, 204, 255 );
    canvas.add( shape );
}

static void stroke_wide( SwCanvas &canvas, int width, int height )
{
    Shape *shape = Shape::gen();
    for ( int step = 0; step < 100; ++step )
    {
        shape->moveTo( frand() * width, frand() * height );
        for ( int part = 0; part < 6; ++part )
            shape->cubicTo( frand() * width, frand() * height,
                            frand() * width, frand() * height,
                            frand() * width, frand() * height );
    }
    shape->strokeWidth( 12.0f );
    shape->strokeJoin( StrokeJoin::Round );
    shape->strokeFill( 51, 102, 204, 255 );
    canvas.add( shape );
}

static void gradient_linear( SwCanvas &canvas, int width, int height )
{
    static const float stops[ 3 ][ 5 ] = {
        { 0.0f, 1.0f, 0.2f, 0.1f, 1.0f },
        { 0.5f, 0.1f, 0.9f, 0.3f, 1.0f },
        { 1.0f, 0.2f, 0.1f, 1.0f, 1.0f } };
    for ( int step = 0; step < 10; ++step )
    {
        Shape *shape = Shape::gen();
        shape->appendRect( 0.0f, 0.0f, width, height );
        shape->fill( make_linear( 0.0f, 0.0f,
                                  static_cast< float >( width ),
                                  static_cast< float >( height ),
                                  stops ) );
        canvas.add( shape );
    }
}

static void gradient_radial( SwCanvas &canvas, int width, int height )
{
    static const float stops[ 3 ][ 5 ] = {
        { 0.0f, 1.0f, 0.9f, 0.2f, 1.0f },
        { 0.7f, 0.2f, 0.5f, 0.9f, 0.8f },
        { 1.0f, 0.1f, 0.1f, 0.4f, 0.2f } };
    for ( int step = 0; step < 10; ++step )
    {
        Shape *shape = Shape::gen();
        shape->appendRect( 0.0f, 0.0f, width, height );
        shape->fill( make_radial( 0.5f * width, 0.5f * height,
                                  0.6f * width, 0.3f * width,
                                  0.4f * height, 2.0f, stops ) );
        canvas.add( shape );
    }
}

static void pattern_tiled( SwCanvas &canvas, int width, int height )
{
    // ThorVG has no pattern paint, so the 64x64 tile becomes 64
    // picture instances per full-canvas fill, ten fills in total.
    std::vector< uint32_t > pixels( 64 * 64 );
    for ( size_t index = 0; index < pixels.size(); ++index )
    {
        unsigned char value = static_cast< unsigned char >( index * 7 );
        pixels[ index ] = static_cast< uint32_t >( value ) << 24 |
                          static_cast< uint32_t >( value ) << 16 |
                          static_cast< uint32_t >( value ) << 8 |
                          static_cast< uint32_t >( value );
    }
    int tiles = ( width + 63 ) / 64;
    for ( int step = 0; step < 10; ++step )
        for ( int ty = 0; ty < tiles; ++ty )
            for ( int tx = 0; tx < tiles; ++tx )
            {
                Picture *tile = Picture::gen();
                tile->load( &pixels.front(), 64, 64,
                            ColorSpace::ARGB8888 );
                tile->translate( tx * 64.0f, ty * 64.0f );
                canvas.add( tile );
            }
}

static void image_scaled( SwCanvas &canvas, int width, int height )
{
    std::vector< uint32_t > pixels( 128 * 96 );
    for ( size_t index = 0; index < pixels.size(); ++index )
    {
        unsigned char value = static_cast< unsigned char >( index * 31 );
        pixels[ index ] = static_cast< uint32_t >( value ) << 24 |
                          static_cast< uint32_t >( value ) << 16 |
                          static_cast< uint32_t >( value ) << 8 |
                          static_cast< uint32_t >( value );
    }
    for ( int step = 0; step < 10; ++step )
    {
        Picture *picture = Picture::gen();
        picture->load( &pixels.front(), 128, 96, ColorSpace::ARGB8888 );
        picture->size( width, height );
        canvas.add( picture );
    }
}

static void clip_heavy( SwCanvas &canvas, int width, int height )
{
    // The clip region of eight overlapping arcs (becoming circles here)
    // is realized as an alpha mask shared by thirty filled circles.
    Scene *mask = Scene::gen();
    for ( int step = 0; step < 8; ++step )
    {
        float cx = frange( 0.25f, 0.75f ) * width;
        float cy = frange( 0.25f, 0.75f ) * height;
        float start = frand() * 6.28f;
        float sweep = frand() * 6.28f + 6.28f;
        bool anticlockwise = frand() < 0.5f;
        ( void ) start; ( void ) sweep; ( void ) anticlockwise;
        Shape *shape = Shape::gen();
        shape->appendCircle( cx, cy, 0.3f * width, 0.3f * width );
        shape->fill( 255, 255, 255, 255 );
        mask->add( shape );
    }
    for ( int step = 0; step < 30; ++step )
    {
        float red = frand(), green = frand(), blue = frand();
        float alpha = frand();
        float cx = frand() * width;
        float cy = frand() * height;
        float radius = frand() * 0.4f * width;
        Shape *shape = Shape::gen();
        shape->appendCircle( cx, cy, radius, radius );
        shape->fill( channel( red ), channel( green ), channel( blue ),
                     channel( alpha ) );
        shape->mask( mask, MaskMethod::Alpha );
        canvas.add( shape );
    }
    // The mask scene is owned by the thirty masking shapes (Paint::mask
    // references it); they free it when the canvas releases them.
    ( void ) mask;
}

static void composite_ops( SwCanvas &canvas, int width, int height )
{
    // ThorVG supports only SVG/CSS blend methods, not the Porter-Duff
    // composite set.  Only source_over and lighter map; the workload
    // runs with those two.
    static const BlendMethod operations[] = { BlendMethod::Normal,
                                              BlendMethod::Add };
    for ( size_t op = 0;
          op < sizeof( operations ) / sizeof( operations[ 0 ] ); ++op )
    {
        Shape *shape = Shape::gen();
        shape->appendRect( 0.1f * width, 0.1f * height, 0.6f * width,
                           0.6f * height );
        shape->fill( 229, 127, 51, 153 );
        shape->blend( operations[ op ] );
        canvas.add( shape );
    }
}

static void transforms( SwCanvas &canvas, int width, int height )
{
    for ( int step = 0; step < 112; ++step )
    {
        float cx = 0.4f * width;
        float cy = 0.4f * height;
        float radius = 0.3f * width;
        Shape *shape = Shape::gen();
        shape->appendCircle( cx, cy, radius, radius );
        shape->fill( 51, 102, 204, 255 );
        shape->transform( compose( 0.5f * width, 0.5f * height,
                                      0.1f * step * 180.0f /
                                          3.14159265f,
                                      1.0f + 0.01f * step, 0.9f,
                                      -0.5f * width, -0.5f * height ) );
        canvas.add( shape );
    }
}

static void many_primitives( SwCanvas &canvas, int width, int height )
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
        Shape *shape = Shape::gen();
        shape->appendRect( x, y, 1.0f + frand() * 8.0f,
                           1.0f + frand() * 8.0f );
        shape->fill( channel( ink[ band ][ 0 ] ),
                     channel( ink[ band ][ 1 ] ),
                     channel( ink[ band ][ 2 ] ),
                     channel( ink[ band ][ 3 ] ) );
        canvas.add( shape );
    }
}

static void complex_scene( SwCanvas &canvas, int width, int height )
{
    static const float stops[ 2 ][ 5 ] = {
        { 0.0f, 1.0f, 0.0f, 0.0f, 0.9f },
        { 1.0f, 0.0f, 0.0f, 1.0f, 0.5f } };
    for ( int layer = 0; layer < 10; ++layer )
    {
        float tx = frand() * 0.2f * width;
        float ty = frand() * 0.2f * height;
        float degrees = frand() * 6.28f * 180.0f / 3.14159265f;
        Shape *shape = Shape::gen();
        if ( layer % 3 == 0 )
        {
            auto *brush = LinearGradient::gen();
            brush->linear( 0.0f, 0.0f, 0.5f * width, 0.5f * height );
            Fill::ColorStop list[ 2 ];
            for ( int index = 0; index < 2; ++index )
            {
                list[ index ].offset = stops[ index ][ 0 ];
                list[ index ].r = channel( stops[ index ][ 1 ] );
                list[ index ].g = channel( stops[ index ][ 2 ] );
                list[ index ].b = channel( stops[ index ][ 3 ] );
                list[ index ].a = channel( stops[ index ][ 4 ] );
            }
            brush->colorStops( list, 2 );
            shape->fill( brush );
        }
        else
        {
            float red = frand(), green = frand(), blue = frand();
            float alpha = frand();
            shape->fill( channel( red ), channel( green ),
                         channel( blue ), channel( alpha ) );
        }
        for ( int subpath = 0; subpath < 12; ++subpath )
        {
            shape->moveTo( frand() * width, frand() * height );
            for ( int step = 0; step < 8; ++step )
                shape->cubicTo( frand() * width, frand() * height,
                                frand() * width, frand() * height,
                                frand() * width, frand() * height );
            shape->close();
        }
        shape->transform( translate_rotate( tx, ty, degrees ) );
        shape->strokeWidth( 1.0f + frand() * 3.0f );
        float red = frand(), green = frand(), blue = frand();
        float alpha = frand();
        shape->strokeFill( channel( red ), channel( green ),
                           channel( blue ), channel( alpha ) );
        canvas.add( shape );
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
    { "shadow_blurred", 0, 512, 512 },
    { "transforms", transforms, 512, 512 },
    { "many_primitives", many_primitives, 512, 512 },
    { "complex_scene", complex_scene, 512, 512 },
};

int main( int argc, char **argv )
{
    setvbuf( stdout, nullptr, _IONBF, 0 );
    int trials = argc > 1 ? atoi( argv[ 1 ] ) : 15;
    bool audit = getenv( "RNG_AUDIT" ) != 0;
    const int warmups = 3;
    int count = sizeof( tests ) / sizeof( tests[ 0 ] );
    double geo = 0.0;
    int supported = 0;
    Initializer::init( 0 );
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
            std::vector< uint32_t > pixels(
                static_cast< size_t >( item.width ) * item.height, 0u );
            SwCanvas *canvas = SwCanvas::gen();
            canvas->target( &pixels.front(),
                            static_cast< uint32_t >( item.width ),
                            static_cast< uint32_t >( item.width ),
                            static_cast< uint32_t >( item.height ),
                            ColorSpace::ARGB8888 );
            item.call( *canvas, item.width, item.height );
            canvas->draw();
            canvas->sync();
            delete canvas;
            printf( "AUDIT %-18s %016llx\n", item.name, rng_state );
            continue;
        }
        std::vector< double > times;
        times.reserve( trials );
        for ( int trial = -warmups; trial < trials; ++trial )
        {
            rng_state = seed;   // identical scene every trial
            std::vector< uint32_t > pixels(
                static_cast< size_t >( item.width ) * item.height, 0u );
            SwCanvas *canvas = SwCanvas::gen();
            canvas->target( &pixels.front(),
                            static_cast< uint32_t >( item.width ),
                            static_cast< uint32_t >( item.width ),
                            static_cast< uint32_t >( item.height ),
                            ColorSpace::ARGB8888 );
            double start = get_seconds();
            item.call( *canvas, item.width, item.height );
            canvas->draw();
            canvas->sync();
            double end = get_seconds();
            unsigned long long total = 0;
            for ( size_t p = 0; p < pixels.size(); ++p )
                total += pixels[ p ] & 0xffffffu;
            if ( trial == 0 && total == 0 )
                printf( "warning: %s produced an empty buffer\n",
                        item.name );
            if ( trial >= 0 )
                times.push_back( end - start );
            delete canvas;
        }
        std::sort( times.begin(), times.end() );
        geo += log( times.front() );
        ++supported;
        printf( "%-20s %9.3f %9.3f\n", item.name,
                times.front() * 1000.0,
                times[ times.size() / 2 ] * 1000.0 );
    }
    Initializer::term();
    printf( "%-20s %9.3f\n", "geo mean",
            exp( geo / supported ) * 1000.0 );
    return 0;
}
