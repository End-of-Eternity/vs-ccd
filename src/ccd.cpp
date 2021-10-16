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

#include <VapourSynth4.h>
#include <VSHelper4.h>

static const double *init_multipliers() {
    const int n = 20; // number of multipliers
    static double mutlipliers[n];
    for (int i=0; i<n; i++)
        mutlipliers[i] = 1. / (i+1);
    return mutlipliers;
}

static const double *MULTIPLIERS = init_multipliers();

typedef struct ccdData {
    VSNode *node;
    float threshold;
} ccdData;

static void ccdRun(const VSFrame *src, VSFrame *dest, float threshold, const VSAPI *vsapi) {
    int width = vsapi->getFrameWidth(src, 0);
    int height = vsapi->getFrameHeight(src, 0);

    auto *src_r_plane = reinterpret_cast<const float *>(vsapi->getReadPtr(src, 0));
    auto *src_g_plane = reinterpret_cast<const float *>(vsapi->getReadPtr(src, 1));
    auto *src_b_plane = reinterpret_cast<const float *>(vsapi->getReadPtr(src, 2));

    auto *dst_r_plane = reinterpret_cast<float *>(vsapi->getWritePtr(dest, 0));
    auto *dst_g_plane = reinterpret_cast<float *>(vsapi->getWritePtr(dest, 1));
    auto *dst_b_plane = reinterpret_cast<float *>(vsapi->getWritePtr(dest, 2));

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int i = y * width + x;

            float r = src_r_plane[i], g = src_g_plane[i], b = src_b_plane[i];
            float total_r = r, total_g = g, total_b = b;
            int n = 0;

            for (int dy = y - 12; dy <= y + 12; dy += 8) {
                int comp_y = dy;
                if (comp_y < 0)
                    comp_y = -comp_y;
                else if (comp_y >= height)
                    comp_y = 2 * (height - 1) - comp_y;

                int y_offset = comp_y * width;
                for (int dx = x - 12; dx <= x + 12; dx += 8) {

                    int comp_x = dx;
                    if (comp_x < 0)
                        comp_x = -comp_x;
                    else if (comp_x >= width)
                        comp_x = 2 * (width - 1) - comp_x;

                    float comp_r = src_r_plane[y_offset + comp_x];
                    float comp_g = src_g_plane[y_offset + comp_x];
                    float comp_b = src_b_plane[y_offset + comp_x];

                    float diff_r = comp_r - r;
                    float diff_g = comp_g - g;
                    float diff_b = comp_b - b;

#define SQUARE(x) ((x) * (x))
                    if (threshold > (SQUARE(diff_r) + SQUARE(diff_g) + SQUARE(diff_b))) {
                        total_r += comp_r;
                        total_b += comp_b;
                        total_g += comp_g;
                        n++;
                    }
#undef SQUARE
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

            dst_r_plane[i] = calculated_r;
            dst_g_plane[i] = calculated_g;
            dst_b_plane[i] = calculated_b;
        }
    }
}

static const VSFrame *VS_CC ccdGetframe(int n, int activationReason,
                                        void *instanceData,
                                        void **frameData,
                                        VSFrameContext *frameCtx,
                                        VSCore *core, const VSAPI *vsapi)  {
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

        ccdRun(src, dest, d->threshold, vsapi);

        vsapi->freeFrame(src);

        return dest;
    }

    return nullptr;
}

static void VS_CC ccdFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    auto *d = reinterpret_cast<ccdData *>(instanceData);
    vsapi->freeNode(d->node);
    delete d;
}

static void VS_CC ccdCreate(const VSMap *in, VSMap *out, void *userData,
                            VSCore *core, const VSAPI *vsapi)  {
    std::unique_ptr<ccdData> d(new ccdData());
    int err;

    d->node = vsapi->mapGetNode(in, "clip", 0, nullptr);
    d->threshold = static_cast<float>(vsapi->mapGetFloat(in, "threshold", 0, &err));
    if (err) d->threshold = 4;
    d->threshold = d->threshold * d->threshold / 195075.0; // the magic number - thanks DomBito

    const VSVideoInfo *vi = vsapi->getVideoInfo(d->node);

    if (vi->format.sampleType != stFloat || vi->format.bitsPerSample != 32 ||
        vi->format.colorFamily != cfRGB || vi->format.subSamplingH != 0 ||
        vi->format.subSamplingW != 0) {
        vsapi->mapSetError(out, "CCD: Input clip must be RGBS");
        vsapi->freeNode(d->node);
    }

    if (vi->width < 12 || vi->height < 12) {
        vsapi->mapSetError(out, "CCD: Input clip dimensions must be at least 12x12");
        vsapi->freeNode(d->node);
    }

    if (d->threshold < 0) {
        vsapi->mapSetError(out, "CCD: Threshold must be >= 0");
        vsapi->freeNode(d->node);
    }

    vsapi->createVideoFilter(out, "ccd", vi, ccdGetframe, ccdFree,
                             fmParallel, 0, 0, d.get(), core);
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
