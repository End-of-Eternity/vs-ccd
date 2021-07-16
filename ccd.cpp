#include <cstring>
#include <cstdint>
#include <cstdlib>

#include <VapourSynth.h>
#include <VSHelper.h>

typedef struct ccdData {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    float threshold;
} ccdData;

static void VS_CC ccdInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    (void) in;
    (void) out;
    (void) core;

    auto *d = (ccdData *) *instanceData;

    vsapi->setVideoInfo(d->vi, 1, node);
}


static const VSFrameRef *
VS_CC ccdGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx,
                  VSCore *core, const VSAPI *vsapi) {
    auto *d = static_cast<ccdData *>(*instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter(n, d->node, frameCtx);
        // actual shit will go here

        return src;
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
    auto threshold = static_cast<float>(vsapi->propGetFloat(in, "threshold", 1, &err));//
    if (err)
        threshold = 4;

    if (threshold < 0) {
        vsapi->setError(out, "CCD: Threshold must be >= 0");
        vsapi->freeNode(d.node);//build?
    }

    d.threshold = threshold;

    d.vi = vsapi->getVideoInfo(d.node);

    if (!d.vi->format) {
        vsapi->setError(out, "CCD: Variable format clips are not supported.");
        vsapi->freeNode(d.node);
    }

    if (d.vi->format->id != 2000015) {  // ID of RGBS
        vsapi->setError(out, "CCD: Input clip must be RGBS");  //cool yea that works, yee, can make a wrapper later
        vsapi->freeNode(d.node);
    }

    data = (ccdData *) malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "ccd", ccdInit, ccdGetframe, ccdFree, fmParallel, 0, data, core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc,
                                            VSPlugin *plugin) {
    configFunc("com.eoe-scrad.ccd", "ccd", "chroma denoiser", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("CCD",
                 "clip:clip;"
                 "threshold:float:opt;", ccdCreate, nullptr, plugin);
}