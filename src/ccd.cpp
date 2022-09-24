/**
 *  CCD - Camcorder Color Denoise v0.1
 *
 *  Copyright (c) 2006-2020 Stolyarevskiy Sergey
 *  Copyright (c) 2020 DomBito
 *  Copyright (c) 2021 Arjun Raj (End of Eternity)
 *  Copyright (c) 2021 Atharva (Scrad)
 *
 *  This project is licensed under the GPL v3 License.
 **/
#include <memory>
#include <cassert>
#include <cmath>
#include <cstdlib>

#include <VapourSynth4.h>
#include <VSHelper4.h>

// Convenient helper to declare unused function parameters
template <typename... T> inline void unused (T &&...) noexcept {}

template <typename T> inline constexpr T square (T x) noexcept {
    return x * x;
}

template <typename T> inline constexpr T clip (T x, T mi, T ma) noexcept {
#if (__cplusplus >= 201402L) // C++14 required here
    assert (mi <= ma);
#endif
    return (x < mi) ? mi : (x > ma) ? ma : x;
}

// Wraps out-of-bound coordinates
template <typename T> inline constexpr T wrap_oob_coord (T x, T w) noexcept {
#if (__cplusplus >= 201402L) // C++14 required here
   assert (w > 0);
#endif
   using std::abs;
   return (x >= w) ? w * 2 - (x + 2) : abs (x);
}

// Maximum radius of the filter kernel in pixels (excluding the center)
// Max kernel diameter is max_radius * 2 + 1
constexpr auto max_radius = 12;
static_assert (max_radius >= 0, "Kernel must be at least 1 pixel");

// Minimum subsampling rate for the filter kernel, per dimension
constexpr auto min_step = 8;
static_assert (min_step >= 1, "1:1 sampling is a strict minimum");

// Maximum number of pixels potentially included by the kernel
constexpr auto max_kernel_samples = square(1 + max_radius * 2 / min_step);

// We always include the reference pixel in addition to the kernel scan
constexpr auto max_weight = 1 + max_kernel_samples;

static const float *init_reciprocal_table() {
    // Builds the inverse reciprocal list
    static float rcp_table[1 + max_weight] {};
    for (int i=1; i<=max_weight; i++)
        rcp_table[i] = 1.f / float (i);
    return rcp_table;
}

static const float *RCP_TABLE = init_reciprocal_table();

typedef struct ccdData {
    VSNode *node;
    float thr_sq; // Scaled and squared threshold, for quick comparison with the L2 norm
} ccdData;

static void ccdRun(const VSFrame *src, VSFrame *dest, float thr_sq, const VSAPI *vsapi) {
    int width = vsapi->getFrameWidth(src, 0);
    int height = vsapi->getFrameHeight(src, 0);

    auto *src_r_plane = reinterpret_cast<const float *>(vsapi->getReadPtr(src, 0));
    auto *src_g_plane = reinterpret_cast<const float *>(vsapi->getReadPtr(src, 1));
    auto *src_b_plane = reinterpret_cast<const float *>(vsapi->getReadPtr(src, 2));

    auto *dst_r_plane = reinterpret_cast<float *>(vsapi->getWritePtr(dest, 0));
    auto *dst_g_plane = reinterpret_cast<float *>(vsapi->getWritePtr(dest, 1));
    auto *dst_b_plane = reinterpret_cast<float *>(vsapi->getWritePtr(dest, 2));

    constexpr auto datasz_log2 = 2; // log2 (data_size)
    assert (vsapi->getVideoFrameFormat (src )->bytesPerSample == 1 << datasz_log2);
    assert (vsapi->getVideoFrameFormat (dest)->bytesPerSample == 1 << datasz_log2);

    const auto src_r_stride = vsapi->getStride (src, 0) >> datasz_log2;
    const auto src_g_stride = vsapi->getStride (src, 1) >> datasz_log2;
    const auto src_b_stride = vsapi->getStride (src, 2) >> datasz_log2;

    const auto dst_r_stride = vsapi->getStride (dest, 0) >> datasz_log2;
    const auto dst_g_stride = vsapi->getStride (dest, 1) >> datasz_log2;
    const auto dst_b_stride = vsapi->getStride (dest, 2) >> datasz_log2;

    constexpr auto radius = max_radius;
    constexpr auto step   = min_step;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const auto i_r = y * src_r_stride + x;
            const auto i_g = y * src_g_stride + x;
            const auto i_b = y * src_b_stride + x;

            const auto o_r = y * dst_r_stride + x;
            const auto o_g = y * dst_g_stride + x;
            const auto o_b = y * dst_b_stride + x;

            float r = src_r_plane[i_r], g = src_g_plane[i_g], b = src_b_plane[i_b];
            float total_r = r, total_g = g, total_b = b;
            int n = 1; // Starts with the reference pixel

            for (int dy = y - radius; dy <= y + radius; dy += step) {
                const auto comp_y = wrap_oob_coord (dy, height);

                const auto y_ofs_r = comp_y * src_r_stride;
                const auto y_ofs_g = comp_y * src_g_stride;
                const auto y_ofs_b = comp_y * src_b_stride;
                for (int dx = x - radius; dx <= x + radius; dx += step) {

                    const auto comp_x = wrap_oob_coord (dx, width);

                    float comp_r = src_r_plane[y_ofs_r + comp_x];
                    float comp_g = src_g_plane[y_ofs_g + comp_x];
                    float comp_b = src_b_plane[y_ofs_b + comp_x];

                    float diff_r = comp_r - r;
                    float diff_g = comp_g - g;
                    float diff_b = comp_b - b;

                    const auto l2norm_sq = square(diff_r) + square(diff_g) + square(diff_b);
                    if (thr_sq > l2norm_sq) {
                        total_r += comp_r;
                        total_b += comp_b;
                        total_g += comp_g;
                        n++;
                    }
                }
            }

            assert (n <= max_weight);
            float calculated_r = total_r * RCP_TABLE[n];
            float calculated_g = total_g * RCP_TABLE[n];
            float calculated_b = total_b * RCP_TABLE[n];

            // Excepted for minor numerical errors (a few ULPs), the
            // usefulness of these lines is not obvious. It could even be
            // erroneous when input is out of the [0 ; 1] range (HDR content).
            calculated_r = clip (calculated_r, 0.f, 1.f);
            calculated_g = clip (calculated_g, 0.f, 1.f);
            calculated_b = clip (calculated_b, 0.f, 1.f);

            dst_r_plane[o_r] = calculated_r;
            dst_g_plane[o_g] = calculated_g;
            dst_b_plane[o_b] = calculated_b;
        }
    }
}

static const VSFrame *VS_CC ccdGetframe(int n, int activationReason,
                                        void *instanceData,
                                        void **frameData,
                                        VSFrameContext *frameCtx,
                                        VSCore *core, const VSAPI *vsapi)  {
    unused (frameData);
    auto *d = reinterpret_cast<ccdData *>(instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrame *src = vsapi->getFrameFilter(n, d->node, frameCtx);
        const VSVideoFormat *format = vsapi->getVideoFrameFormat(src);

        int width = vsapi->getFrameWidth(src, 0);
        int height = vsapi->getFrameHeight(src, 0);

        const VSFrame *plane_src[3] = {src, src, src};
        int planes[3] = {0, 1, 2};

        VSFrame *dest = vsapi->newVideoFrame2(format, width, height, plane_src, planes, src, core);

        ccdRun(src, dest, d->thr_sq, vsapi);

        vsapi->freeFrame(src);

        return dest;
    }

    return nullptr;
}

static void VS_CC ccdFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    unused (core);
    auto *d = reinterpret_cast<ccdData *>(instanceData);
    vsapi->freeNode(d->node);
    delete d;
}

static void VS_CC ccdCreate(const VSMap *in, VSMap *out, void *userData,
                            VSCore *core, const VSAPI *vsapi)  {
    unused (userData);
    std::unique_ptr<ccdData> d(new ccdData());
    int err;

    d->node = vsapi->mapGetNode(in, "clip", 0, nullptr);
    auto thr_user = static_cast<float>(vsapi->mapGetFloat(in, "threshold", 0, &err));
    if (err) thr_user = 4;
    // The following calculation is equivalent to the orignal formula
    // thr*thr/195075 but a bit more explicit.
    // It is not known why the sqrt(3) factor has been introduced.
    const auto scale = 255.f * sqrtf(3); // threshold unit is 1/scale-th of the data range [0 ; 1]
    d->thr_sq = square(thr_user / scale); // the magic number - thanks DomBito

    const VSVideoInfo *vi = vsapi->getVideoInfo(d->node);

    if (vi->format.sampleType != stFloat || vi->format.bitsPerSample != 32 ||
        vi->format.colorFamily != cfRGB || vi->format.subSamplingH != 0 ||
        vi->format.subSamplingW != 0) {
        vsapi->mapSetError(out, "CCD: Input clip must be RGBS");
        return;
    }

    if (vi->width < max_radius || vi->height < max_radius) {
        vsapi->mapSetError(out, "CCD: Input clip dimensions must be at least 12x12");
        return;
    }

    if (thr_user < 0) {
        vsapi->mapSetError(out, "CCD: Threshold must be >= 0");
        return;
    }

    VSFilterDependency deps[] = {{d->node, rpGeneral}};
    vsapi->createVideoFilter(out, "ccd", vi, ccdGetframe, ccdFree,
                             fmParallel, deps, 1, d.get(), core);
    d.release();
}

VS_EXTERNAL_API(void)
VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin("com.eoe-scrad.ccd", "ccd", "chroma denoiser",
                         1, VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("CCD",
                             "clip:vnode;"
                             "threshold:float:opt;",
                             "clip:vnode;", ccdCreate, 0, plugin);
}
