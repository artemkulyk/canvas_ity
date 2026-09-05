// Clean canonical 17-workload microbenchmark driver for canvas_ity.
//
// On the cell-proto branch, src/canvas_ity.hpp contains BOTH rasterizers;
// compiling this driver with -DCELL_PROTO activates the experimental
// analytic cell rasterizer inside the header for plain solid fills.  The
// same source compiles either way.
//
// Methodology (also used by the four library ports after the 2026-09
// clean-benchmark pass):
// - The RNG seed is derived from the workload index and is IDENTICAL in
//   every driver (all drivers share the same 17-entry tests[] order).
// - rng_state is reset BEFORE EVERY TRIAL, so every trial renders the
//   exact same scene.  best-of-N is therefore a min over measurement
//   noise, not a "cheapest random scene" lottery (the old drivers
//   advanced the stream across trials, which made results
//   irreproducible and seed-dependent).
// - 3 uncounted warm-up trials precede the counted trials (allocator
//   warm-up, JIT warm-up).
// - Each workload prints best and median milliseconds.
// - RNG_AUDIT=1 renders each workload once and prints the final
//   rng_state; identical values across drivers prove that every library
//   consumed the identical scene stream (same number of frand() calls
//   in the same order).
//
// Build (see bench/cleanbench.sh):
//   c++ -O2 -std=c++11 -I<src-dir> -o mb_ci ci_clean_driver.cpp
//   c++ -O2 -std=c++11 -DCELL_PROTO -I<src-dir> -o mb_ci_cell ci_clean_driver.cpp

#define CANVAS_ITY_IMPLEMENTATION
#include "canvas_ity.hpp"
#if defined(__MACH__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>
using namespace canvas_ity;

static double get_seconds() {
#if defined(__MACH__)
    static double r=0; if(!r){static mach_timebase_info_data_t f; mach_timebase_info(&f); r=f.numer*1e-9/f.denom;}
    return mach_absolute_time()*r;
#else
    timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9;
#endif
}
static unsigned long long rng_state = 0x243f6a8885a308d3ULL;
static float frand(){rng_state^=rng_state<<13;rng_state^=rng_state>>7;rng_state^=rng_state<<17;return (float)(rng_state&0xffffffULL)/(float)0x1000000ULL;}
static float frange(float a,float b){return a+frand()*(b-a);}
typedef void (*workload)(canvas&,int,int);
struct entry{const char*name;workload call;int width,height;};

static void path_construction(canvas& that,int,int){ for(int r=0;r<200;r++){ that.begin_path();
    for(int s=0;s<50;s++){ that.move_to(frand()*512,frand()*512);
        for(int k=0;k<10;k++){ that.line_to(frand()*512,frand()*512);
            that.bezier_curve_to(frand()*512,frand()*512,frand()*512,frand()*512,frand()*512,frand()*512); } } } }
static void flattening_fill(canvas& that,int w,int h){ that.begin_path();
    for(int s=0;s<60;s++){that.move_to(frand()*w,frand()*h);
        for(int k=0;k<12;k++) that.bezier_curve_to(frand()*w,frand()*h,frand()*w,frand()*h,frand()*w,frand()*h);
        that.close_path();} that.fill(); }
static void fill_small(canvas& that,int w,int h){ for(int i=0;i<2000;i++){ that.begin_path();
    that.arc(frand()*w,frand()*h,2.0f+frand()*6.0f,0,6.2832f,false); that.fill(); } }
static void fill_large(canvas& that,int w,int h){ that.set_color(fill_style,0.2f,0.4f,0.8f,1.0f);
    for(int k=0;k<20;k++) that.fill_rectangle(-10.0f,-10.0f,(float)w+20.0f,(float)h+20.0f); }
static void fill_zone_plate(canvas& that,int w,int h){ for(int r=0;r<90;r++){that.begin_path();
    that.arc(0.5f*w,0.5f*h,3.0f+3.0f*r,0,6.2832f,false); that.fill();} }
static void stroke_many(canvas& that,int w,int h){ that.set_line_width(1.5f); that.begin_path();
    for(int i=0;i<400;i++){that.move_to(frand()*w,frand()*h);
        that.bezier_curve_to(frand()*w,frand()*h,frand()*w,frand()*h,frand()*w,frand()*h);} that.stroke(); }
static void stroke_wide(canvas& that,int w,int h){ that.set_line_width(12.0f); that.line_join=rounded; that.begin_path();
    for(int i=0;i<100;i++){that.move_to(frand()*w,frand()*h);
        for(int p=0;p<6;p++) that.bezier_curve_to(frand()*w,frand()*h,frand()*w,frand()*h,frand()*w,frand()*h);} that.stroke(); }
static void gradient_linear(canvas& that,int w,int h){ that.set_linear_gradient(fill_style,0,0,(float)w,(float)h);
    that.add_color_stop(fill_style,0.0f,1,0.2f,0.1f,1); that.add_color_stop(fill_style,0.5f,0.1f,0.9f,0.3f,1); that.add_color_stop(fill_style,1.0f,0.2f,0.1f,1,1);
    for(int k=0;k<10;k++) that.fill_rectangle(0,0,(float)w,(float)h); }
static void gradient_radial(canvas& that,int w,int h){ that.set_radial_gradient(fill_style,0.3f*w,0.4f*h,2.0f,0.5f*w,0.5f*h,0.6f*w);
    that.add_color_stop(fill_style,0.0f,1,0.9f,0.2f,1); that.add_color_stop(fill_style,0.7f,0.2f,0.5f,0.9f,0.8f); that.add_color_stop(fill_style,1.0f,0.1f,0.1f,0.4f,0.2f);
    for(int k=0;k<10;k++) that.fill_rectangle(0,0,(float)w,(float)h); }
static void pattern_tiled(canvas& that,int w,int h){ std::vector<unsigned char> img(64*64*4);
    for(size_t i=0;i<img.size();i++) img[i]=(unsigned char)(i*7);
    that.set_pattern(fill_style,&img.front(),64,64,256,repeat);
    for(int k=0;k<10;k++) that.fill_rectangle(0,0,(float)w,(float)h); }
static void image_scaled(canvas& that,int w,int h){ std::vector<unsigned char> img(128*96*4);
    for(size_t i=0;i<img.size();i++) img[i]=(unsigned char)(i*31);
    for(int k=0;k<10;k++) that.draw_image(&img.front(),128,96,512,0,0,(float)w,(float)h); }
static void clip_heavy(canvas& that,int w,int h){ that.begin_path();
    for(int s=0;s<8;s++) that.arc(frange(0.25f,0.75f)*w,frange(0.25f,0.75f)*h,0.3f*w,frand()*6.28f,frand()*6.28f+6.28f,frand()<0.5f);
    that.clip();
    for(int s=0;s<30;s++){ that.set_color(fill_style,frand(),frand(),frand(),frand()); that.begin_path();
        that.arc(frand()*w,frand()*h,frand()*0.4f*w,0,6.2832f,false); that.fill(); } }
static void composite_ops(canvas& that,int w,int h){ for(int op=1;op<=15;op++){
    if(op==5||op==6||op==8||op==9) continue;
    that.global_composite_operation=(composite_operation)op;
    that.set_color(fill_style,0.9f,0.5f,0.2f,0.6f);
    that.fill_rectangle(0.1f*w,0.1f*h,0.6f*w,0.6f*h);} }
static void shadow_blurred(canvas& that,int w,int h){ that.set_shadow_color(0,0,0,0.6f);
    for(int s=0;s<20;s++){ that.set_shadow_blur(4.0f+(s%5)*4.0f); that.shadow_offset_x=5; that.shadow_offset_y=7;
        that.begin_path(); that.rectangle(0.1f*w,0.1f*h,0.5f*w,0.4f*h); that.fill(); } }
static void transforms(canvas& that,int w,int h){ for(int s=0;s<112;s++){ that.save();
    that.translate(0.5f*w,0.5f*h); that.rotate(0.1f*s); that.scale(1.0f+0.01f*s,0.9f); that.translate(-0.5f*w,-0.5f*h);
    that.begin_path(); that.arc(0.4f*w,0.4f*h,0.3f*w,0,6.2832f,false); that.fill(); that.restore(); } }
static void many_primitives(canvas& that,int w,int h){ for(int i=0;i<22000;i++){
    float x=frand()*w, y=frand()*h; int band=((int)(x*0.05f+y*0.03f)%6+6)%6;
    static float const ink[6][4]={{0.13f,0.23f,0.38f,1},{0.91f,0.35f,0.13f,1},{0.87f,0.62f,0.13f,1},{0.13f,0.55f,0.55f,1},{0.45f,0.27f,0.49f,1},{0.94f,0.93f,0.88f,1}};
    that.set_color(fill_style,ink[band][0],ink[band][1],ink[band][2],ink[band][3]);
    that.fill_rectangle(x,y,1.0f+frand()*8.0f,1.0f+frand()*8.0f);} }
static void complex_scene(canvas& that,int w,int h){ for(int l=0;l<10;l++){ that.save();
    that.translate(frand()*0.2f*w,frand()*0.2f*h); that.rotate(frand()*6.28f);
    if(l%3==0){that.set_linear_gradient(fill_style,0,0,0.5f*w,0.5f*h);
        that.add_color_stop(fill_style,0.0f,1,0,0,0.9f); that.add_color_stop(fill_style,1.0f,0,0,1,0.5f);}
    else that.set_color(fill_style,frand(),frand(),frand(),frand());
    that.begin_path(); for(int s=0;s<12;s++){that.move_to(frand()*w,frand()*h);
        for(int k=0;k<8;k++) that.bezier_curve_to(frand()*w,frand()*h,frand()*w,frand()*h,frand()*w,frand()*h);
        that.close_path();} that.fill();
    that.set_line_width(1.0f+frand()*3.0f); that.set_color(stroke_style,frand(),frand(),frand(),frand()); that.stroke(); that.restore(); } }

static entry const tests[]={{"path_construction",path_construction,512,512},{"flattening_fill",flattening_fill,512,512},{"fill_small",fill_small,512,512},{"fill_large",fill_large,1024,1024},{"fill_zone_plate",fill_zone_plate,512,512},{"stroke_many",stroke_many,512,512},{"stroke_wide",stroke_wide,512,512},{"gradient_linear",gradient_linear,512,512},{"gradient_radial",gradient_radial,512,512},{"pattern_tiled",pattern_tiled,512,512},{"image_scaled",image_scaled,512,512},{"clip_heavy",clip_heavy,512,512},{"composite_ops",composite_ops,512,512},{"shadow_blurred",shadow_blurred,512,512},{"transforms",transforms,512,512},{"many_primitives",many_primitives,512,512},{"complex_scene",complex_scene,512,512}};

static unsigned long long seed_for(int index){
    return 0x243f6a8885a308d3ULL + index * 0x9e3779b97f4a7c15ULL;
}

int main(int argc,char**argv){
    setvbuf(stdout,0,_IONBF,0);
    int trials = argc>1?atoi(argv[1]):15;
    const char* filt = argc>2?argv[2]:0;
    bool audit = getenv("RNG_AUDIT")!=0;
    const int warmups = 3;
    double geo = 0.0;
    int supported = 0;
    for(size_t i=0;i<sizeof(tests)/sizeof(tests[0]);i++){
        entry e=tests[i];
        if(filt&&strcmp(filt,e.name)) continue;
        unsigned long long seed = seed_for((int)i);
        if(audit){
            rng_state=seed;
            canvas c(e.width,e.height);
            e.call(c,e.width,e.height);
            printf("AUDIT %-18s %016llx\n",e.name,rng_state);
            continue;
        }
        std::vector<double> ts; ts.reserve(trials);
        for(int t=-warmups;t<trials;t++){
            rng_state=seed;               // identical scene every trial
            canvas c(e.width,e.height);
            double s=get_seconds();
            e.call(c,e.width,e.height);
            double x=get_seconds();
            if(t>=0) ts.push_back(x-s);
        }
        std::sort(ts.begin(),ts.end());
        geo += log(ts.front());
        ++supported;
        printf("%-18s %9.3f %9.3f\n",e.name,ts.front()*1000.0,ts[ts.size()/2]*1000.0);
    }
    printf("%-18s %9.3f\n","geo mean",exp(geo/supported)*1000.0);
    return 0;
}
