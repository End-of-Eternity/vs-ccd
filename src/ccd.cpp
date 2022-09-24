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

#include <VapourSynth4.h>
#include <VSHelper4.h>

// Convenient helper to declare unused function parameters
template <typename... T> inline void unused (T &&...) noexcept {}

template <typename T> inline constexpr T square (T x) noexcept {
    return x * x;
}

static const float *init_multipliers() {
    const int n = 20; // number of multipliers
    static float mutlipliers[n];
    for (int i=0; i<n; i++)
        mutlipliers[i] = 1.f / float (i+1);
    return mutlipliers;
}

static const float *MULTIPLIERS = init_multipliers();

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
            int n = 0;

            for (int dy = y - 12; dy <= y + 12; dy += 8) {
                int comp_y = dy;
                if (comp_y < 0)
                    comp_y = -comp_y;
                else if (comp_y >= height)
                    comp_y = 2 * (height - 1) - comp_y;

                const auto y_ofs_r = comp_y * src_r_stride;
                const auto y_ofs_g = comp_y * src_g_stride;
                const auto y_ofs_b = comp_y * src_b_stride;
                for (int dx = x - 12; dx <= x + 12; dx += 8) {

                    int comp_x = dx;
                    if (comp_x < 0)
                        comp_x = -comp_x;
                    else if (comp_x >= width)
                        comp_x = 2 * (width - 1) - comp_x;

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

            float calculated_r = total_r * MULTIPLIERS[n];
            float calculated_g = total_g * MULTIPLIERS[n];
            float calculated_b = total_b * MULTIPLIERS[n];

            if (calculated_r < 0)
                calculated_r = 0;
            else if (calculated_r > 1)
                calculated_r = 1;

            if (calculated_g < 0)
                calculated_g = 0;
            else if (calculated_g > 1)
                calculated_g = 1;

            if (calculated_b < 0)
                calculated_b = 0;
            else if (calculated_b > 1)
                calculated_b = 1;

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

    if (vi->width < 12 || vi->height < 12) {
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
