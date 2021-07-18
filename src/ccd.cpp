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

#include <VapourSynth.h>
#include <VSHelper.h>

const double *init_multipliers() {
    const int n = 20; // number of multipliers
    static double mutlipliers[n];
    for (int i=0; i<n; i++)
        mutlipliers[i] = 1. / (i+1);
    return mutlipliers;
}

// calculate 1/n at compile time for averages - faster than dividing at runtime
const double *MULTIPLIERS = init_multipliers();

typedef struct ccdData {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    float threshold;                  // Euclidean distance threshold to be included in average
    int matrix_width, matrix_height;  // dimensions of the search matrix /2
    int offset_width, offset_height;  // offset between samples
} ccdData;

// edge handling
int reflect_oob(int x, int upper) {
    if (x >= upper)
        return 2 * (upper - 1) - x;
    else
        return std::abs(x);
}

void ccd_run(const VSFrameRef *src, VSFrameRef *dest, ccdData *d, const VSAPI *vsapi) {
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
            int i = y * width + x; // index of this (x, y) pixel

            float r = src_r_plane[i], g = src_g_plane[i], b = src_b_plane[i];
            float total_r = r, total_g = g, total_b = b;
            int n = 0;

            // iterate over matrix around current pixel
            for (int dy = y - d->matrix_height; dy <= y + d->matrix_height; dy += d->offset_height) {
                int comp_y = reflect_oob(y, height);

                int y_offset = comp_y * width;
                for (int dx = x - d->matrix_width; dx <= x + d->matrix_width; dx += d->offset_width) {
                    int comp_x = reflect_oob(x, width);

                    // current comparison pixel
                    float comp_r = src_r_plane[y_offset + comp_x];
                    float comp_g = src_g_plane[y_offset + comp_x];
                    float comp_b = src_b_plane[y_offset + comp_x];

                    float diff_r = comp_r - r;
                    float diff_g = comp_g - g;
                    float diff_b = comp_b - b;

#define SQUARE(x) ((x) * (x))
                    // is Euclidean distance below threshold?
                    if (d->threshold > (SQUARE(diff_r) + SQUARE(diff_g) + SQUARE(diff_b))) {
                        total_r += comp_r;
                        total_b += comp_b;
                        total_g += comp_g;
                        n++;
                    }
#undef SQUARE
                }
            }

            // assuming the user passes legal float values (0-1), then the output should always
            // be between 0 and 1. Therefore, to save a few extra if statements, I'm just not going
            // to bother clamping the value to the legal range.
            dst_r_plane[i] = total_r * MULTIPLIERS[n];
            dst_g_plane[i] = total_g * MULTIPLIERS[n];
            dst_b_plane[i] = total_b * MULTIPLIERS[n];
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

        ccd_run(src, dest, d, vsapi);

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

    d.matrix_width = vsapi->propGetFloat(in, "matrix_size", 0, &err);
    if (err)
        d.matrix_width = d.matrix_height = 12;
    else {
        d.matrix_height = vsapi->propGetFloat(in, "matrix_size", 1, &err);
        if (err)
            d.matrix_height = d.matrix_width;
    }

    d.offset_width = vsapi->propGetFloat(in, "offset_size", 0, &err);
    if (err)
        d.offset_width = d.offset_height = 12;
    else {
        d.offset_height = vsapi->propGetFloat(in, "offset_size", 1, &err);
        if (err)
            d.offset_height = d.offset_width;
    }

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

    // i hate this
    if (d.matrix_width < 3 || d.matrix_height < 3 || !(d.matrix_width & 1) || !(d.matrix_height & 1) || d.matrix_width > d.vi->width || d.matrix_height > d.vi->height) {
        vsapi->setError(out, "CCD: Matrix dimensions must be odd and between 3 and the dimensions of your clip");
        vsapi->freeNode(d.node);
    }

    if (d.offset_height % (d.matrix_width - 1) || d.offset_height % (d.matrix_width - 1)) {
        vsapi->setError(out, "CCD: Offsets must evenly divide into matrix - 1");
        vsapi->freeNode(d.node);
    }

    // ccd_run() expects offsets from central pixel, not full lengths
    d.matrix_width >>= 1;
    d.matrix_height >>= 1;

    data = reinterpret_cast<ccdData *>(malloc(sizeof(d)));
    *data = d;

    vsapi->createFilter(in, out, "ccd", ccdInit, ccdGetframe, ccdFree, fmParallel, 0, data, core);
}

VS_EXTERNAL_API(void)
VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc,
                      VSPlugin *plugin) {
    configFunc("com.eoe-scrad.ccd", "ccd", "chroma denoiser", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("CCD",
                 "clip:clip;"
                 "threshold:float:opt;"
                 "matrix_size:int[]:opt;"
                 "offset_size:int[]:opt;",
                 ccdCreate, nullptr, plugin);
}
