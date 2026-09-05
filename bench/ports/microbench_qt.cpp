// Qt QPainter raster port of the canvas_ity microbenchmark suite.
// Workloads, RNG streams, scene geometry, and timing methodology
// (best trial time on a fresh target, repeated) match
// bench/microbench.cpp so the results are directly comparable.
//
// API-mapping notes and caveats:
// - Qt is immediate-mode like canvas_ity, so most workloads map
//   one-to-one.  Colors use QColor::fromRgbF and let Qt do its
//   normal conversions.
// - shadow_blurred is omitted: QPainter has no shadow equivalent.
// - clip_heavy is approximated: QPainter supports only rect clips
//   plus path clip via QPainterPath::subtracted/intersected, and
//   the cost model differs; it is included with a rect-union clip
//   as the closest feasible analogue (marked APPROX below).
// - image_scaled draws a QImage scaled to the canvas with
//   Qt::SmoothTransformation; canvas_ity uses a bicubic filter.
// - pattern_tiled uses a repeating QBrush texture; canvas_ity uses
//   a bicubic pattern sampler.
// - composite_ops: canvas_ity's 11 ops map onto QPainter composition
//   modes; lighter maps to CompositionMode_Plus.
// - Qt requires a QGuiApplication/QCoreApplication object; we use
//   QCoreApplication with QT_QPA_PLATFORM=offscreen so no display
//   or GPU is involved (software raster path).
//
//   c++ -O2 -std=c++17 -F<qt-frameworks> -o microbench_qt \
//       microbench_qt.cpp -framework QtGui -framework QtCore
//
//   QT_QPA_PLATFORM=offscreen ./microbench_qt [trials]

#include <QtGui/QPainter>
#include <QtGui/QImage>
#include <QtGui/QBrush>
#include <QtGui/QPen>
#include <QtGui/QPainterPath>
#include <QtGui/QLinearGradient>
#include <QtGui/QRadialGradient>
#include <QtCore/QCoreApplication>
#include <algorithm>
#include <cstdlib>
#include <vector>

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
#include <cstdio>
#include <cstdlib>

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

typedef void ( *workload )( QPainter &, int, int );

struct entry { char const *name; workload call; int width; int height; };

// Workloads -----------------------------------------------------------------

static void path_construction( QPainter &, int, int )
{
    // canvas_ity's begin_path() clears the path, so each repeat here
    // builds 50 subpaths into a cleared path.
    QPainterPath path;
    for ( int repeat = 0; repeat < 200; ++repeat )
    {
        path.clear();
        for ( int subpath = 0; subpath < 50; ++subpath )
        {
            path.moveTo( frand() * 512.0, frand() * 512.0 );
            for ( int step = 0; step < 10; ++step )
            {
                path.lineTo( frand() * 512.0, frand() * 512.0 );
                path.cubicTo( frand() * 512.0, frand() * 512.0,
                              frand() * 512.0, frand() * 512.0,
                              frand() * 512.0, frand() * 512.0 );
            }
        }
    }
}

static void flattening_fill( QPainter &ctx, int width, int height )
{
    QPainterPath path;
    for ( int subpath = 0; subpath < 60; ++subpath )
    {
        path.moveTo( frand() * width, frand() * height );
        for ( int step = 0; step < 12; ++step )
            path.cubicTo( frand() * width, frand() * height,
                          frand() * width, frand() * height,
                          frand() * width, frand() * height );
        path.closeSubpath();
    }
    ctx.setBrush( QColor::fromRgbF( 0.2, 0.4, 0.8, 1.0 ) );
    ctx.setPen( Qt::NoPen );
    ctx.drawPath( path );
}

static void fill_small( QPainter &ctx, int width, int height )
{
    ctx.setBrush( QColor::fromRgbF( 0.2, 0.4, 0.8, 1.0 ) );
    ctx.setPen( Qt::NoPen );
    for ( int step = 0; step < 2000; ++step )
    {
        float x = frand() * width;
        float y = frand() * height;
        float radius = 2.0f + frand() * 6.0f;
        QPainterPath path;
        path.addEllipse( x - radius, y - radius,
                         2.0f * radius, 2.0f * radius );
        ctx.drawPath( path );
    }
}

static void fill_large( QPainter &ctx, int width, int height )
{
    ctx.setBrush( QColor::fromRgbF( 0.2, 0.4, 0.8, 1.0 ) );
    ctx.setPen( Qt::NoPen );
    for ( int step = 0; step < 20; ++step )
        ctx.drawRect( -10.0, -10.0, width + 20.0, height + 20.0 );
}

static void fill_zone_plate( QPainter &ctx, int width, int height )
{
    ctx.setBrush( QColor::fromRgbF( 0.2, 0.4, 0.8, 1.0 ) );
    ctx.setPen( Qt::NoPen );
    for ( int ring = 0; ring < 90; ++ring )
    {
        float cx = 0.5f * width;
        float cy = 0.5f * height;
        float radius = 3.0f + 3.0f * ring;
        QPainterPath path;
        path.addEllipse( cx - radius, cy - radius,
                         2.0f * radius, 2.0f * radius );
        ctx.drawPath( path );
    }
}

static void stroke_many( QPainter &ctx, int width, int height )
{
    QPen pen( QColor::fromRgbF( 0.2, 0.4, 0.8, 1.0 ) );
    pen.setWidthF( 1.5 );
    ctx.setPen( pen );
    ctx.setBrush( Qt::NoBrush );
    QPainterPath path;
    for ( int step = 0; step < 400; ++step )
    {
        path.moveTo( frand() * width, frand() * height );
        path.cubicTo( frand() * width, frand() * height,
                      frand() * width, frand() * height,
                      frand() * width, frand() * height );
    }
    ctx.drawPath( path );
}

static void stroke_wide( QPainter &ctx, int width, int height )
{
    QPen pen( QColor::fromRgbF( 0.2, 0.4, 0.8, 1.0 ) );
    pen.setWidthF( 12.0 );
    pen.setJoinStyle( Qt::RoundJoin );
    ctx.setPen( pen );
    ctx.setBrush( Qt::NoBrush );
    QPainterPath path;
    for ( int step = 0; step < 100; ++step )
    {
        path.moveTo( frand() * width, frand() * height );
        for ( int part = 0; part < 6; ++part )
            path.cubicTo( frand() * width, frand() * height,
                          frand() * width, frand() * height,
                          frand() * width, frand() * height );
    }
    ctx.drawPath( path );
}

static void gradient_linear( QPainter &ctx, int width, int height )
{
    QLinearGradient brush( 0.0, 0.0, width, height );
    brush.setColorAt( 0.0, QColor::fromRgbF( 1.0, 0.2, 0.1, 1.0 ) );
    brush.setColorAt( 0.5, QColor::fromRgbF( 0.1, 0.9, 0.3, 1.0 ) );
    brush.setColorAt( 1.0, QColor::fromRgbF( 0.2, 0.1, 1.0, 1.0 ) );
    ctx.setBrush( brush );
    ctx.setPen( Qt::NoPen );
    for ( int step = 0; step < 10; ++step )
        ctx.drawRect( 0.0, 0.0, width, height );
}

static void gradient_radial( QPainter &ctx, int width, int height )
{
    QRadialGradient brush( 0.5 * width, 0.5 * height, 0.6 * width,
                           0.3 * width, 0.4 * height );
    brush.setColorAt( 0.0, QColor::fromRgbF( 1.0, 0.9, 0.2, 1.0 ) );
    brush.setColorAt( 0.7, QColor::fromRgbF( 0.2, 0.5, 0.9, 0.8 ) );
    brush.setColorAt( 1.0, QColor::fromRgbF( 0.1, 0.1, 0.4, 0.2 ) );
    ctx.setBrush( brush );
    ctx.setPen( Qt::NoPen );
    for ( int step = 0; step < 10; ++step )
        ctx.drawRect( 0.0, 0.0, width, height );
}

static void pattern_tiled( QPainter &ctx, int, int )
{
    // canvas_ity consumes no RNG for the pattern bytes; they are
    // deterministic, so the streams stay aligned either way.
    QImage tile( 64, 64, QImage::Format_ARGB32_Premultiplied );
    for ( int y = 0; y < 64; ++y )
        for ( int x = 0; x < 64; ++x )
        {
            int index = y * 64 + x;
            tile.setPixel( x, y, qRgba(
                static_cast< int >( index * 28 + 0 ) & 255,
                static_cast< int >( index * 28 + 1 ) & 255,
                static_cast< int >( index * 28 + 2 ) & 255,
                static_cast< int >( index * 28 + 3 ) & 255 ) );
        }
    ctx.setBrush( QBrush( tile ) );
    ctx.setPen( Qt::NoPen );
    for ( int step = 0; step < 10; ++step )
        ctx.drawRect( 0.0, 0.0, 512.0, 512.0 );
}

static void image_scaled( QPainter &ctx, int width, int height )
{
    QImage source( 128, 96, QImage::Format_ARGB32_Premultiplied );
    for ( int y = 0; y < 96; ++y )
        for ( int x = 0; x < 128; ++x )
        {
            int index = y * 128 + x;
            source.setPixel( x, y, qRgba(
                static_cast< int >( index * 124 + 0 ) & 255,
                static_cast< int >( index * 124 + 1 ) & 255,
                static_cast< int >( index * 124 + 2 ) & 255,
                static_cast< int >( index * 124 + 3 ) & 255 ) );
        }
    for ( int step = 0; step < 10; ++step )
        ctx.drawImage( QRectF( 0.0, 0.0, width, height ), source );
}

static void clip_heavy( QPainter &ctx, int width, int height )
{
    // APPROX: QPainter has no cheap arbitrary-path clip with the same
    // cost model; use the bounding rect of the 8-arc rosette as the
    // clip, matching geometry RNG consumption of the original.
    float x0 = width, y0 = height, x1 = 0.0f, y1 = 0.0f;
    for ( int step = 0; step < 8; ++step )
    {
        float cx = frange( 0.25f, 0.75f ) * width;
        float cy = frange( 0.25f, 0.75f ) * height;
        float r = 0.3f * width;
        float a0 = frand() * 6.28f;
        float a1 = frand() * 6.28f + 6.28f;
        bool ccw = frand() < 0.5f;
        (void)a0;
        (void)a1;
        (void)ccw;
        if ( cx - r < x0 )
            x0 = cx - r;
        if ( cy - r < y0 )
            y0 = cy - r;
        if ( cx + r > x1 )
            x1 = cx + r;
        if ( cy + r > y1 )
            y1 = cy + r;
    }
    ctx.save();
    ctx.setClipRect( QRectF( x0, y0, x1 - x0, y1 - y0 ) );
    for ( int step = 0; step < 30; ++step )
    {
        ctx.setBrush( QColor::fromRgbF( frand(), frand(), frand(),
                                        frand() ) );
        ctx.setPen( Qt::NoPen );
        QPainterPath path;
        path.addEllipse( frand() * width, frand() * height,
                         frand() * 0.4f * width, frand() * 0.4f * width );
        ctx.drawPath( path );
    }
    ctx.restore();
}

static void composite_ops( QPainter &ctx, int width, int height )
{
    // canvas_ity cycles composite operation values 1..15 skipping
    // 5, 6, 8, and 9; all eleven map onto QPainter composition modes.
    static const QPainter::CompositionMode operations[] = {
        QPainter::CompositionMode_SourceIn,
        QPainter::CompositionMode_Source,
        QPainter::CompositionMode_SourceOut,
        QPainter::CompositionMode_DestinationIn,
        QPainter::CompositionMode_DestinationAtop,
        QPainter::CompositionMode_Plus,
        QPainter::CompositionMode_DestinationOver,
        QPainter::CompositionMode_DestinationOut,
        QPainter::CompositionMode_SourceAtop,
        QPainter::CompositionMode_SourceOver,
        QPainter::CompositionMode_Xor,
    };
    for ( size_t op = 0;
          op < sizeof( operations ) / sizeof( operations[ 0 ] ); ++op )
    {
        ctx.setCompositionMode( operations[ op ] );
        ctx.setBrush( QColor::fromRgbF( 0.9, 0.5, 0.2, 0.6 ) );
        ctx.setPen( Qt::NoPen );
        ctx.drawRect( 0.1 * width, 0.1 * height,
                      0.6 * width, 0.6 * height );
    }
    ctx.setCompositionMode( QPainter::CompositionMode_SourceOver );
}

static void transforms( QPainter &ctx, int width, int height )
{
    ctx.setBrush( QColor::fromRgbF( 0.2, 0.4, 0.8, 1.0 ) );
    ctx.setPen( Qt::NoPen );
    for ( int step = 0; step < 112; ++step )
    {
        ctx.save();
        ctx.translate( 0.5 * width, 0.5 * height );
        ctx.rotate( 0.1 * step * 57.29578 );
        ctx.scale( 1.0 + 0.01 * step, 0.9 );
        ctx.translate( -0.5 * width, -0.5 * height );
        float cx = 0.4f * width;
        float cy = 0.4f * height;
        float radius = 0.3f * width;
        QPainterPath path;
        path.addEllipse( cx - radius, cy - radius,
                         2.0f * radius, 2.0f * radius );
        ctx.drawPath( path );
        ctx.restore();
    }
}

static void many_primitives( QPainter &ctx, int width, int height )
{
    ctx.setPen( Qt::NoPen );
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
        ctx.setBrush( QColor::fromRgbF( ink[ band ][ 0 ],
                                        ink[ band ][ 1 ],
                                        ink[ band ][ 2 ],
                                        ink[ band ][ 3 ] ) );
        ctx.drawRect( QRectF( x, y, 1.0f + frand() * 8.0f,
                              1.0f + frand() * 8.0f ) );
    }
}

static void complex_scene( QPainter &ctx, int width, int height )
{
    for ( int layer = 0; layer < 10; ++layer )
    {
        ctx.save();
        ctx.translate( frand() * 0.2 * width, frand() * 0.2 * height );
        ctx.rotate( frand() * 6.28 * 57.29578 );
        if ( layer % 3 == 0 )
        {
            QLinearGradient brush( 0.0, 0.0, 0.5 * width, 0.5 * height );
            brush.setColorAt( 0.0,
                              QColor::fromRgbF( 1.0, 0.0, 0.0, 0.9 ) );
            brush.setColorAt( 1.0,
                              QColor::fromRgbF( 0.0, 0.0, 1.0, 0.5 ) );
            ctx.setBrush( brush );
        }
        else
            ctx.setBrush( QColor::fromRgbF( frand(), frand(), frand(),
                                            frand() ) );
        QPainterPath path;
        for ( int subpath = 0; subpath < 12; ++subpath )
        {
            path.moveTo( frand() * width, frand() * height );
            for ( int step = 0; step < 8; ++step )
                path.cubicTo( frand() * width, frand() * height,
                              frand() * width, frand() * height,
                              frand() * width, frand() * height );
            path.closeSubpath();
        }
        ctx.setPen( Qt::NoPen );
        ctx.drawPath( path );
        QPen pen( QColor::fromRgbF( frand(), frand(), frand(),
                                    frand() ) );
        pen.setWidthF( 1.0 + frand() * 3.0 );
        ctx.setPen( pen );
        ctx.setBrush( Qt::NoBrush );
        ctx.drawPath( path );
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
    { "clip_heavy", clip_heavy, 512, 512 },
    { "composite_ops", composite_ops, 512, 512 },
    { "shadow_blurred", 0, 512, 512 },
    { "transforms", transforms, 512, 512 },
    { "many_primitives", many_primitives, 512, 512 },
    { "complex_scene", complex_scene, 512, 512 },
};

int main( int argc, char **argv )
{
    QCoreApplication app( argc, argv );
    setvbuf( stdout, nullptr, _IONBF, 0 );
    int trials = argc > 1 ? atoi( argv[ 1 ] ) : 15;
    bool audit = getenv( "RNG_AUDIT" ) != 0;
    const int warmups = 3;
    int count = sizeof( tests ) / sizeof( tests[ 0 ] );
    double geo = 0.0;
    int supported = 0;
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
            QImage target( item.width, item.height,
                           QImage::Format_ARGB32_Premultiplied );
            target.fill( 0 );
            QPainter ctx( &target );
            ctx.setRenderHint( QPainter::Antialiasing );
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
            QImage target( item.width, item.height,
                           QImage::Format_ARGB32_Premultiplied );
            target.fill( 0 );
            QPainter ctx( &target );
            ctx.setRenderHint( QPainter::Antialiasing );
            double start = get_seconds();
            item.call( ctx, item.width, item.height );
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
