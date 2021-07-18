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
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <random>

#include <VapourSynth.h>
#include <VSHelper.h>

static const double *init_multipliers() {
    const int n = 20; // number of multipliers
    static double mutlipliers[n];
    for (int i=0; i<n; i++)
        mutlipliers[i] = 1. / (i+1);
    return mutlipliers;
}

static const double *MULTIPLIERS = init_multipliers();

typedef struct ccdData {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    float threshold;
} ccdData;

struct location {
    int x;
    int y;
};

static void ccd_run(const VSFrameRef *src, VSFrameRef *dest, float threshold, const VSAPI *vsapi) {
    int width = vsapi->getFrameWidth(src, 0);
    int height = vsapi->getFrameHeight(src, 0);

    auto *src_r_plane = reinterpret_cast<const float *>(vsapi->getReadPtr(src, 0));
    auto *src_g_plane = reinterpret_cast<const float *>(vsapi->getReadPtr(src, 1));
    auto *src_b_plane = reinterpret_cast<const float *>(vsapi->getReadPtr(src, 2));

    auto *dst_r_plane = reinterpret_cast<float *>(vsapi->getWritePtr(dest, 0));
    auto *dst_g_plane = reinterpret_cast<float *>(vsapi->getWritePtr(dest, 1));
    auto *dst_b_plane = reinterpret_cast<float *>(vsapi->getWritePtr(dest, 2));

    // offsets for getting vector locations
    std::vector<location> offsets;

    for (int y = -12; y <= 12; y++) {
        for (int x = -12; x <= 12; x++) {
            location cur_offset;
            cur_offset.x = x;
            cur_offset.y = y;
            offsets.push_back(cur_offset);
        }
    }

    auto rng = std::minstd_rand {};

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int index = y * width + x;

            float r = src_r_plane[index], g = src_g_plane[index], b = src_b_plane[index];
            float total_r = r, total_g = g, total_b = b;
            int n = 0;

            std::shuffle(offsets.begin(), offsets.end(), rng);

            for (std::vector<location>::size_type offset_index = 0; offset_index < 12; index++) {
                const location offset = offsets[offset_index];
                int comp_y = offset.y + y;
                if (comp_y < 0)
                    comp_y = -comp_y;
                else if (comp_y >= width)
                    comp_y = 2 * (width - 1) - comp_y;

                int comp_x = offset.x + x;
                if (comp_x < 0)
                    comp_x = -comp_x;
                else if (comp_x >= width)
                    comp_x = 2 * (width - 1) - comp_x;

                float comp_r = src_r_plane[(comp_y * width) + comp_x];
                float comp_g = src_g_plane[(comp_y * width) + comp_x];
                float comp_b = src_b_plane[(comp_y * width) + comp_x];

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

            dst_r_plane[index] = calculated_r;
            dst_g_plane[index] = calculated_g;
            dst_b_plane[index] = calculated_b;
        }
    }
}

static void VS_CC ccdInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    (void) in;
    (void) out;
    (void) core;

    auto *d = (ccdData *) *instanceData;

    vsapi->setVideoInfo(d->vi, 1, node);
}

static const VSFrameRef *
VS_CC
ccdGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx,
            VSCore *core, const VSAPI *vsapi) {
    auto *d = static_cast<ccdData *>(*instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter(n, d->node, frameCtx);
        const VSFormat *format = vsapi->getFrameFormat(src);

        int width = vsapi->getFrameWidth(src, 0);
        int height = vsapi->getFrameHeight(src, 0);

        const VSFrameRef *plane_src[3] = {src, src, src};
        int planes[3] = {0, 1, 2};

        VSFrameRef *dest = vsapi->newVideoFrame2(format, width, height, plane_src, planes, src, core);

        ccd_run(src, dest, d->threshold, vsapi);

        vsapi->freeFrame(src);

        return dest;
    }

    return nullptr;
}

static void VS_CC ccdFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    auto *d = (ccdData *) instanceData;
    vsapi->freeNode(d->node);
    free(d);
}

static void VS_CC ccdCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    ccdData d;
    ccdData *data;
    int err;

    d.node = vsapi->propGetNode(in, "clip", 0, nullptr);
    d.threshold = static_cast<float>(vsapi->propGetFloat(in, "threshold", 0, &err));
    if (err)
        d.threshold = 4;
    d.threshold = d.threshold * d.threshold / 195075.0; // the magic number - thanks DomBito

    d.vi = vsapi->getVideoInfo(d.node);

    if (!d.vi->format) {
        vsapi->setError(out, "CCD: Variable format clips are not supported.");
        vsapi->freeNode(d.node);
    }

    if (d.vi->format->id != 2000015) { // ID of RGBS
        vsapi->setError(out, "CCD: Input clip must be RGBS");
        vsapi->freeNode(d.node);
    }

    if (d.vi->width < 12 || d.vi->height < 12) {
        vsapi->setError(out, "CCD: Input clip dimensions must be at least 12x12");
        vsapi->freeNode(d.node);
    }

    if (d.threshold < 0) {
        vsapi->setError(out, "CCD: Threshold must be >= 0");
        vsapi->freeNode(d.node);
    }

    data = (ccdData *) malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "ccd", ccdInit, ccdGetframe, ccdFree, fmParallel, 0, data, core);
}

VS_EXTERNAL_API(void)
VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc,
                      VSPlugin *plugin) {
    configFunc("com.eoe-scrad.ccd", "ccd", "chroma denoiser", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("CCD",
                 "clip:clip;"
                 "threshold:float:opt;",
                 ccdCreate, nullptr, plugin);
}
