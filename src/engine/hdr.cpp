// hdr.cpp: high-dynamic-range fp16 scene framebuffer + tonemapping / auto-exposure.
//
// The whole scene pass is redirected into a GL_RGBA16F framebuffer (scenefbo), then a
// final tonemap pass maps the linear HDR result to the 8-bit backbuffer. Everything here
// is gated behind the `hdr` var so that, when off, the engine renders exactly as before
// (straight to the backbuffer, scenefbo == 0).
//
// Inspired by the HDR pipeline described in Mitchell, "Shading in Valve's Source Engine"
// (SIGGRAPH 2006): a floating-point scene buffer, in-engine tonemapping and a luminance
// histogram auto-exposure that exposes toward a target percentile and eases over time.

#include "engine.h"

GLuint scenefbo = 0;

static GLuint hdrfb = 0, hdrtex = 0, hdrdb = 0;
static int hdrw = 0, hdrh = 0;

// luminance reduction chain for auto-exposure
static GLuint lumfb = 0, lumtex = 0;
static int lumsize = 0, lumlevels = 0;
static float curlumexposure = 1.0f;

void cleanuphdr(bool fullclean)
{
    if(hdrtex) { glDeleteTextures(1, &hdrtex); hdrtex = 0; }
    if(hdrdb) { glDeleteRenderbuffers_(1, &hdrdb); hdrdb = 0; }
    if(hdrfb) { glDeleteFramebuffers_(1, &hdrfb); hdrfb = 0; }
    if(lumtex) { glDeleteTextures(1, &lumtex); lumtex = 0; }
    if(lumfb) { glDeleteFramebuffers_(1, &lumfb); lumfb = 0; }
    hdrw = hdrh = lumsize = lumlevels = 0;
    if(fullclean) curlumexposure = 1.0f;
}

VARFP(hdr, 0, 0, 1, cleanuphdr());                 // master HDR toggle (off => identical to stock)
FVARP(hdrexposure, 1e-3f, 1.0f, 64.0f);            // manual exposure multiplier (used when autoexposure off)
VARP(hdrtonemap, 0, 3, 3);                         // 0=Reinhard 1=Hejl 2=ACES 3=neutral(stock-matching, default)
VARP(hdrautoexposure, 0, 0, 1);                    // eye-adaptation auto-exposure (off by default so hdr 1 ~= stock)
FVARP(hdrkey, 1e-3f, 0.10f, 4.0f);                 // middle-grey key for auto-exposure (lower = less washed)
FVARP(hdrminexposure, 1e-3f, 0.25f, 64.0f);        // clamp on adapted exposure
FVARP(hdrmaxexposure, 1e-3f, 8.0f, 256.0f);
FVARP(hdradaptrate, 0.0f, 2.5f, 64.0f);            // eye-adaptation speed (per second)
FVARP(hdrexposurepct, 0.0f, 0.5f, 1.0f);           // Valve-style: expose so this percentile of lit pixels hits the key (0.5 = median)
FVARP(hdrvoidcutoff, 0.0f, 1e-3f, 1.0f);           // luminance below this is treated as void/black and ignored by auto-exposure
VARP(hdrgamma, 100, 100, 400);                     // extra output gamma*100 (100 = none; Sauer is already display-referred)
FVARP(hdrcontrast, 0.5f, 1.0f, 3.0f);              // post-tonemap contrast (1.0 = none)

VARP(hdremissive, 0, 1, 1);                        // scale glow/emissive into HDR so it self-illuminates and blooms
FVARP(emissivescale, 0.0f, 2.0f, 64.0f);           // global emissive intensity multiplier (only when hdr active)
FVARP(modelglowclamp, 0.0f, 1.0f, 64.0f);          // clamp model glow under HDR (1 = LDR-like; raise to let model glow bloom)

VAR(debughdr, 0, 0, 1);

bool usehdr()
{
    return hdr && hasTF && hasFBO;
}

void setuphdr(int w, int h)
{
    if(hdrfb && hdrw==w && hdrh==h) return;
    cleanuphdr(false);

    hdrw = w;
    hdrh = h;

    glGenFramebuffers_(1, &hdrfb);
    glBindFramebuffer_(GL_FRAMEBUFFER, hdrfb);

    glGenTextures(1, &hdrtex);
    static const GLenum hdrfmts[] = { GL_RGBA16F, GL_RGB16F, GL_RGBA16F_ARB, GL_FALSE };
    GLenum colorfmt = GL_RGBA16F;
    int find = 0;
    do
    {
        colorfmt = hdrfmts[find];
        createtexture(hdrtex, w, h, NULL, 3, 1, colorfmt);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hdrtex, 0);
        if(glCheckFramebufferStatus_(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE) break;
    }
    while(hdrfmts[++find]);

    glGenRenderbuffers_(1, &hdrdb);
    glBindRenderbuffer_(GL_RENDERBUFFER, hdrdb);
    static const GLenum depthfmts[] = { GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT16, GL_FALSE };
    find = 0;
    do
    {
        glRenderbufferStorage_(GL_RENDERBUFFER, depthfmts[find], w, h);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, hdrdb);
        if(glCheckFramebufferStatus_(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE) break;
    }
    while(depthfmts[++find]);

    if(glCheckFramebufferStatus_(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)
        conoutf(CON_WARN, "HDR framebuffer not complete; disabling hdr");

    // luminance reduction target (square, power-of-two, with full mip chain)
    lumsize = 256;
    lumlevels = 0;
    for(int s = lumsize; s > 1; s >>= 1) lumlevels++;
    glGenFramebuffers_(1, &lumfb);
    glBindFramebuffer_(GL_FRAMEBUFFER, lumfb);
    glGenTextures(1, &lumtex);
    createtexture(lumtex, lumsize, lumsize, NULL, 3, 1, GL_R16F);
    glGenerateMipmap_(GL_TEXTURE_2D);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lumtex, 0);

    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
}

bool beginhdr()
{
    // global emissive boost applied by the glow/bump world shaders (1.0 == stock look)
    GLOBALPARAMF(glowscale, usehdr() && hdremissive ? emissivescale : 1.0f);
    // model glow is a visibility highlight, not an HDR area light; clamp it under HDR so it doesn't bloom out
    GLOBALPARAMF(modelglowmax, usehdr() ? modelglowclamp : 1.0e6f);
    // models use a x2 "overbright" texture convention that LDR clamps away; under HDR (linear, tonemapped) that
    // doubling blows bright-textured models out, so drop it -> models render at true albedo x light, like the world
    GLOBALPARAMF(modeltexscale, usehdr() ? 1.0f : 2.0f);

    if(!usehdr()) { scenefbo = 0; return false; }
    setuphdr(screenw, screenh);
    glBindFramebuffer_(GL_FRAMEBUFFER, hdrfb);
    scenefbo = hdrfb;
    glViewport(0, 0, hdrw, hdrh);
    return true;
}

// Auto-exposure following Valve's Source HDR approach ("Shading in Valve's Source Engine", SIGGRAPH 2006):
// build a luminance HISTOGRAM of the scene and expose so a target PERCENTILE of the lit pixels hits the key,
// rather than a single average. This is robust to outliers - a black void or a few tiny bright lights no
// longer hijack the result (the failure mode of the old log-average). Eased over time for eye adaptation.
static void updateexposure()
{
    if(!hdrautoexposure) { curlumexposure = hdrexposure; return; }

    // render per-pixel linear luminance, then reduce to a small grid we can read back cheaply
    glBindFramebuffer_(GL_FRAMEBUFFER, lumfb);
    glViewport(0, 0, lumsize, lumsize);
    SETSHADER(hdrluminance);
    glBindTexture(GL_TEXTURE_2D, hdrtex);
    screenquad(1, 1);

    const int LR = 64;                          // read-back resolution (small grid of block-averaged luminance)
    int level = 0; for(int s = lumsize; s > LR; s >>= 1) level++;
    glBindTexture(GL_TEXTURE_2D, lumtex);
    glGenerateMipmap_(GL_TEXTURE_2D);
    static float lumbuf[LR*LR];
    glGetTexImage(GL_TEXTURE_2D, level, GL_RED, GL_FLOAT, lumbuf);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);

    // histogram of log-luminance over the LIT cells (void/near-black excluded so it can't drag the exposure)
    enum { BUCKETS = 64 };
    int hist[BUCKETS]; memset(hist, 0, sizeof(hist));
    const float logmin = -8.0f, logmax = 6.0f;  // covers ~e^-8 (near black) .. e^6 (very bright)
    int total = 0;
    loopi(LR*LR)
    {
        float l = lumbuf[i];
        if(!(l > hdrvoidcutoff)) continue;      // skip void / black borders / unlit
        float t = (logf(l) - logmin) / (logmax - logmin);
        hist[clamp(int(t*BUCKETS), 0, BUCKETS-1)]++;
        total++;
    }

    float lum;
    if(total < 4) lum = hdrkey;                 // almost nothing lit on screen -> hold steady
    else
    {
        int want = max(int(total * clamp(hdrexposurepct, 0.0f, 1.0f)), 1), acc = 0, b = 0;
        for(; b < BUCKETS-1; b++) { acc += hist[b]; if(acc >= want) break; }
        lum = expf(logmin + ((b + 0.5f)/BUCKETS)*(logmax - logmin));   // luminance at the target percentile
    }
    if(!(lum > 0)) lum = hdrkey;
    float target = clamp(hdrkey/lum, hdrminexposure, hdrmaxexposure);
    float rate = clamp(hdradaptrate*(curtime/1000.0f), 0.0f, 1.0f);
    curlumexposure += (target - curlumexposure)*rate;
    if(!(curlumexposure > 0)) curlumexposure = target;
}

void endhdr()
{
    if(!usehdr() || !hdrfb) return;

    updateexposure();

    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    scenefbo = 0;
    glViewport(0, 0, screenw, screenh);

    glDisable(GL_BLEND);
    SETSHADER(hdrtonemap);
    LOCALPARAMF(hdrparams, curlumexposure, float(hdrtonemap), 100.0f/hdrgamma, hdrcontrast);
    glBindTexture(GL_TEXTURE_2D, hdrtex);
    screenquad(1, 1);
}

ICOMMAND(hdrinfo, "", (),
{
    extern bool lightmapshdr; extern bool lightmapsrnm;
    conoutf("HDR pipeline: %s  (tonemap %d [0=Reinhard 1=Hejl 2=ACES 3=neutral], exposure %.2f, autoexposure %d)",
        usehdr() ? "ON" : "off", hdrtonemap, hdrexposure, hdrautoexposure ? 1 : 0);
    conoutf("lightmaps loaded: hdr=%d rnm=%d   |   emissive: %s (scale %.1f)",
        lightmapshdr ? 1 : 0, lightmapsrnm ? 1 : 0, usehdr() && hdremissive ? "on" : "off", emissivescale);
    conoutf("light probes (model lighting): %s", haslightprobes() ? "ON" : "off");
});

void viewhdr()
{
    if(!usehdr() || !hdrfb) return;
    SETSHADER(screenrect);
    gle::colorf(1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, hdrtex);
    int size = min(screenw, screenh)/3;
    glViewport(0, 0, size, size);
    screenquad(1, 1);
    glViewport(0, 0, screenw, screenh);
}
