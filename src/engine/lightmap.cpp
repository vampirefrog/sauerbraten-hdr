#include "engine.h"

#define MAXLIGHTMAPTASKS 4096
#define LIGHTMAPBUFSIZE (2*1024*1024)

struct lightmapinfo;
struct lightmaptask;

struct lightmapworker
{
    uchar *buf;
    int bufstart, bufused;
    lightmapinfo *firstlightmap, *lastlightmap, *curlightmaps;
    cube *c;
    cubeext *ext;
    uchar *colorbuf;
    bvec *raybuf;
    uchar *ambient, *blur;
    vec *colordata, *raydata;
    vec *rnmdata[3];   // per-lumel HL2 radiosity-normal-map basis accumulators (RNM bake)
    vec *gidata;       // per-lumel HDR sky/GI irradiance (gi bake)
    int type, bpp, w, h, orient, rotate;
    VSlot *vslot;
    Slot *slot;
    vector<const extentity *> lights;
    ShadowRayCache *shadowraycache;
    BlendMapCache *blendmapcache;
    bool needspace, doneworking;
    SDL_cond *spacecond;
    SDL_Thread *thread;

    lightmapworker();
    ~lightmapworker();

    void reset();
    bool setupthread();
    void cleanupthread();

    static int work(void *data);
};

struct lightmapinfo
{
    lightmapinfo *next;
    cube *c;
    uchar *colorbuf;
    bvec *raybuf;
    bool packed;
    int type, w, h, bpp, bufsize, surface, layers;
};

struct lightmaptask
{
    ivec o;
    int size, usefaces, progress;
    cube *c;
    cubeext *ext;
    lightmapinfo *lightmaps;
    lightmapworker *worker;
};

struct lightmapext
{
    cube *c;
    cubeext *ext;
};

static vector<lightmapworker *> lightmapworkers;
static vector<lightmaptask> lightmaptasks[2];
static vector<lightmapext> lightmapexts;
static int packidx = 0, allocidx = 0;
static SDL_mutex *lightlock = NULL, *tasklock = NULL;
static SDL_cond *fullcond = NULL, *emptycond = NULL;

int lightmapping = 0;

vector<LightMap> lightmaps;

VARR(lightprecision, 1, 32, 1024);
VARR(lighterror, 1, 8, 16);
VARR(bumperror, 1, 3, 16);
VARR(lightlod, 0, 0, 10);
VARR(hdrlightmaps, 0, 0, 1);   // bake HDR (RGBE) lightmaps instead of clamped 8-bit (see hdr.cpp)
VARR(rnm, 0, 0, 1);            // bake 3-basis HL2 radiosity-normal-mapped lightmaps (implies hdrlightmaps)
VARR(softshadows, 0, 0, 256);  // area-light soft-shadow samples per light (0/1 = hard single-ray shadows)
FVARR(lightsize, 0, 8, 512);   // point/spot light area radius (world units) -> penumbra widens with distance
FVARR(sunsoftness, 0, 0.5f, 16); // sun angular radius (degrees) for soft sun shadows

// van der Corput radical inverse (base 2) for low-discrepancy sampling (jittered shadow/GI rays)
static inline float radicalinverse2(uint i)
{
    i = (i << 16) | (i >> 16);
    i = ((i & 0x55555555u) << 1) | ((i & 0xAAAAAAAAu) >> 1);
    i = ((i & 0x33333333u) << 2) | ((i & 0xCCCCCCCCu) >> 2);
    i = ((i & 0x0F0F0F0Fu) << 4) | ((i & 0xF0F0F0F0u) >> 4);
    i = ((i & 0x00FF00FFu) << 8) | ((i & 0xFF00FF00u) >> 8);
    return i * 2.3283064365386963e-10f;   // / 2^32
}
bvec ambientcolor(0x19, 0x19, 0x19), skylightcolor(0, 0, 0);
HVARFR(ambient, 1, 0x191919, 0xFFFFFF,
{
    if(ambient <= 255) ambient |= (ambient<<8) | (ambient<<16);
    ambientcolor = bvec((ambient>>16)&0xFF, (ambient>>8)&0xFF, ambient&0xFF);
});
HVARFR(skylight, 0, 0, 0xFFFFFF,
{
    if(skylight <= 255) skylight |= (skylight<<8) | (skylight<<16);
    skylightcolor = bvec((skylight>>16)&0xFF, (skylight>>8)&0xFF, skylight&0xFF);
});

extern void setupsunlight();
bvec sunlightcolor(0, 0, 0);
HVARFR(sunlight, 0, 0, 0xFFFFFF,
{
    if(sunlight <= 255) sunlight |= (sunlight<<8) | (sunlight<<16);
    sunlightcolor = bvec((sunlight>>16)&0xFF, (sunlight>>8)&0xFF, sunlight&0xFF);
    setupsunlight();
});
FVARFR(sunlightscale, 0, 1, 16, setupsunlight());
vec sunlightdir(0, 0, 1);
extern void setsunlightdir();
VARFR(sunlightyaw, 0, 0, 360, setsunlightdir());
VARFR(sunlightpitch, -90, 90, 90, setsunlightdir());

void setsunlightdir()
{
    sunlightdir = vec(sunlightyaw*RAD, sunlightpitch*RAD);
    loopk(3) if(fabs(sunlightdir[k]) < 1e-5f) sunlightdir[k] = 0;
    sunlightdir.normalize();
    setupsunlight();
}

entity sunlightent;
void setupsunlight()
{
    memclear(sunlightent);
    sunlightent.type = ET_LIGHT;
    sunlightent.attr1 = 0;
    sunlightent.attr2 = int(sunlightcolor.x*sunlightscale);
    sunlightent.attr3 = int(sunlightcolor.y*sunlightscale);
    sunlightent.attr4 = int(sunlightcolor.z*sunlightscale);
    float dist = min(min(sunlightdir.x ? 1/fabs(sunlightdir.x) : 1e16f, sunlightdir.y ? 1/fabs(sunlightdir.y) : 1e16f), sunlightdir.z ? 1/fabs(sunlightdir.z) : 1e16f);
    sunlightent.o = vec(sunlightdir).mul(dist*worldsize).add(vec(worldsize/2, worldsize/2, worldsize/2));
}

VARR(skytexturelight, 0, 1, 1);
extern int useskytexture;

static const surfaceinfo brightsurfaces[6] =
{
    brightsurface,
    brightsurface,
    brightsurface,
    brightsurface,
    brightsurface,
    brightsurface
};

void brightencube(cube &c)
{
    if(!c.ext) newcubeext(c, 0, false);
    memcpy(c.ext->surfaces, brightsurfaces, sizeof(brightsurfaces));
}

void setsurfaces(cube &c, const surfaceinfo *surfs, const vertinfo *verts, int numverts)
{
    if(!c.ext || c.ext->maxverts < numverts) newcubeext(c, numverts, false);
    memcpy(c.ext->surfaces, surfs, sizeof(c.ext->surfaces));
    memcpy(c.ext->verts(), verts, numverts*sizeof(vertinfo));
}

void setsurface(cube &c, int orient, const surfaceinfo &src, const vertinfo *srcverts, int numsrcverts)
{
    int dstoffset = 0;
    if(!c.ext) newcubeext(c, numsrcverts, true);
    else
    {
        int numbefore = 0, beforeoffset = 0;
        loopi(orient)
        {
            surfaceinfo &surf = c.ext->surfaces[i];
            int numverts = surf.totalverts();
            if(!numverts) continue;
            numbefore += numverts;
            beforeoffset = surf.verts + numverts;
        }
        int numafter = 0, afteroffset = c.ext->maxverts;
        for(int i = 5; i > orient; i--)
        {
            surfaceinfo &surf = c.ext->surfaces[i];
            int numverts = surf.totalverts();
            if(!numverts) continue;
            numafter += numverts;
            afteroffset = surf.verts;
        }
        if(afteroffset - beforeoffset >= numsrcverts) dstoffset = beforeoffset;
        else
        {
            cubeext *ext = c.ext;
            if(numbefore + numsrcverts + numafter > c.ext->maxverts)
            {
                ext = growcubeext(c.ext, numbefore + numsrcverts + numafter);
                memcpy(ext->surfaces, c.ext->surfaces, sizeof(ext->surfaces));
            }
            int offset = 0;
            if(numbefore == beforeoffset)
            {
                if(numbefore && c.ext != ext) memcpy(ext->verts(), c.ext->verts(), numbefore*sizeof(vertinfo));
                offset = numbefore;
            }
            else loopi(orient)
            {
                surfaceinfo &surf = ext->surfaces[i];
                int numverts = surf.totalverts();
                if(!numverts) continue;
                memmove(ext->verts() + offset, c.ext->verts() + surf.verts, numverts*sizeof(vertinfo));
                surf.verts = offset;
                offset += numverts;
            }
            dstoffset = offset;
            offset += numsrcverts;
            if(numafter && offset > afteroffset)
            {
                offset += numafter;
                for(int i = 5; i > orient; i--)
                {
                    surfaceinfo &surf = ext->surfaces[i];
                    int numverts = surf.totalverts();
                    if(!numverts) continue;
                    offset -= numverts;
                    memmove(ext->verts() + offset, c.ext->verts() + surf.verts, numverts*sizeof(vertinfo));
                    surf.verts = offset;
                }
            }
            if(c.ext != ext) setcubeext(c, ext);
        }
    }
    surfaceinfo &dst = c.ext->surfaces[orient];
    dst = src;
    dst.verts = dstoffset;
    if(srcverts) memcpy(c.ext->verts() + dstoffset, srcverts, numsrcverts*sizeof(vertinfo));
}

// quality parameters, set by the calclight arg
VARN(lmshadows, lmshadows_, 0, 2, 2);
VARN(lmaa, lmaa_, 0, 3, 3);
VARN(lerptjoints, lerptjoints_, 0, 1, 1);
static int lmshadows = 2, lmaa = 3, lerptjoints = 1;

static uint progress = 0, taskprogress = 0;
static GLuint progresstex = 0;
static int progresstexticks = 0, progresslightmap = -1;

bool calclight_canceled = false;
volatile bool check_calclight_progress = false;

void check_calclight_canceled()
{
    if(interceptkey(SDLK_ESCAPE))
    {
        calclight_canceled = true;
        loopv(lightmapworkers) lightmapworkers[i]->doneworking = true;
    }
    if(!calclight_canceled) check_calclight_progress = false;
}

void show_calclight_progress()
{
    float bar1 = float(progress) / float(allocnodes);
    defformatstring(text1, "%d%% using %d textures", int(bar1 * 100), lightmaps.length());

    if(LM_PACKW <= hwtexsize && !progresstex)
    {
        glGenTextures(1, &progresstex);
        createtexture(progresstex, LM_PACKW, LM_PACKH, NULL, 3, 1, GL_RGB);
    }

    // only update once a sec (4 * 250 ms ticks) to not kill performance
    if(progresstex && !calclight_canceled && progresslightmap >= 0 && !(progresstexticks++ % 4))
    {
        if(tasklock) SDL_LockMutex(tasklock);
        LightMap &lm = lightmaps[progresslightmap];
        uchar *data = lm.data;
        int bpp = lm.bpp;
        if(tasklock) SDL_UnlockMutex(tasklock);
        glBindTexture(GL_TEXTURE_2D, progresstex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, texalign(data, LM_PACKW, bpp));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, LM_PACKW, LM_PACKH, bpp > 3 ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE, data);
    }
    renderprogress(bar1, text1, progresstexticks ? progresstex : 0);
}

#define CHECK_PROGRESS_LOCKED(exit, before, after) CHECK_CALCLIGHT_PROGRESS_LOCKED(exit, show_calclight_progress, before, after)
#define CHECK_PROGRESS(exit) CHECK_PROGRESS_LOCKED(exit, , )

bool PackNode::insert(ushort &tx, ushort &ty, ushort tw, ushort th)
{
    if((available < tw && available < th) || w < tw || h < th)
        return false;
    if(child1)
    {
        bool inserted = child1->insert(tx, ty, tw, th) ||
                        child2->insert(tx, ty, tw, th);
        available = max(child1->available, child2->available);
        if(!available) clear();
        return inserted;
    }
    if(w == tw && h == th)
    {
        available = 0;
        tx = x;
        ty = y;
        return true;
    }

    if(w - tw > h - th)
    {
        child1 = new PackNode(x, y, tw, h);
        child2 = new PackNode(x + tw, y, w - tw, h);
    }
    else
    {
        child1 = new PackNode(x, y, w, th);
        child2 = new PackNode(x, y + th, w, h - th);
    }

    bool inserted = child1->insert(tx, ty, tw, th);
    available = max(child1->available, child2->available);
    return inserted;
}

bool LightMap::insert(ushort &tx, ushort &ty, uchar *src, ushort tw, ushort th)
{
    if((type&LM_TYPE) != LM_BUMPMAP1 && !packroot.insert(tx, ty, tw, th))
        return false;

    copy(tx, ty, src, tw, th);
    return true;
}

void LightMap::copy(ushort tx, ushort ty, uchar *src, ushort tw, ushort th)
{
    uchar *dst = data + bpp * tx + ty * bpp * LM_PACKW;
    loopi(th)
    {
        memcpy(dst, src, bpp * tw);
        dst += bpp * LM_PACKW;
        src += bpp * tw;
    }
    ++lightmaps;
    lumels += tw * th;
}

static void insertunlit(int i)
{
    LightMap &l = lightmaps[i];
    if((l.type&LM_TYPE) == LM_BUMPMAP1)
    {
        l.unlitx = l.unlity = -1;
        return;
    }
    ushort x, y;
    uchar unlit[4] = { ambientcolor[0], ambientcolor[1], ambientcolor[2], 255 };
    if(l.insert(x, y, unlit, 1, 1))
    {
        if((l.type&LM_TYPE) == LM_BUMPMAP0)
        {
            bvec front(128, 128, 255);
            ASSERT(lightmaps[i+1].insert(x, y, front.v, 1, 1));
        }
        l.unlitx = x;
        l.unlity = y;
    }
}

struct layoutinfo
{
    ushort x, y, lmid;
    uchar w, h;
};

static void insertlightmap(lightmapinfo &li, layoutinfo &si)
{
    loopv(lightmaps)
    {
        if(lightmaps[i].type == li.type && lightmaps[i].insert(si.x, si.y, li.colorbuf, si.w, si.h))
        {
            si.lmid = i + LMID_RESERVED;
            if((li.type&LM_TYPE) == LM_BUMPMAP0) ASSERT(lightmaps[i+1].insert(si.x, si.y, (uchar *)li.raybuf, si.w, si.h));
            return;
        }
    }

    progresslightmap = lightmaps.length();

    si.lmid = lightmaps.length() + LMID_RESERVED;
    LightMap &l = lightmaps.add();
    l.type = li.type;
    l.bpp = li.bpp;
    l.data = new uchar[li.bpp*LM_PACKW*LM_PACKH];
    memset(l.data, 0, li.bpp*LM_PACKW*LM_PACKH);
    ASSERT(l.insert(si.x, si.y, li.colorbuf, si.w, si.h));
    if((li.type&LM_TYPE) == LM_BUMPMAP0)
    {
        LightMap &r = lightmaps.add();
        r.type = LM_BUMPMAP1 | (li.type&~LM_TYPE);
        r.bpp = 3;
        r.data = new uchar[3*LM_PACKW*LM_PACKH];
        memset(r.data, 0, 3*LM_PACKW*LM_PACKH);
        ASSERT(r.insert(si.x, si.y, (uchar *)li.raybuf, si.w, si.h));
    }
}

static void copylightmap(lightmapinfo &li, layoutinfo &si)
{
    lightmaps[si.lmid-LMID_RESERVED].copy(si.x, si.y, li.colorbuf, si.w, si.h);
    if((li.type&LM_TYPE)==LM_BUMPMAP0 && lightmaps.inrange(si.lmid+1-LMID_RESERVED))
        lightmaps[si.lmid+1-LMID_RESERVED].copy(si.x, si.y, (uchar *)li.raybuf, si.w, si.h);
}

static inline bool htcmp(const lightmapinfo &k, const layoutinfo &v)
{
    int kw = k.w, kh = k.h;
    if(kw != v.w || kh != v.h) return false;
    LightMap &vlm = lightmaps[v.lmid - LMID_RESERVED];
    int ktype = k.type;
    if(ktype != vlm.type) return false;
    int kbpp = k.bpp;
    const uchar *kcolor = k.colorbuf, *vcolor = vlm.data + kbpp*(v.x + v.y*LM_PACKW);
    loopi(kh)
    {
        if(memcmp(kcolor, vcolor, kbpp*kw)) return false;
        kcolor += kbpp*kw;
        vcolor += kbpp*LM_PACKW;
    }
    if((ktype&LM_TYPE) != LM_BUMPMAP0) return true;
    const bvec *kdir = k.raybuf, *vdir = (const bvec *)lightmaps[v.lmid+1 - LMID_RESERVED].data + (v.x + v.y*LM_PACKW);
    loopi(kh)
    {
        if(memcmp(kdir, vdir, kw*sizeof(bvec))) return false;
        kdir += kw;
        vdir += LM_PACKW;
    }
    return true;
}

static inline uint hthash(const lightmapinfo &k)
{
    int kw = k.w, kh = k.h, kbpp = k.bpp;
    uint hash = kw + (kh<<8);
    const uchar *color = k.colorbuf;
    loopi(kw*kh)
    {
       hash ^= color[0] + (color[1] << 4) + (color[2] << 8);
       color += kbpp;
    }
    return hash;
}

static hashset<layoutinfo> compressed;

VAR(lightcompress, 0, 3, 6);

static bool packlightmap(lightmapinfo &l, layoutinfo &surface)
{
    surface.w = l.w;
    surface.h = l.h;
    if((int)l.w <= lightcompress && (int)l.h <= lightcompress)
    {
        layoutinfo *val = compressed.access(l);
        if(!val)
        {
            insertlightmap(l, surface);
            compressed[l] = surface;
        }
        else
        {
            surface.x = val->x;
            surface.y = val->y;
            surface.lmid = val->lmid;
            return false;
        }
    }
    else insertlightmap(l, surface);
    return true;
}

static void updatelightmap(const layoutinfo &surface)
{
    if(max(LM_PACKW, LM_PACKH) > hwtexsize || !lightmaps.inrange(surface.lmid-LMID_RESERVED)) return;

    LightMap &lm = lightmaps[surface.lmid-LMID_RESERVED];
    if(lm.tex < 0)
    {
        lm.offsetx = lm.offsety = 0;
        lm.tex = lightmaptexs.length();
        LightMapTexture &tex = lightmaptexs.add();
        tex.type = lm.type;
        tex.w = LM_PACKW;
        tex.h = LM_PACKH;
        tex.unlitx = lm.unlitx;
        tex.unlity = lm.unlity;
        glGenTextures(1, &tex.id);
        createtexture(tex.id, tex.w, tex.h, NULL, 3, 1, tex.type&(LM_ALPHA|LM_HDR) ? GL_RGBA : GL_RGB);
        if((lm.type&LM_TYPE)==LM_BUMPMAP0 && lightmaps.inrange(surface.lmid+1-LMID_RESERVED))
        {
            LightMap &lm2 = lightmaps[surface.lmid+1-LMID_RESERVED];
            lm2.offsetx = lm2.offsety = 0;
            lm2.tex = lightmaptexs.length();
            LightMapTexture &tex2 = lightmaptexs.add();
            tex2.type = (lm.type&~LM_TYPE) | LM_BUMPMAP0;
            tex2.w = LM_PACKW;
            tex2.h = LM_PACKH;
            tex2.unlitx = lm2.unlitx;
            tex2.unlity = lm2.unlity;
            glGenTextures(1, &tex2.id);
            createtexture(tex2.id, tex2.w, tex2.h, NULL, 3, 1, GL_RGB);
        }
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, LM_PACKW);

    glBindTexture(GL_TEXTURE_2D, lightmaptexs[lm.tex].id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, lm.offsetx + surface.x, lm.offsety + surface.y, surface.w, surface.h, lm.type&(LM_ALPHA|LM_HDR) ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE, &lm.data[(surface.y*LM_PACKW + surface.x)*lm.bpp]);
    if((lm.type&LM_TYPE)==LM_BUMPMAP0 && lightmaps.inrange(surface.lmid+1-LMID_RESERVED))
    {
        LightMap &lm2 = lightmaps[surface.lmid+1-LMID_RESERVED];
        glBindTexture(GL_TEXTURE_2D, lightmaptexs[lm2.tex].id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, lm2.offsetx + surface.x, lm2.offsety + surface.y, surface.w, surface.h, GL_RGB, GL_UNSIGNED_BYTE, &lm2.data[(surface.y*LM_PACKW + surface.x)*3]);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}


// Half-Life 2 radiosity-normal-map basis (tangent space), per Mitchell 2006
extern const vec orientation_tangent[8][3];
extern const vec orientation_bitangent[8][3];
static const vec hl2basis[3] =
{
    vec(-0.40824829f,  0.70710678f, 0.57735027f),
    vec(-0.40824829f, -0.70710678f, 0.57735027f),
    vec( 0.81649658f,  0.0f,        0.57735027f)
};

// build an orthonormal basis (u,v) perpendicular to dir
static inline void perpbasis(const vec &dir, vec &u, vec &v)
{
    u = vec(fabs(dir.z) < 0.9f ? vec(0, 0, 1) : vec(1, 0, 0));
    u.sub(vec(dir).mul(dir.dot(u))).normalize();
    v.cross(dir, u);
}

// area-light visibility 0..1: jitter `softshadows` shadow rays across a disk of radius `size` at the
// light (perpendicular to the light direction) and average occlusion. Penumbra widens with distance
// from the caster automatically (the disk subtends a wider shadow gradient on farther receivers).
static float lightvisibility(lightmapworker *w, const vec &target, const vec &lightpos, float size, float mag, float tolerance)
{
    vec dir = vec(lightpos).sub(target).mul(1.0f/mag), u, v;
    perpbasis(dir, u, v);
    float jit = radicalinverse2(uint(target.x*53.0f) ^ (uint(target.y*131.0f)*2654435761u) ^ uint(target.z*97.0f));
    int hits = 0, n = max(softshadows, 1);
    loopi(n)
    {
        float r = sqrtf((i + 0.5f)/n)*size, phi = 2*M_PI*(radicalinverse2(i) + jit);
        vec lp = vec(lightpos).add(vec(u).mul(r*cosf(phi))).add(vec(v).mul(r*sinf(phi)));
        vec sray = vec(target).sub(lp);
        float smag = sray.magnitude();
        if(smag < 1e-3f) { hits++; continue; }
        sray.mul(1.0f/smag);
        if(shadowray(w->shadowraycache, lp, sray, smag - tolerance, RAY_SHADOW | (lmshadows > 1 ? RAY_ALPHAPOLY : 0)) >= smag - tolerance) hits++;
    }
    return float(hits)/n;
}

// the sun's angular radius (degrees) used for soft shadows: when atmo is on, take it from the visible
// Nishita sun disk (atmosundisksize is a diameter) so shadow penumbra matches the sky's sun; else sunsoftness
extern int atmo;
extern float atmosundisksize;
static inline float sunangularradius() { return atmo ? 0.5f*atmosundisksize : sunsoftness; }

// area-light visibility for the sun: jitter the shadow direction over the sun's angular disk
static float sunvisibility(lightmapworker *w, const vec &target, float tolerance, int flags)
{
    vec u, v;
    perpbasis(sunlightdir, u, v);
    float tana = tanf(sunangularradius()*RAD), jit = radicalinverse2(uint(target.x*97.0f) ^ (uint(target.y*53.0f)*2654435761u) ^ uint(target.z*131.0f));
    int hits = 0, n = max(softshadows, 1);
    loopi(n)
    {
        float r = sqrtf((i + 0.5f)/n)*tana, phi = 2*M_PI*(radicalinverse2(i) + jit);
        vec dir = vec(sunlightdir).add(vec(u).mul(r*cosf(phi))).add(vec(v).mul(r*sinf(phi))).normalize();
        if(shadowray(w->shadowraycache, vec(dir).mul(tolerance).add(target), dir, 1e16f, flags) > 1e15f) hits++;
    }
    return float(hits)/n;
}

static uint generatelumel(lightmapworker *w, const float tolerance, uint lightmask, const vector<const extentity *> &lights, const vec &target, const vec &normal, vec &sample, int x, int y)
{
    vec avgray(0, 0, 0);
    float r = 0, g = 0, b = 0;
    uint lightused = 0;
    bool rnmpass = (w->type&LM_TYPE) == LM_RNM0;
    vec rnmS(0, 0, 0), rnmT(0, 0, 0), rnmN(0, 0, 1);
    if(rnmpass)
    {
        rnmS = orientation_tangent[w->rotate][dimension(w->orient)];
        rnmT = orientation_bitangent[w->rotate][dimension(w->orient)];
        rnmN = normal;
        rnmN.normalize();
        rnmN.orthonormalize(rnmS, rnmT);
        rnmS.normalize();
        rnmT.normalize();
    }
    int rnmidx = y*w->w + x;
    loopv(lights)
    {
        if(lightmask&(1<<i)) continue;
        const extentity &light = *lights[i];
        vec ray = target;
        ray.sub(light.o);
        float mag = ray.magnitude();
        if(!mag) continue;
        float attenuation = 1;
        if(light.attr1)
        {
            attenuation -= mag / float(light.attr1);
            if(attenuation <= 0) continue;
        }
        ray.mul(1.0f / mag);
        float angle = -ray.dot(normal);
        if(angle <= 0) continue;
        if(light.attached && light.attached->type==ET_SPOTLIGHT)
        {
            vec spot = vec(light.attached->o).sub(light.o).normalize();
            float maxatten = sincos360[clamp(int(light.attached->attr1), 1, 89)].x, spotatten = (ray.dot(spot) - maxatten) / (1 - maxatten);
            if(spotatten <= 0) continue;
            attenuation *= spotatten;
        }
        if(lmshadows && mag)
        {
            if(softshadows > 1 && lightsize > 0)
            {   // area light: average visibility over the light's disk -> soft, distance-widening penumbra
                float vis = lightvisibility(w, target, light.o, lightsize, mag, tolerance);
                if(vis <= 0) continue;
                attenuation *= vis;
            }
            else if(shadowray(w->shadowraycache, light.o, ray, mag - tolerance, RAY_SHADOW | (lmshadows > 1 ? RAY_ALPHAPOLY : 0)) < mag - tolerance) continue;
        }
        lightused |= 1<<i;
        float intensity;
        switch(w->type&LM_TYPE)
        {
            case LM_BUMPMAP0:
                intensity = attenuation;
                avgray.add(ray.mul(-attenuation));
                break;
            default:
                intensity = angle * attenuation;
                break;
        }
        r += intensity * float(light.attr2);
        g += intensity * float(light.attr3);
        b += intensity * float(light.attr4);
        if(rnmpass)
        {
            vec tolight = vec(ray).neg(), Lt(rnmS.dot(tolight), rnmT.dot(tolight), rnmN.dot(tolight)),
                lc(light.attr2, light.attr3, light.attr4);
            loopk(3) { float wgt = hl2basis[k].dot(Lt); if(wgt > 0) w->rnmdata[k][rnmidx].add(vec(lc).mul(attenuation*wgt)); }
        }
    }
    if(sunlight)
    {
        float angle = sunlightdir.dot(normal);
        if(angle > 0)
        {
            int sunflags = RAY_SHADOW | (lmshadows > 1 ? RAY_ALPHAPOLY : 0) | (skytexturelight ? RAY_SKIPSKY | (useskytexture ? RAY_SKYTEX : 0) : 0);
            float sunvis = !lmshadows ? 1.0f
                         : (softshadows > 1 && sunangularradius() > 0 ? sunvisibility(w, target, tolerance, sunflags)   // soft area sun (atmo sun size if atmo on)
                            : (shadowray(w->shadowraycache, vec(sunlightdir).mul(tolerance).add(target), sunlightdir, 1e16f, sunflags) > 1e15f ? 1.0f : 0.0f));
            if(sunvis > 0)
            {
                float intensity;
                switch(w->type&LM_TYPE)
                {
                    case LM_BUMPMAP0:
                        intensity = sunvis;
                        avgray.add(vec(sunlightdir).mul(sunvis));
                        break;
                    default:
                        intensity = angle * sunvis;
                        break;
                }
                r += intensity * (sunlightcolor.x*sunlightscale);
                g += intensity * (sunlightcolor.y*sunlightscale);
                b += intensity * (sunlightcolor.z*sunlightscale);
                if(rnmpass)
                {
                    vec Lt(rnmS.dot(sunlightdir), rnmT.dot(sunlightdir), rnmN.dot(sunlightdir)),
                        lc(sunlightcolor.x*sunlightscale*sunvis, sunlightcolor.y*sunlightscale*sunvis, sunlightcolor.z*sunlightscale*sunvis);
                    loopk(3) { float wgt = hl2basis[k].dot(Lt); if(wgt > 0) w->rnmdata[k][rnmidx].add(vec(lc).mul(wgt)); }
                }
            }
        }
    }
    switch(w->type&LM_TYPE)
    {
        case LM_BUMPMAP0:
            if(avgray.iszero()) break;
            // transform to tangent space
            extern const vec orientation_tangent[8][3];
            extern const vec orientation_bitangent[8][3];
            vec S(orientation_tangent[w->rotate][dimension(w->orient)]),
                T(orientation_bitangent[w->rotate][dimension(w->orient)]);
            normal.orthonormalize(S, T);
            avgray.normalize();
            w->raydata[y*w->w+x].add(vec(S.dot(avgray)/S.magnitude(), T.dot(avgray)/T.magnitude(), normal.dot(avgray)));
            break;
    }
    if(w->type&LM_HDR)
    {
        // keep unbounded HDR radiance; the 255 ceiling is only an 8-bit storage limit
        sample.x = max(r, float(ambientcolor[0]));
        sample.y = max(g, float(ambientcolor[1]));
        sample.z = max(b, float(ambientcolor[2]));
    }
    else
    {
        sample.x = min(255.0f, max(r, float(ambientcolor[0])));
        sample.y = min(255.0f, max(g, float(ambientcolor[1])));
        sample.z = min(255.0f, max(b, float(ambientcolor[2])));
    }
    return lightused;
}

static bool lumelsample(const vec &sample, int aasample, int stride)
{
    if(sample.x >= int(ambientcolor[0])+1 || sample.y >= int(ambientcolor[1])+1 || sample.z >= int(ambientcolor[2])+1) return true;
#define NCHECK(n) \
    if((n).x >= int(ambientcolor[0])+1 || (n).y >= int(ambientcolor[1])+1 || (n).z >= int(ambientcolor[2])+1) \
        return true;
    const vec *n = &sample - stride - aasample;
    NCHECK(n[0]); NCHECK(n[aasample]); NCHECK(n[2*aasample]);
    n += stride;
    NCHECK(n[0]); NCHECK(n[2*aasample]);
    n += stride;
    NCHECK(n[0]); NCHECK(n[aasample]); NCHECK(n[2*aasample]);
    return false;
}

// hemisphere sampling directions shared by the sky-occlusion test and the GI gather
static const vec hemirays[17] =
{
    vec(cosf(21*RAD)*cosf(50*RAD), sinf(21*RAD)*cosf(50*RAD), sinf(50*RAD)),
    vec(cosf(111*RAD)*cosf(50*RAD), sinf(111*RAD)*cosf(50*RAD), sinf(50*RAD)),
    vec(cosf(201*RAD)*cosf(50*RAD), sinf(201*RAD)*cosf(50*RAD), sinf(50*RAD)),
    vec(cosf(291*RAD)*cosf(50*RAD), sinf(291*RAD)*cosf(50*RAD), sinf(50*RAD)),
    vec(cosf(66*RAD)*cosf(70*RAD), sinf(66*RAD)*cosf(70*RAD), sinf(70*RAD)),
    vec(cosf(156*RAD)*cosf(70*RAD), sinf(156*RAD)*cosf(70*RAD), sinf(70*RAD)),
    vec(cosf(246*RAD)*cosf(70*RAD), sinf(246*RAD)*cosf(70*RAD), sinf(70*RAD)),
    vec(cosf(336*RAD)*cosf(70*RAD), sinf(336*RAD)*cosf(70*RAD), sinf(70*RAD)),
    vec(0, 0, 1),
    vec(cosf(43*RAD)*cosf(60*RAD), sinf(43*RAD)*cosf(60*RAD), sinf(60*RAD)),
    vec(cosf(133*RAD)*cosf(60*RAD), sinf(133*RAD)*cosf(60*RAD), sinf(60*RAD)),
    vec(cosf(223*RAD)*cosf(60*RAD), sinf(223*RAD)*cosf(60*RAD), sinf(60*RAD)),
    vec(cosf(313*RAD)*cosf(60*RAD), sinf(313*RAD)*cosf(60*RAD), sinf(60*RAD)),
    vec(cosf(88*RAD)*cosf(80*RAD), sinf(88*RAD)*cosf(80*RAD), sinf(80*RAD)),
    vec(cosf(178*RAD)*cosf(80*RAD), sinf(178*RAD)*cosf(80*RAD), sinf(80*RAD)),
    vec(cosf(268*RAD)*cosf(80*RAD), sinf(268*RAD)*cosf(80*RAD), sinf(80*RAD)),
    vec(cosf(358*RAD)*cosf(80*RAD), sinf(358*RAD)*cosf(80*RAD), sinf(80*RAD)),
};

static void calcskylight(lightmapworker *w, const vec &o, const vec &normal, float tolerance, uchar *skylight, int flags = RAY_ALPHAPOLY, extentity *t = NULL)
{
    const vec *rays = hemirays;
    flags |= RAY_SHADOW;
    if(skytexturelight) flags |= RAY_SKIPSKY | (useskytexture ? RAY_SKYTEX : 0);
    int hit = 0;
    if(w) loopi(17)
    {
        if(normal.dot(rays[i])>=0 && shadowray(w->shadowraycache, vec(rays[i]).mul(tolerance).add(o), rays[i], 1e16f, flags, t)>1e15f) hit++;
    }
    else loopi(17)
    {
        if(normal.dot(rays[i])>=0 && shadowray(vec(rays[i]).mul(tolerance).add(o), rays[i], 1e16f, flags, t)>1e15f) hit++;
    }

    loopk(3) skylight[k] = uchar(ambientcolor[k] + (max(skylightcolor[k], ambientcolor[k]) - ambientcolor[k])*hit/17.0f);
}

static inline bool hasskylight()
{
    return skylightcolor[0]>ambientcolor[0] || skylightcolor[1]>ambientcolor[1] || skylightcolor[2]>ambientcolor[2];
}

// ---- global illumination bake (HDR sky image-based lighting + indirect bounce) ----
VARR(gi, 0, 0, 1);                  // bake sky IBL + indirect GI into HDR lightmaps (needs hdrlightmaps)
VARR(gibounces, 1, 2, 4);           // number of indirect light bounces
VARR(girays, 8, 64, 1024);          // cosine-weighted hemisphere samples per lumel (quality vs bake time)
FVARR(giscale, 0, 1, 16);           // GI intensity
FVARR(gialbedo, 0, 0.7f, 1);        // assumed surface reflectance for indirect bounces
FVARR(giemissive, 0, 1, 64);        // how strongly emissive (glow) textures act as GI area light sources
VARR(giemissivetexel, 0, 0, 1);     // 0 = per-slot average emission (fast, low-noise); 1 = sample the glow texture per-texel at the hit UV (vscale/offset/rotation matter, needs more girays)
VARR(giemitdirect, 0, 1, 1);        // 1 = explicit area-light sampling of emissive surfaces (Source/VRAD texlight style: smooth, no grazing cutoff); 0 = old hemisphere-gather capture (noisy)
FVARR(giemitcell, 1, 16, 256);      // emissive-surface subdivision cell size (world units) for the area-light form factor (smaller = more accurate near field, slower)

extern int atmo;
extern vec atmospherelight(const vec &dir);
extern bool skyboxlightloaded();
extern vec skyboxlight(const vec &dir);
VARR(giskybox, 0, 0, 1);   // sample the loaded skybox image (incl. its sun) as the GI light source

// HDR radiance of the sky in a world direction (0-255 light scale)
static vec skyradiance(const vec &dir)
{
    // atmo, when on, renders/overrides the visible sky, so the GI must match it (NOT the now-hidden skybox)
    if(atmo) return atmospherelight(dir);                          // procedural Nishita atmosphere
    if(giskybox && skyboxlightloaded()) return skyboxlight(dir);   // else sample the loaded skybox image
    vec sky = hasskylight() ? vec(skylightcolor.x, skylightcolor.y, skylightcolor.z)
                            : vec(ambientcolor.x, ambientcolor.y, ambientcolor.z);
    if(sunlight)   // a bright HDR sun lobe toward the sun direction (sun-from-sky)
    {
        float d = max(dir.dot(sunlightdir), 0.0f);
        sky.add(vec(sunlightcolor.x, sunlightcolor.y, sunlightcolor.z).mul(sunlightscale*powf(d, 16.0f)));
    }
    return sky;
}

// thread-safe direct-light gather at an arbitrary point/normal (uses the worker's per-task light list
// and per-thread shadow cache). Returns incident radiance in 0-255 light scale.
static vec gatherdirect(lightmapworker *w, const vec &target, const vec &normal)
{
    float r = 0, g = 0, b = 0;
    loopv(w->lights)
    {
        const extentity &e = *w->lights[i];
        vec ray = vec(target).sub(e.o);
        float mag = ray.magnitude();
        if(e.attr1 && mag >= float(e.attr1)) continue;
        if(mag < 1e-4f) continue;
        ray.div(mag);
        float angle = -ray.dot(normal);
        if(angle <= 0) continue;
        float atten = e.attr1 ? 1 - mag/float(e.attr1) : 1;
        if(shadowray(w->shadowraycache, e.o, ray, mag - 1.0f, RAY_SHADOW | RAY_POLY) < mag - 1.0f) continue;
        float in = angle*atten;
        r += in*e.attr2; g += in*e.attr3; b += in*e.attr4;
    }
    if(sunlight)
    {
        // sun visibility must pass THROUGH sky-textured faces when skytexturelight is on (same as the primary
        // sun in generatelumel) -- otherwise a sunlit surface seen via a sky opening bounces no sun.
        int sunflags = RAY_SHADOW | RAY_POLY | (skytexturelight ? RAY_SKIPSKY | (useskytexture ? RAY_SKYTEX : 0) : 0);
        float angle = sunlightdir.dot(normal);
        if(angle > 0 && shadowray(w->shadowraycache, target, sunlightdir, 1e16f, sunflags) > 1e15f)
        {
            r += angle*sunlightcolor.x*sunlightscale;
            g += angle*sunlightcolor.y*sunlightscale;
            b += angle*sunlightcolor.z*sunlightscale;
        }
    }
    return vec(r, g, b);
}

// thread-safe surface lookup at a bounce hit (lookupcube with explicit out-params + tsize 0 is read-only):
// returns the hit surface's albedo (reflectance) and emission (so emissive textures act as GI area lights)
static void hitsurface(const vec &hitpos, const vec &ray, vec &albedo, vec &emission)
{
    ivec ro;
    int rsize;
    cube &c = lookupcube(ivec(int(hitpos.x), int(hitpos.y), int(hitpos.z)), 0, ro, rsize);
    vec ar(fabs(ray.x), fabs(ray.y), fabs(ray.z));
    int dim = ar.x >= ar.y ? (ar.x >= ar.z ? 0 : 2) : (ar.y >= ar.z ? 1 : 2);
    int orient = 2*dim + (ray[dim] > 0 ? 0 : 1);
    VSlot &vs = lookupvslot(c.texture[orient], false);
    // base texture reflectance * this vslot's vcolor (colorscale) -> per-surface colour bleed (red/green walls etc.)
    albedo = vs.slot && vs.slot->albedo.x >= 0 ? vec(vs.slot->albedo).mul(vs.colorscale) : vec(gialbedo, gialbedo, gialbedo);
    if(!vs.slot) { emission = vec(0, 0, 0); return; }
    if(giemissivetexel && vs.slot->emissionmap)
    {
        // sample the actual glow pattern at the hit point's texture UV (so vscale/offset/rotation localise the emission)
        extern void calctexgen(VSlot &vslot, int dim, vec4 &sgen, vec4 &tgen);
        vec4 sgen, tgen;
        calctexgen(vs, dim, sgen, tgen);
        float s = sgen.x*hitpos.x + sgen.y*hitpos.y + sgen.z*hitpos.z + sgen.w,
              t = tgen.x*hitpos.x + tgen.y*hitpos.y + tgen.z*hitpos.z + tgen.w;
        s -= floor(s); t -= floor(t);   // wrap into one tile
        int px = clamp(int(s*vs.slot->emissionw), 0, vs.slot->emissionw-1),
            py = clamp(int(t*vs.slot->emissionh), 0, vs.slot->emissionh-1);
        uchar *ep = &vs.slot->emissionmap[(py*vs.slot->emissionw + px)*3];
        emission = vec(ep[0], ep[1], ep[2]).mul(giemissive);
    }
    else emission = vec(vs.slot->emission).mul(giemissive);   // per-slot average (uniform area emitter)
}

// Emissive material surfaces (lava/water/glass) act as GI area light sources. We collect their boundary
// quads once at bake start, then a bounce ray that crosses one before hitting solid geometry "sees" it
// and picks up its emitted radiance (= material colour * gilava/giwater/giglass<variant>).
struct giemitsurf
{
    int dim, a0, a1;                  // plane axis + the two in-plane axes
    float planec, lo0, hi0, lo1, hi1; // plane coord + 2D quad bounds
    vec emission;                     // emitted radiance, 0-255 scale
};
static vector<giemitsurf> giemitsurfs;

static void buildgiemitsurfs()
{
    giemitsurfs.setsize(0);
    if(!gi) return;
    loopv(valist)
    {
        vtxarray *va = valist[i];
        loopj(va->matsurfs)
        {
            materialsurface &m = va->matbuf[j];
            float emit; const bvec *col;
            switch(m.material&MATF_VOLUME)
            {
                case MAT_LAVA:  emit = getlavaemission(m.material);  col = &getlavacolor(m.material);  break;
                case MAT_WATER: emit = getwateremission(m.material); col = &getwatercolor(m.material); break;
                case MAT_GLASS: emit = getglassemission(m.material); col = &getglasscolor(m.material); break;
                default: continue;
            }
            if(emit <= 0) continue;
            giemitsurf &e = giemitsurfs.add();
            e.dim = dimension(m.orient);
            e.planec = m.o[e.dim];
            e.emission = vec(col->r, col->g, col->b).mul(emit);
            // in-plane extents follow octa.h GENFACEVERTS: X-face -> Y=rsize,Z=csize; Y-face -> X=csize,Z=rsize; Z-face -> X=rsize,Y=csize
            switch(e.dim)
            {
                case 0: e.a0 = 1; e.a1 = 2; e.lo0 = m.o.y; e.hi0 = m.o.y + m.rsize; e.lo1 = m.o.z; e.hi1 = m.o.z + m.csize; break;
                case 1: e.a0 = 0; e.a1 = 2; e.lo0 = m.o.x; e.hi0 = m.o.x + m.csize; e.lo1 = m.o.z; e.hi1 = m.o.z + m.rsize; break;
                default:e.a0 = 0; e.a1 = 1; e.lo0 = m.o.x; e.hi0 = m.o.x + m.rsize; e.lo1 = m.o.y; e.hi1 = m.o.y + m.csize; break;
            }
        }
    }
    if(giemitsurfs.length()) conoutf("GI: %d emissive material surface(s) (lava/water/glass)", giemitsurfs.length());
}

// nearest emissive material surface the ray crosses within (tol, maxdist); thread-safe (read-only list)
static bool nearestgiemit(const vec &start, const vec &ray, float tol, float maxdist, vec &outemit)
{
    bool found = false;
    float best = maxdist;
    loopv(giemitsurfs)
    {
        giemitsurf &e = giemitsurfs[i];
        float rd = ray[e.dim];
        if(fabs(rd) < 1e-6f) continue;
        float t = (e.planec - start[e.dim]) / rd;
        if(t <= tol || t >= best) continue;
        float p0 = start[e.a0] + ray[e.a0]*t, p1 = start[e.a1] + ray[e.a1]*t;
        if(p0 < e.lo0 || p0 > e.hi0 || p1 < e.lo1 || p1 > e.hi1) continue;
        best = t; outemit = e.emission; found = true;
    }
    return found;
}

// ---- Explicit emissive area-light sampling (Source/VRAD "texlight" style) ----
// Instead of hoping hemisphere rays hit an emitter (noisy, grazing cutoff), collect every emissive surface
// as a set of area samples and integrate each one's form factor + visibility at the receiver. Smooth, no
// variance. The list is built single-threaded before the workers, so it's read-only/thread-safe in the bake.
struct giemitter { vec center, normal, radiance; float area; };
static vector<giemitter> giemitters;

static void addemitter(const vec corner[4], const vec &cubecenter, const vec &radiance)
{
    vec u = vec(corner[1]).sub(corner[0]), v = vec(corner[3]).sub(corner[0]);
    vec nrm = vec().cross(u, v);
    float area = nrm.magnitude();
    if(area < 1e-3f) return;
    nrm.mul(1.0f/area);
    vec facecenter = vec(corner[0]).add(corner[1]).add(corner[2]).add(corner[3]).mul(0.25f);
    if(nrm.dot(vec(facecenter).sub(cubecenter)) < 0) nrm.neg();   // emit outward, away from the solid
    float lu = u.magnitude(), lv = v.magnitude(), cell = max(giemitcell, 1.0f);
    // subdivide by giemitcell along BOTH edges (so a long strip gets many cells along its length, not a few
    // discrete blobs); cap only the TOTAL cells per surface, preserving aspect ratio, to bound bake cost.
    int nu = max(int(ceilf(lu/cell)), 1), nv = max(int(ceilf(lv/cell)), 1);
    const int maxcells = 1024;
    if(nu*nv > maxcells) { float scl = sqrtf(float(maxcells)/(nu*nv)); nu = max(int(nu*scl), 1); nv = max(int(nv*scl), 1); }
    float cellarea = area/(nu*nv);
    loop(b, nv) loop(a, nu)
    {
        giemitter &e = giemitters.add();
        e.center = vec(corner[0]).add(vec(u).mul((a+0.5f)/nu)).add(vec(v).mul((b+0.5f)/nv));
        e.normal = nrm;
        e.radiance = radiance;
        e.area = cellarea;
    }
}

// collect emissive-textured cube faces into area emitters (mirrors generatelightmaps' visible-face walk)
static void collectemissivefaces(cube *c, const ivec &co, int size)
{
    loopi(8)
    {
        ivec o(i, co, size);
        if(c[i].children) { collectemissivefaces(c[i].children, o, size>>1); continue; }
        if(isempty(c[i])) continue;
        loopj(6)
        {
            if(c[i].texture[j] == DEFAULT_SKY) continue;
            VSlot &vs = lookupvslot(c[i].texture[j], false);
            if(!vs.slot || vs.slot->emission.x + vs.slot->emission.y + vs.slot->emission.z <= 0) continue;
            if(!visibletris(c[i], j, o, size)) continue;
            ivec v[4];
            genfaceverts(c[i], j, v);
            vec corner[4];
            loopk(4) corner[k] = vec(o).add(vec(v[k]).mul(size/8.0f));
            vec radiance = vec(vs.slot->emission).mul(vs.colorscale).mul(giemissive);
            addemitter(corner, vec(o).add(vec(size, size, size).mul(0.5f)), radiance);
        }
    }
}

static void buildgiemitters()
{
    giemitters.setsize(0);
    if(!gi || !giemitdirect) return;
    extern cube *worldroot;
    extern int worldsize;
    collectemissivefaces(worldroot, ivec(0, 0, 0), worldsize>>1);   // emissive textures
    loopv(giemitsurfs)                                              // emissive materials (lava/water/glass) as quads
    {
        giemitsurf &m = giemitsurfs[i];
        // reconstruct the quad corners from the material surface's plane + 2D bounds
        vec c0, c1, c3; int d = m.dim;
        c0[d] = c1[d] = c3[d] = m.planec;
        c0[m.a0] = m.lo0; c0[m.a1] = m.lo1;
        c1[m.a0] = m.hi0; c1[m.a1] = m.lo1;
        c3[m.a0] = m.lo0; c3[m.a1] = m.hi1;
        vec corner[4] = { c0, c1, vec(), c3 };
        corner[2][d] = m.planec; corner[2][m.a0] = m.hi0; corner[2][m.a1] = m.hi1;
        vec center((m.lo0+m.hi0)*0.5f, (m.lo1+m.hi1)*0.5f, 0); // approximate (pushed off-plane below)
        addemitter(corner, vec(corner[0]).add(corner[2]).mul(0.5f).sub(vec(m.dim==0?1:0, m.dim==1?1:0, m.dim==2?1:0)), m.emission);
    }
    if(giemitters.length()) conoutf("GI: %d emissive area-light samples", giemitters.length());
}

// explicit emissive irradiance at a receiver point/normal: sum each emitter's form factor * visibility
static vec gatheremissive(lightmapworker *w, const vec &x, const vec &n, float tol)
{
    vec sum(0, 0, 0);
    loopv(giemitters)
    {
        giemitter &e = giemitters[i];
        vec d = vec(e.center).sub(x);
        float r2 = d.squaredlen();
        if(r2 < 1e-3f) continue;
        float rinv = 1.0f/sqrtf(r2);
        vec dir = vec(d).mul(rinv);
        float cosR = dir.dot(n);
        if(cosR <= 0) continue;
        float cosE = -dir.dot(e.normal);
        if(cosE <= 0) continue;
        vec contrib = vec(e.radiance).mul(cosR*cosE*e.area/(M_PI*r2));
        if(contrib.x + contrib.y + contrib.z < 0.02f) continue;   // negligible (far/grazing): skip before the visibility ray
        float dist = sqrtf(r2), maxd = dist - 2*tol;
        if(maxd <= 0) continue;
        vec start = vec(dir).mul(tol).add(x);
        if(shadowray(w->shadowraycache, start, dir, maxd, RAY_SHADOW|RAY_POLY) < maxd) continue;   // occluded
        sum.add(contrib);
    }
    return sum;
}

// cosine-weighted hemisphere gather: HDR sky on unoccluded rays, multi-bounce indirect off geometry.
// `depth` recurses up to `gibounces` levels so light bounces multiple times (true radiosity, with
// per-surface albedo colour bleed). giscale is applied once at the top level.
static void calcgi(lightmapworker *w, const vec &o, const vec &normal, float tolerance, vec &out, int depth)
{
    out = vec(0, 0, 0);
    // match calcskylight: rays must pass THROUGH sky-textured faces (RAY_SKIPSKY) so they escape to the
    // sky and pick up sky radiance, instead of stopping on the sky face as if it were solid geometry.
    int flags = RAY_SHADOW | RAY_POLY;
    if(skytexturelight) flags |= RAY_SKIPSKY | (useskytexture ? RAY_SKYTEX : 0);
    // tangent frame around the normal for cosine-weighted hemisphere sampling
    vec n = vec(normal).normalize();
    vec tang(fabs(n.z) < 0.9f ? vec(0, 0, 1) : vec(1, 0, 0));
    tang.sub(vec(n).mul(n.dot(tang))).normalize();
    vec bitang = vec().cross(n, tang);
    // deeper bounces are low-frequency: use far fewer samples so cost stays ~O(girays), not O(girays^bounces)
    int n2 = depth <= 1 ? max(girays, 1) : max(girays >> (3*(depth-1)), 4);
    // per-lumel phase jitter decorrelates the sample pattern between lumels (turns banding into noise)
    float jitter = radicalinverse2(uint(o.x*53.0f) ^ uint(o.y*131.0f)*2654435761u ^ uint(o.z*97.0f));
    loopi(n2)
    {
        float u1 = (i + 0.5f)/n2, u2 = radicalinverse2(i) + jitter;
        u2 -= floor(u2);
        // Malley's method: cosine-weighted direction in local hemisphere (z = normal)
        float r = sqrtf(u1), phi = 2*M_PI*u2;
        vec ray = vec(tang).mul(r*cosf(phi)).add(vec(bitang).mul(r*sinf(phi))).add(vec(n).mul(sqrtf(max(0.0f, 1 - u1))));
        ray.normalize();
        vec start = vec(ray).mul(tolerance).add(o);
        float dist = w ? shadowray(w->shadowraycache, start, ray, 1e16f, flags)
                       : shadowray(start, ray, 1e16f, flags);
        vec ememit;
        if(!giemitdirect && giemitsurfs.length() && nearestgiemit(start, ray, tolerance, dist, ememit))
            out.add(ememit);                                   // (gather mode) ray sees an emissive material surface
        else if(dist > 1e15f) out.add(skyradiance(ray));       // open sky -> HDR sky IBL (cosine implicit)
        else if(w)
        {
            vec hit = vec(ray).mul(dist).add(start);
            // the hit surface's true (axis-aligned) face normal, NOT -ray: gatherdirect needs it for the
            // lambert term (sunlightdir.dot(normal)) and the recursive bounce needs it to orient its hemisphere.
            vec ar(fabs(ray.x), fabs(ray.y), fabs(ray.z));
            int hdim = ar.x >= ar.y ? (ar.x >= ar.z ? 0 : 2) : (ar.y >= ar.z ? 1 : 2);
            vec hitn(0, 0, 0);
            hitn[hdim] = ray[hdim] > 0 ? -1.0f : 1.0f;
            vec ghit = vec(hit).add(vec(hitn).mul(tolerance));  // lift off the surface to avoid self-shadow acne
            vec inc = gatherdirect(w, ghit, hitn);             // direct light reaching the hit surface
            if(depth < gibounces)                              // + indirect reaching it (further bounces)
            {
                vec ind;
                calcgi(w, hit, hitn, tolerance, ind, depth + 1);
                inc.add(ind);
            }
            vec albedo, emission;
            hitsurface(hit, ray, albedo, emission);
            inc.mul(albedo);                                   // reflected (colour bleed)
            if(!giemitdirect) inc.add(emission);               // (gather mode) emitted; explicit mode handles emission below
            out.add(inc);
        }
    }
    out.mul(1.0f/n2);                                          // cosine-weighted Monte-Carlo average
    // explicit emissive area lights (deterministic; recursion propagates it as indirect emissive too)
    if(giemitdirect && w && giemitters.length()) out.add(gatheremissive(w, o, n, tolerance));
    if(depth <= 1) out.mul(giscale);
}

// Full ambient-cube gather at `pos`: fill the 6 faces (+X,-X,+Y,-Y,+Z,-Z) with the lighting a surface facing
// each axis would receive -- ambient floor + direct lights + sun + full radiosity (sky IBL + bounce + emissive,
// via calcgi). Thread-safe: uses only the passed worker's shadow-ray cache and light list. Used to bake the
// probe grid and to light static mapmodels (Valve sampled ambient cubes from the radiosity solution).
static const vec probeaxis[6] = { vec(1, 0, 0), vec(-1, 0, 0), vec(0, 1, 0), vec(0, -1, 0), vec(0, 0, 1), vec(0, 0, -1) };

void gatherprobe(lightmapworker *w, const vec &pos, vec *faces)
{
    // ambient floor (when GI is baked the sky comes from the radiosity gather below, so don't double-count it)
    vec amb = gi ? vec(ambientcolor.tocolor()) : vec(skylightcolor.tocolor().max(ambientcolor.tocolor()));
    loopi(6) faces[i] = amb;

    loopv(w->lights)
    {
        const extentity &e = *w->lights[i];
        vec ray = vec(pos).sub(e.o);
        float mag = ray.magnitude();
        if(e.attr1 && mag >= float(e.attr1)) continue;
        vec tolight(0, 0, 1);
        if(mag >= 1e-4f)
        {
            ray.div(mag);
            if(shadowray(w->shadowraycache, e.o, ray, mag, RAY_SHADOW | RAY_POLY) < mag) continue;
            tolight = vec(ray).neg();
        }
        float intensity = e.attr1 ? 1 - mag/float(e.attr1) : 1;
        if(e.attached && e.attached->type==ET_SPOTLIGHT)
        {
            vec spot = vec(e.attached->o).sub(e.o).normalize();
            float maxatten = cosf(clamp(int(e.attached->attr1), 1, 89)*RAD),
                  spotatten = (ray.dot(spot) - maxatten) / (1 - maxatten);
            if(spotatten <= 0) continue;
            intensity *= spotatten;
        }
        vec lightcol = vec(e.attr2, e.attr3, e.attr4).mul(intensity/255.0f);
        loopj(6) { float d = probeaxis[j].dot(tolight); if(d > 0) faces[j].add(vec(lightcol).mul(d)); }
    }
    if(sunlight)
    {
        // skip sky faces when skytexturelight is on, same as the lightmap sun (else a probe by a sky opening misses it)
        int sunflags = RAY_SHADOW | RAY_POLY | (skytexturelight ? RAY_SKIPSKY | (useskytexture ? RAY_SKYTEX : 0) : 0);
        if(shadowray(w->shadowraycache, pos, sunlightdir, 1e16f, sunflags) > 1e15f)
        {
            vec suncol = vec(sunlightcolor.x, sunlightcolor.y, sunlightcolor.z).mul(sunlightscale/255.0f);
            loopj(6) { float d = probeaxis[j].dot(sunlightdir); if(d > 0) faces[j].add(vec(suncol).mul(d)); }
        }
    }
    if(gi) loopi(6)
    {
        vec g(0, 0, 0);
        calcgi(w, pos, probeaxis[i], 0.5f, g, 1);
        faces[i].add(vec(g).mul(1.0f/255));    // radiosity is on the 0-255 scale; direct terms above are 0-1
    }
}

// --- threaded probe-grid bake (each worker computes a slice; ESC cancels; main thread shows progress) ---
static SDL_mutex *probebakelock = NULL;
static const vec *probebakepos = NULL;
static vec *probebakeout = NULL;
static volatile int probebakecount = 0, probebakenext = 0, probebakedone = 0;
static volatile bool probebakecancel = false;

static int probebakeworker(void *data)
{
    lightmapworker *w = (lightmapworker *)data;
    for(;;)
    {
        SDL_LockMutex(probebakelock);
        int i = (!probebakecancel && probebakenext < probebakecount) ? probebakenext++ : -1;
        SDL_UnlockMutex(probebakelock);
        if(i < 0) break;
        gatherprobe(w, probebakepos[i], &probebakeout[i*6]);
        SDL_LockMutex(probebakelock); probebakedone++; SDL_UnlockMutex(probebakelock);
    }
    return 0;
}

// fill out[count*6] vecs from positions[count], multithreaded. returns false if the user pressed ESC.
extern int lightthreads;   // VARP defined later in this file
bool bakeprobes(const vec *positions, vec *out, int count)
{
    int nthreads = lightthreads > 0 ? lightthreads : max(numcpus + lightthreads, 1);
    nthreads = clamp(nthreads, 1, 16);
    probebakepos = positions; probebakeout = out; probebakecount = count;
    probebakenext = probebakedone = 0; probebakecancel = false;

    while(lightmapworkers.length() < nthreads) lightmapworkers.add(new lightmapworker);
    const vector<extentity *> &ents = entities::getents();
    loopi(nthreads)
    {
        lightmapworker *w = lightmapworkers[i];
        resetshadowraycache(w->shadowraycache);
        w->lights.setsize(0);
        loopvj(ents) if(ents[j]->type == ET_LIGHT) w->lights.add(ents[j]);   // all map lights, so bounce sees them
    }
    probebakelock = SDL_CreateMutex();
    vector<SDL_Thread *> threads;
    loopi(nthreads) threads.add(SDL_CreateThread(probebakeworker, "probe bake", lightmapworkers[i]));

    int done = 0;
    while(done < count)
    {
        if(interceptkey(SDLK_ESCAPE)) { SDL_LockMutex(probebakelock); probebakecancel = true; SDL_UnlockMutex(probebakelock); }
        SDL_LockMutex(probebakelock); done = probebakedone; SDL_UnlockMutex(probebakelock);
        defformatstring(text, "generating light probes... %d%% (esc to skip)", count ? int(100.0f*done/count) : 100);
        renderprogress(count ? float(done)/count : 1, text);
        if(probebakecancel) break;
        SDL_Delay(30);
    }
    loopv(threads) SDL_WaitThread(threads[i], NULL);
    SDL_DestroyMutex(probebakelock); probebakelock = NULL;
    return !probebakecancel;
}

// Static mapmodels get their own ambient cube sampled from the radiosity solution at the centre of their
// bounding box, rather than the grid probe at the model origin (which can sit in a hot sunlit floor cell and
// blow the model out). Baked at calclight time into each mapmodel entity's light.
void bakemapmodelprobes()
{
    const vector<extentity *> &ents = entities::getents();
    loopv(ents) ents[i]->light.hasprobe = false;
    if(!haslightprobes() || !gi) return;

    if(lightmapworkers.empty()) lightmapworkers.add(new lightmapworker);
    lightmapworker *w = lightmapworkers[0];
    resetshadowraycache(w->shadowraycache);
    w->lights.setsize(0);
    loopv(ents) if(ents[i]->type == ET_LIGHT) w->lights.add(ents[i]);

    loopv(ents)
    {
        extentity &e = *ents[i];
        if(e.type != ET_MAPMODEL) continue;
        model *m = loadmodel(NULL, e.attr2);
        if(!m) continue;
        vec center, radius;
        m->boundbox(center, radius);                 // bbox centre in model space
        float yaw = e.attr1*RAD, cy = cosf(yaw), sy = sinf(yaw);
        vec pos(e.o.x + center.x*cy - center.y*sy, e.o.y + center.x*sy + center.y*cy, e.o.z + center.z);
        gatherprobe(w, pos, e.light.probe);
        e.light.hasprobe = true;
    }
}

VARR(blurlms, 0, 0, 2);
VARR(blurskylight, 0, 0, 2);

static inline void generatealpha(lightmapworker *w, float tolerance, const vec &pos, uchar &alpha)
{
    alpha = lookupblendmap(w->blendmapcache, pos);
    if(w->slot->layermask)
    {
        static const int sdim[] = { 1, 0, 0 }, tdim[] = { 2, 2, 1 };
        int dim = dimension(w->orient);
        float k = 8.0f/w->vslot->scale,
              s = (pos[sdim[dim]] * k - w->vslot->offset.y) / w->slot->layermaskscale,
              t = (pos[tdim[dim]] * (dim <= 1 ? -k : k) - w->vslot->offset.y) / w->slot->layermaskscale;
        const texrotation &r = texrotations[w->rotate];
        if(r.swapxy) swap(s, t);
        if(r.flipx) s = -s;
        if(r.flipy) t = -t;
        const ImageData &mask = *w->slot->layermask;
        int mx = int(floor(s))%mask.w, my = int(floor(t))%mask.h;
        if(mx < 0) mx += mask.w;
        if(my < 0) my += mask.h;
        uchar maskval = mask.data[mask.bpp*(mx + 1) - 1 + mask.pitch*my];
        switch(w->slot->layermaskmode)
        {
            case 2: alpha = min(alpha, maskval); break;
            case 3: alpha = max(alpha, maskval); break;
            case 4: alpha = min(alpha, uchar(0xFF - maskval)); break;
            case 5: alpha = max(alpha, uchar(0xFF - maskval)); break;
            default: alpha = maskval; break;
        }
    }
}

VAR(edgetolerance, 1, 4, 64);
VAR(adaptivesample, 0, 2, 2);

enum
{
    NO_SURFACE = 0,
    SURFACE_AMBIENT_BOTTOM,
    SURFACE_AMBIENT_TOP,
    SURFACE_LIGHTMAP_BOTTOM,
    SURFACE_LIGHTMAP_TOP,
    SURFACE_LIGHTMAP_BLEND
};

#define SURFACE_AMBIENT SURFACE_AMBIENT_BOTTOM
#define SURFACE_LIGHTMAP SURFACE_LIGHTMAP_BOTTOM

static bool generatelightmap(lightmapworker *w, float lpu, const lerpvert *lv, int numv, vec origin1, const vec &xstep1, const vec &ystep1, vec origin2, const vec &xstep2, const vec &ystep2, float side0, float sidestep)
{
    static const float aacoords[8][2] =
    {
        {0.0f, 0.0f},
        {-0.5f, -0.5f},
        {0.0f, -0.5f},
        {-0.5f, 0.0f},

        {0.3f, -0.6f},
        {0.6f, 0.3f},
        {-0.3f, 0.6f},
        {-0.6f, -0.3f},
    };
    float tolerance = 0.5 / lpu;
    uint lightmask = 0, lightused = 0;
    vec offsets1[8], offsets2[8];
    loopi(8)
    {
        offsets1[i] = vec(xstep1).mul(aacoords[i][0]).add(vec(ystep1).mul(aacoords[i][1]));
        offsets2[i] = vec(xstep2).mul(aacoords[i][0]).add(vec(ystep2).mul(aacoords[i][1]));
    }
    if((w->type&LM_TYPE) == LM_BUMPMAP0) memclear(w->raydata, (LM_MAXW + 4)*(LM_MAXH + 4));
    if((w->type&LM_TYPE) == LM_RNM0) loopi(3) memclear(w->rnmdata[i], (LM_MAXW + 4)*(LM_MAXH + 4));

    // HDR lightmaps skip the blur (RGBE can't be blurred) and thus the trim, so they must also skip the
    // blur padding/origin-shift below; otherwise the surface content stays offset by blurlms texels (shear).
    if(!(w->type&LM_HDR))
    {
        origin1.sub(vec(ystep1).add(xstep1).mul(blurlms));
        origin2.sub(vec(ystep2).add(xstep2).mul(blurlms));
    }

    int aasample = min(1 << lmaa, 4);
    int stride = aasample*(w->w+1);
    vec *sample = w->colordata;
    uchar *skylight = w->ambient;
    lerpbounds start, end;
    initlerpbounds(-blurlms, -blurlms, lv, numv, start, end);
    float sidex = side0 + blurlms*sidestep;
    for(int y = 0; y < w->h; ++y, sidex += sidestep)
    {
        vec normal, nstep;
        lerpnormal(-blurlms, y - blurlms, lv, numv, start, end, normal, nstep);

        for(int x = 0; x < w->w; ++x, normal.add(nstep), skylight += w->bpp)
        {
#define EDGE_TOLERANCE(x, y) \
    (x < blurlms \
     || x+1 > w->w - blurlms \
     || y < blurlms \
     || y+1 > w->h - blurlms \
     ? edgetolerance : 1)
            float t = EDGE_TOLERANCE(x, y) * tolerance;
            vec u = x < sidex ? vec(xstep1).mul(x).add(vec(ystep1).mul(y)).add(origin1) : vec(xstep2).mul(x).add(vec(ystep2).mul(y)).add(origin2);
            lightused |= generatelumel(w, t, 0, w->lights, u, vec(normal).normalize(), *sample, x, y);
            if(hasskylight())
            {
                if((w->type&LM_TYPE)==LM_BUMPMAP0 || !adaptivesample || sample->x<skylightcolor[0] || sample->y<skylightcolor[1] || sample->z<skylightcolor[2])
                    calcskylight(w, u, normal, t, skylight, lmshadows > 1 ? RAY_ALPHAPOLY : 0);
                else loopk(3) skylight[k] = max(skylightcolor[k], ambientcolor[k]);
            }
            else loopk(3) skylight[k] = ambientcolor[k];
            // HDR sky image-based lighting / GI: cosine-weighted hemisphere gather (once per lumel).
            // sky comes from gidata, so drop the flat skylight floor to ambient to avoid double-counting.
            if(gi && (w->type&LM_HDR)) { calcgi(w, u, vec(normal).normalize(), t, w->gidata[y*w->w + x], 1); loopk(3) skylight[k] = ambientcolor[k]; }
            if(w->type&LM_ALPHA) generatealpha(w, t, u, skylight[3]);
            sample += aasample;
        }
        sample += aasample;
    }
    if(adaptivesample > 1 && min(w->w, w->h) >= 2) lightmask = ~lightused;
    sample = w->colordata;
    initlerpbounds(-blurlms, -blurlms, lv, numv, start, end);
    sidex = side0 + blurlms*sidestep;
    for(int y = 0; y < w->h; ++y, sidex += sidestep)
    {
        vec normal, nstep;
        lerpnormal(-blurlms, y - blurlms, lv, numv, start, end, normal, nstep);

        for(int x = 0; x < w->w; ++x, normal.add(nstep))
        {
            vec &center = *sample++;
            if(adaptivesample && x > 0 && x+1 < w->w && y > 0 && y+1 < w->h && !lumelsample(center, aasample, stride))
                loopi(aasample-1) *sample++ = center;
            else
            {
#define AA_EDGE_TOLERANCE(x, y, i) EDGE_TOLERANCE(x + aacoords[i][0], y + aacoords[i][1])
                vec u = x < sidex ? vec(xstep1).mul(x).add(vec(ystep1).mul(y)).add(origin1) : vec(xstep2).mul(x).add(vec(ystep2).mul(y)).add(origin2);
                const vec *offsets = x < sidex ? offsets1 : offsets2;
                vec n = vec(normal).normalize();
                loopi(aasample-1)
                    generatelumel(w, AA_EDGE_TOLERANCE(x, y, i+1) * tolerance, lightmask, w->lights, vec(u).add(offsets[i+1]), n, *sample++, x, y);
                if(lmaa == 3)
                {
                    loopi(4)
                    {
                        vec s;
                        generatelumel(w, AA_EDGE_TOLERANCE(x, y, i+4) * tolerance, lightmask, w->lights, vec(u).add(offsets[i+4]), n, s, x, y);
                        center.add(s);
                    }
                    center.div(5);
                }
            }
        }
        if(aasample > 1)
        {
            vec u = w->w < sidex ? vec(xstep1).mul(w->w).add(vec(ystep1).mul(y)).add(origin1) : vec(xstep2).mul(w->w).add(vec(ystep2).mul(y)).add(origin2);
            const vec *offsets = w->w < sidex ? offsets1 : offsets2;
            vec n = vec(normal).normalize();
            generatelumel(w, edgetolerance * tolerance, lightmask, w->lights, vec(u).add(offsets[1]), n, sample[1], w->w-1, y);
            if(aasample > 2)
                generatelumel(w, edgetolerance * tolerance, lightmask, w->lights, vec(u).add(offsets[3]), n, sample[3], w->w-1, y);
        }
        sample += aasample;
    }

    if(aasample > 1)
    {
        vec normal, nstep;
        lerpnormal(-blurlms, w->h - blurlms, lv, numv, start, end, normal, nstep);

        for(int x = 0; x <= w->w; ++x, normal.add(nstep))
        {
            vec u = x < sidex ? vec(xstep1).mul(x).add(vec(ystep1).mul(w->h)).add(origin1) : vec(xstep2).mul(x).add(vec(ystep2).mul(w->h)).add(origin2);
            const vec *offsets = x < sidex ? offsets1 : offsets2;
            vec n = vec(normal).normalize();
            generatelumel(w, edgetolerance * tolerance, lightmask, w->lights, vec(u).add(offsets[1]), n, sample[1], min(x, w->w-1), w->h-1);
            if(aasample > 2)
                generatelumel(w, edgetolerance * tolerance, lightmask, w->lights, vec(u).add(offsets[2]), n, sample[2], min(x, w->w-1), w->h-1);
            sample += aasample;
        }
    }
    return true;
}

static int finishlightmap(lightmapworker *w)
{
    if(hasskylight() && blurskylight && !(w->type&LM_HDR) && (w->w>1 || w->h>1))
    {
        blurtexture(blurskylight, w->bpp, w->w, w->h, w->blur, w->ambient);
        swap(w->blur, w->ambient);
    }
    vec *sample = w->colordata;
    int aasample = min(1 << lmaa, 4), stride = aasample*(w->w+1);
    float weight = 1.0f / (1.0f + 4.0f*lmaa),
          cweight = weight * (lmaa == 3 ? 5.0f : 1.0f);
    uchar *skylight = w->ambient;
    vec *ray = w->raydata;
    uchar *dstcolor = blurlms && !(w->type&LM_HDR) && (w->w > 1 || w->h > 1) ? w->blur : w->colorbuf;
    uchar mincolor[4] = { 255, 255, 255, 255 }, maxcolor[4] = { 0, 0, 0, 0 };
    bvec *dstray = blurlms && (w->w > 1 || w->h > 1) ? (bvec *)w->raydata : w->raybuf;
    bvec minray(255, 255, 255), maxray(0, 0, 0);
    loop(y, w->h)
    {
        loop(x, w->w)
        {
            vec l(0, 0, 0);
            const vec &center = *sample++;
            loopi(aasample-1) l.add(*sample++);
            if(aasample > 1)
            {
                l.add(sample[1]);
                if(aasample > 2) l.add(sample[3]);
            }
            vec *next = sample + stride - aasample;
            if(aasample > 1)
            {
                l.add(next[1]);
                if(aasample > 2) l.add(next[2]);
                l.add(next[aasample+1]);
            }

            if(w->type&LM_HDR)
            {
                // store unbounded radiance as RGBE (normalized to 0..1+); decoded in the world shader
                vec c(max(center.x*cweight + l.x*weight, float(skylight[0])),
                      max(center.y*cweight + l.y*weight, float(skylight[1])),
                      max(center.z*cweight + l.z*weight, float(skylight[2])));
                int gidx = y*w->w + x;
                if(gi) c.add(w->gidata[gidx]);   // add HDR sky/GI irradiance
                if((w->type&LM_TYPE) == LM_RNM0)
                {
                    // 3-basis radiosity normal map: scale the baked basis radiances so their +Z-weighted
                    // average (sum/3) reproduces the Lambert diffuse c, then add ambient to each basis
                    int idx = y*w->w + x;
                    vec amb(skylight[0], skylight[1], skylight[2]);
                    if(gi) amb.add(w->gidata[idx]);   // sky/GI fill into each basis
                    // at the geometric normal the HL2-weighted basis average is rawsum/3; pick a single
                    // (stable, clamped) scalar so that average reproduces the directional diffuse (c-amb),
                    // then add ambient. A scalar avoids per-channel division blow-ups on coloured lights.
                    vec rawsum = vec(w->rnmdata[0][idx]).add(w->rnmdata[1][idx]).add(w->rnmdata[2][idx]);
                    float rawl = rawsum.x + rawsum.y + rawsum.z,
                          dirl = max((c.x-amb.x) + (c.y-amb.y) + (c.z-amb.z), 0.0f),
                          comp = rawl > 1.0f ? min(3.0f*dirl/rawl, 3.0f) : 0.0f;
                    // cap a single basis at 2x overbright: keeps directional bounce HDR-ish while
                    // preventing pile-ups of many bright lights from blowing a normal-facing texel to white
                    float bcap = 2.0f*255.0f;
                    loopk(3)
                    {
                        vec bb = vec(w->rnmdata[k][idx]).mul(comp).add(amb).min(vec(bcap, bcap, bcap));
                        encodergbe(bb.mul(1.0f/255.0f), dstcolor + 4*k);
                    }
                }
                else encodergbe(c.mul(1.0f/255.0f), dstcolor);
                dstcolor += w->bpp;
                skylight += w->bpp;
                continue;
            }
            int r = int(center.x*cweight + l.x*weight),
                g = int(center.y*cweight + l.y*weight),
                b = int(center.z*cweight + l.z*weight),
                ar = skylight[0], ag = skylight[1], ab = skylight[2];
            dstcolor[0] = max(ar, r);
            dstcolor[1] = max(ag, g);
            dstcolor[2] = max(ab, b);
            loopk(3)
            {
                mincolor[k] = min(mincolor[k], dstcolor[k]);
                maxcolor[k] = max(maxcolor[k], dstcolor[k]);
            }
            if(w->type&LM_ALPHA)
            {
                dstcolor[3] = skylight[3];
                mincolor[3] = min(mincolor[3], dstcolor[3]);
                maxcolor[3] = max(maxcolor[3], dstcolor[3]);
            }
            if((w->type&LM_TYPE) == LM_BUMPMAP0)
            {
                if(ray->iszero()) dstray[0] = bvec(128, 128, 255);
                else
                {
                    // bias the normals towards the amount of ambient/skylight in the lumel
                    // this is necessary to prevent the light values in shaders from dropping too far below the skylight (to the ambient) if N.L is small
                    ray->normalize();
                    int l = max(r, max(g, b)), a = max(ar, max(ag, ab));
                    ray->mul(max(l-a, 0));
                    ray->z += a;
                    dstray[0] = bvec(ray->normalize());
                }
                loopk(3)
                {
                    minray[k] = min(minray[k], dstray[0][k]);
                    maxray[k] = max(maxray[k], dstray[0][k]);
                }
                ray++;
                dstray++;
            }
            dstcolor += w->bpp;
            skylight += w->bpp;
        }
        sample += aasample;
    }
    if(!(w->type&LM_HDR) &&
       int(maxcolor[0]) - int(mincolor[0]) <= lighterror &&
       int(maxcolor[1]) - int(mincolor[1]) <= lighterror &&
       int(maxcolor[2]) - int(mincolor[2]) <= lighterror &&
       mincolor[3] >= maxcolor[3])
    {
        uchar color[3];
        loopk(3) color[k] = (int(maxcolor[k]) + int(mincolor[k])) / 2;
        if(color[0] <= int(ambientcolor[0]) + lighterror &&
           color[1] <= int(ambientcolor[1]) + lighterror &&
           color[2] <= int(ambientcolor[2]) + lighterror &&
           (maxcolor[3]==0 || mincolor[3]==255))
            return mincolor[3]==255 ? SURFACE_AMBIENT_TOP : SURFACE_AMBIENT_BOTTOM;
        if((w->type&LM_TYPE) != LM_BUMPMAP0 ||
            (int(maxray.x) - int(minray.x) <= bumperror &&
             int(maxray.y) - int(minray.z) <= bumperror &&
             int(maxray.z) - int(minray.z) <= bumperror))

        {
            memcpy(w->colorbuf, color, 3);
            if(w->type&LM_ALPHA) w->colorbuf[3] = mincolor[3];
            if((w->type&LM_TYPE) == LM_BUMPMAP0)
            {
                loopk(3) w->raybuf[0][k] = uchar((int(maxray[k])+int(minray[k]))/2);
            }
            w->lastlightmap->w = w->w = 1;
            w->lastlightmap->h = w->h = 1;
        }
    }
    if(blurlms && !(w->type&LM_HDR) && (w->w>1 || w->h>1))
    {
        blurtexture(blurlms, w->bpp, w->w, w->h, w->colorbuf, w->blur, blurlms);
        if((w->type&LM_TYPE) == LM_BUMPMAP0) blurnormals(blurlms, w->w, w->h, w->raybuf, (const bvec *)w->raydata, blurlms);
        w->lastlightmap->w = (w->w -= 2*blurlms);
        w->lastlightmap->h = (w->h -= 2*blurlms);
    }
    if(mincolor[3]==255) return SURFACE_LIGHTMAP_TOP;
    else if(maxcolor[3]==0) return SURFACE_LIGHTMAP_BOTTOM;
    else return SURFACE_LIGHTMAP_BLEND;
}

static int previewlightmapalpha(lightmapworker *w, float lpu, const vec &origin1, const vec &xstep1, const vec &ystep1, const vec &origin2, const vec &xstep2, const vec &ystep2, float side0, float sidestep)
{
    extern int fullbrightlevel;
    float tolerance = 0.5 / lpu;
    uchar *dst = w->colorbuf;
    uchar minalpha = 255, maxalpha = 0;
    float sidex = side0;
    for(int y = 0; y < w->h; ++y, sidex += sidestep)
    {
        for(int x = 0; x < w->w; ++x, dst += 4)
        {
            vec u = x < sidex ?
                vec(xstep1).mul(x).add(vec(ystep1).mul(y)).add(origin1) :
                vec(xstep2).mul(x).add(vec(ystep2).mul(y)).add(origin2);
            loopk(3) dst[k] = fullbrightlevel;
            generatealpha(w, tolerance, u, dst[3]);
            minalpha = min(minalpha, dst[3]);
            maxalpha = max(maxalpha, dst[3]);
        }
    }
    if(minalpha==255) return SURFACE_AMBIENT_TOP;
    if(maxalpha==0) return SURFACE_AMBIENT_BOTTOM;
    if(minalpha==maxalpha) w->w = w->h = 1;
    if((w->type&LM_TYPE) == LM_BUMPMAP0) loopi(w->w*w->h) w->raybuf[i] = bvec(128, 128, 255);
    return SURFACE_LIGHTMAP_BLEND;
}

static void clearsurfaces(cube *c)
{
    loopi(8)
    {
        if(c[i].ext)
        {
            loopj(6)
            {
                surfaceinfo &surf = c[i].ext->surfaces[j];
                if(!surf.used()) continue;
                surf.clear();
                int numverts = surf.numverts&MAXFACEVERTS;
                if(numverts)
                {
                    if(!(c[i].merged&(1<<j))) { surf.numverts &= ~MAXFACEVERTS; continue; }

                    vertinfo *verts = c[i].ext->verts() + surf.verts;
                    loopk(numverts)
                    {
                        vertinfo &v = verts[k];
                        v.u = 0;
                        v.v = 0;
                        v.norm = 0;
                    }
                }
            }
        }
        if(c[i].children) clearsurfaces(c[i].children);
    }
}

#define LIGHTCACHESIZE 1024

static struct lightcacheentry
{
    int x, y;
    vector<int> lights;
} lightcache[LIGHTCACHESIZE];

#define LIGHTCACHEHASH(x, y) (((((x)^(y))<<5) + (((x)^(y))>>5)) & (LIGHTCACHESIZE - 1))

VARF(lightcachesize, 4, 6, 12, clearlightcache());

void clearlightcache(int id)
{
    if(id >= 0)
    {
        const extentity &light = *entities::getents()[id];
        int radius = light.attr1;
        if(radius)
        {
            for(int x = int(max(light.o.x-radius, 0.0f))>>lightcachesize, ex = int(min(light.o.x+radius, worldsize-1.0f))>>lightcachesize; x <= ex; x++)
            for(int y = int(max(light.o.y-radius, 0.0f))>>lightcachesize, ey = int(min(light.o.y+radius, worldsize-1.0f))>>lightcachesize; y <= ey; y++)
            {
                lightcacheentry &lce = lightcache[LIGHTCACHEHASH(x, y)];
                if(lce.x != x || lce.y != y) continue;
                lce.x = -1;
                lce.lights.setsize(0);
            }
            return;
        }
    }

    for(lightcacheentry *lce = lightcache; lce < &lightcache[LIGHTCACHESIZE]; lce++)
    {
        lce->x = -1;
        lce->lights.setsize(0);
    }
}

const vector<int> &checklightcache(int x, int y)
{
    x >>= lightcachesize;
    y >>= lightcachesize;
    lightcacheentry &lce = lightcache[LIGHTCACHEHASH(x, y)];
    if(lce.x == x && lce.y == y) return lce.lights;

    lce.lights.setsize(0);
    int csize = 1<<lightcachesize, cx = x<<lightcachesize, cy = y<<lightcachesize;
    const vector<extentity *> &ents = entities::getents();
    loopv(ents)
    {
        const extentity &light = *ents[i];
        switch(light.type)
        {
            case ET_LIGHT:
            {
                int radius = light.attr1;
                if(radius > 0)
                {
                    if(light.o.x + radius < cx || light.o.x - radius > cx + csize ||
                       light.o.y + radius < cy || light.o.y - radius > cy + csize)
                        continue;
                }
                break;
            }
            default: continue;
        }
        lce.lights.add(i);
    }

    lce.x = x;
    lce.y = y;
    return lce.lights;
}

static inline void addlight(lightmapworker *w, const extentity &light, int cx, int cy, int cz, int size, const vec *v, const vec *n, int numv)
{
    int radius = light.attr1;
    if(radius > 0)
    {
        if(light.o.x + radius < cx || light.o.x - radius > cx + size ||
           light.o.y + radius < cy || light.o.y - radius > cy + size ||
           light.o.z + radius < cz || light.o.z - radius > cz + size)
            return;
    }

    loopi(4)
    {
        vec p(light.o);
        p.sub(v[i]);
        float dist = p.dot(n[i]);
        if(dist >= 0 && (!radius || dist < radius))
        {
            w->lights.add(&light);
            break;
        }
    }
}

static bool findlights(lightmapworker *w, int cx, int cy, int cz, int size, const vec *v, const vec *n, int numv, const Slot &slot, const VSlot &vslot)
{
    w->lights.setsize(0);
    const vector<extentity *> &ents = entities::getents();
    static volatile bool usinglightcache = false;
    if(size <= 1<<lightcachesize && (!lightlock || !usinglightcache))
    {
        if(lightlock) { SDL_LockMutex(lightlock); usinglightcache = true; }
        const vector<int> &lights = checklightcache(cx, cy);
        loopv(lights)
        {
            const extentity &light = *ents[lights[i]];
            switch(light.type)
            {
                case ET_LIGHT: addlight(w, light, cx, cy, cz, size, v, n, numv); break;
            }
        }
        if(lightlock) { usinglightcache = false; SDL_UnlockMutex(lightlock); }
    }
    else loopv(ents)
    {
        const extentity &light = *ents[i];
        switch(light.type)
        {
            case ET_LIGHT: addlight(w, light, cx, cy, cz, size, v, n, numv); break;
        }
    }
    if(vslot.layer && (setblendmaporigin(w->blendmapcache, ivec(cx, cy, cz), size) || slot.layermask)) return true;
    // with GI on, every surface receives indirect light (sky IBL, colour bleed, emissive textures/materials),
    // so it needs a lightmap even with no direct light/sky/sun (e.g. an emissive-only scene).
    return w->lights.length() || hasskylight() || sunlight || (gi && hdrlightmaps);
}

static int packlightmaps(lightmapworker *w = NULL)
{
    int numpacked = 0;
    for(; packidx < lightmaptasks[0].length(); packidx++, numpacked++)
    {
        lightmaptask &t = lightmaptasks[0][packidx];
        if(!t.lightmaps) break;
        if(t.ext && t.c->ext != t.ext)
        {
            lightmapext &e = lightmapexts.add();
            e.c = t.c;
            e.ext = t.ext;
        }
        progress = t.progress;
        lightmapinfo *l = t.lightmaps;
        if(l == (lightmapinfo *)-1) continue;
        int space = 0;
        for(; l && l->c == t.c; l = l->next)
        {
            l->packed = true;
            space += l->bufsize;
            if(l->surface < 0 || !t.ext) continue;
            surfaceinfo &surf = t.ext->surfaces[l->surface];
            layoutinfo layout;
            packlightmap(*l, layout);
            int numverts = surf.numverts&MAXFACEVERTS;
            vertinfo *verts = t.ext->verts() + surf.verts;
            if(l->layers&LAYER_DUP)
            {
                if(l->type&LM_ALPHA) surf.lmid[0] = layout.lmid;
                else { surf.lmid[1] = layout.lmid; verts += numverts; }
            }
            else
            {
                if(l->layers&LAYER_TOP) surf.lmid[0] = layout.lmid;
                if(l->layers&LAYER_BOTTOM) surf.lmid[1] = layout.lmid;
            }
            ushort offsetx = layout.x*((USHRT_MAX+1)/LM_PACKW), offsety = layout.y*((USHRT_MAX+1)/LM_PACKH);
            loopk(numverts)
            {
                vertinfo &v = verts[k];
                v.u += offsetx;
                v.v += offsety;
            }
        }
        if(t.worker == w)
        {
            w->bufused -= space;
            w->bufstart = (w->bufstart + space)%LIGHTMAPBUFSIZE;
            w->firstlightmap = l;
            if(!l)
            {
                w->lastlightmap = NULL;
                w->bufstart = w->bufused = 0;
            }
        }
        if(t.worker->needspace) SDL_CondSignal(t.worker->spacecond);
    }
    return numpacked;
}

static lightmapinfo *alloclightmap(lightmapworker *w)
{
    int needspace1 = sizeof(lightmapinfo) + w->w*w->h*w->bpp,
        needspace2 = (w->type&LM_TYPE) == LM_BUMPMAP0 ? w->w*w->h*3 : 0,
        needspace = needspace1 + needspace2,
        bufend = (w->bufstart + w->bufused)%LIGHTMAPBUFSIZE,
        availspace = LIGHTMAPBUFSIZE - w->bufused,
        availspace1 = min(availspace, LIGHTMAPBUFSIZE - bufend),
        availspace2 = min(availspace, w->bufstart);
    if(availspace < needspace || (max(availspace1, availspace2) < needspace && (availspace1 < needspace1 || availspace2 < needspace2)))
    {
        if(tasklock) SDL_LockMutex(tasklock);
        while(!w->doneworking)
        {
            lightmapinfo *l = w->firstlightmap;
            for(; l && l->packed; l = l->next)
            {
                w->bufused -= l->bufsize;
                w->bufstart = (w->bufstart + l->bufsize)%LIGHTMAPBUFSIZE;
            }
            w->firstlightmap = l;
            if(!l)
            {
                w->lastlightmap = NULL;
                w->bufstart = w->bufused = 0;
            }
            bufend = (w->bufstart + w->bufused)%LIGHTMAPBUFSIZE;
            availspace = LIGHTMAPBUFSIZE - w->bufused;
            availspace1 = min(availspace, LIGHTMAPBUFSIZE - bufend);
            availspace2 = min(availspace, w->bufstart);
            if(availspace >= needspace && (max(availspace1, availspace2) >= needspace || (availspace1 >= needspace1 && availspace2 >= needspace2))) break;
            if(packlightmaps(w)) continue;
            if(!w->spacecond || !tasklock) break;
            w->needspace = true;
            SDL_CondWait(w->spacecond, tasklock);
            w->needspace = false;
        }
        if(tasklock) SDL_UnlockMutex(tasklock);
    }
    int usedspace = needspace;
    lightmapinfo *l = NULL;
    if(availspace1 >= needspace1)
    {
        l = (lightmapinfo *)&w->buf[bufend];
        w->colorbuf = (uchar *)(l + 1);
        if((w->type&LM_TYPE) != LM_BUMPMAP0) w->raybuf = NULL;
        else if(availspace1 >= needspace) w->raybuf = (bvec *)&w->buf[bufend + needspace1];
        else
        {
            w->raybuf = (bvec *)w->buf;
            usedspace += availspace1 - needspace1;
        }
    }
    else if(availspace2 >= needspace)
    {
        usedspace += availspace1;
        l = (lightmapinfo *)w->buf;
        w->colorbuf = (uchar *)(l + 1);
        w->raybuf = (w->type&LM_TYPE) == LM_BUMPMAP0 ? (bvec *)&w->buf[needspace1] : NULL;
    }
    else return NULL;
    w->bufused += usedspace;
    l->next = NULL;
    l->c = w->c;
    l->type = w->type;
    l->w = w->w;
    l->h = w->h;
    l->bpp = w->bpp;
    l->colorbuf = w->colorbuf;
    l->raybuf = w->raybuf;
    l->packed = false;
    l->bufsize = usedspace;
    l->surface = -1;
    l->layers = 0;
    if(!w->firstlightmap) w->firstlightmap = l;
    if(w->lastlightmap) w->lastlightmap->next = l;
    w->lastlightmap = l;
    if(!w->curlightmaps) w->curlightmaps = l;
    return l;
}

static void freelightmap(lightmapworker *w)
{
    lightmapinfo *l = w->lastlightmap;
    if(!l || l->surface >= 0) return;
    if(w->firstlightmap == w->lastlightmap)
    {
        w->firstlightmap = w->lastlightmap = w->curlightmaps = NULL;
        w->bufstart = w->bufused = 0;
    }
    else
    {
        w->bufused -= l->bufsize - sizeof(lightmapinfo);
        l->bufsize = sizeof(lightmapinfo);
        l->packed = true;
    }
    if(w->curlightmaps == l) w->curlightmaps = NULL;
}

static int setupsurface(lightmapworker *w, plane planes[2], int numplanes, const vec *p, const vec *n, int numverts, vertinfo *litverts, bool preview = false)
{
    vec u, v, t;
    vec2 c[MAXFACEVERTS];

    u = vec(p[2]).sub(p[0]).normalize();
    v.cross(planes[0], u);
    c[0] = vec2(0, 0);
    if(numplanes >= 2) t.cross(planes[1], u); else t = v;
    vec r1 = vec(p[1]).sub(p[0]);
    c[1] = vec2(r1.dot(u), min(r1.dot(v), 0.0f));
    c[2] = vec2(vec(p[2]).sub(p[0]).dot(u), 0);
    for(int i = 3; i < numverts; i++)
    {
        vec r = vec(p[i]).sub(p[0]);
        c[i] = vec2(r.dot(u), max(r.dot(t), 0.0f));
    }

    float carea = 1e16f;
    vec2 cx(0, 0), cy(0, 0), co(0, 0), cmin(0, 0), cmax(0, 0);
    loopi(numverts)
    {
        vec2 px = vec2(c[i+1 < numverts ? i+1 : 0]).sub(c[i]);
        float len = px.squaredlen();
        if(!len) continue;
        px.mul(1/sqrtf(len));
        vec2 py(-px.y, px.x), pmin(0, 0), pmax(0, 0);
        if(numplanes >= 2 && (i == 0 || i >= 3)) px.neg();
        loopj(numverts)
        {
            vec2 rj = vec2(c[j]).sub(c[i]), pj(rj.dot(px), rj.dot(py));
            pmin.x = min(pmin.x, pj.x);
            pmin.y = min(pmin.y, pj.y);
            pmax.x = max(pmax.x, pj.x);
            pmax.y = max(pmax.y, pj.y);
        }
        float area = (pmax.x-pmin.x)*(pmax.y-pmin.y);
        if(area < carea) { carea = area; cx = px; cy = py; co = c[i]; cmin = pmin; cmax = pmax; }
    }

    int scale = int(min(cmax.x - cmin.x, cmax.y - cmin.y));
    float lpu = 16.0f / float(lightlod && scale < (1 << lightlod) ? max(lightprecision / 2, 1) : lightprecision);
    int lw = clamp(int(ceil((cmax.x - cmin.x + 1)*lpu)), LM_MINW, LM_MAXW), lh = clamp(int(ceil((cmax.y - cmin.y + 1)*lpu)), LM_MINH, LM_MAXH);
    w->w = lw;
    w->h = lh;
    if(!preview && !(w->type&LM_HDR))   // HDR isn't blurred/trimmed, so it must not get the blur padding either (else shear)
    {
        w->w += 2*blurlms;
        w->h += 2*blurlms;
    }
    if(!alloclightmap(w)) return NO_SURFACE;

    vec2 cscale = vec2(cmax).sub(cmin).div(vec2(lw-1, lh-1)),
         comin = vec2(cx).mul(cmin.x).add(vec2(cy).mul(cmin.y)).add(co);
    loopi(numverts)
    {
        vec2 ri = vec2(c[i]).sub(comin);
        c[i] = vec2(ri.dot(cx)/cscale.x, ri.dot(cy)/cscale.y);
    }

    vec xstep1 = vec(v).mul(cx.y).add(vec(u).mul(cx.x)).mul(cscale.x),
        ystep1 = vec(v).mul(cy.y).add(vec(u).mul(cy.x)).mul(cscale.y),
        origin1 = vec(v).mul(comin.y).add(vec(u).mul(comin.x)).add(p[0]),
        xstep2 = xstep1, ystep2 = ystep1, origin2 = origin1;
    float side0 = LM_MAXW + 1, sidestep = 0;
    if(numplanes >= 2)
    {
        xstep2 = vec(t).mul(cx.y).add(vec(u).mul(cx.x)).mul(cscale.x);
        ystep2 = vec(t).mul(cy.y).add(vec(u).mul(cy.x)).mul(cscale.y);
        origin2 = vec(t).mul(comin.y).add(vec(u).mul(comin.x)).add(p[0]);
        if(cx.y) { side0 = comin.y/-(cx.y*cscale.x); sidestep = cy.y*cscale.y/-(cx.y*cscale.x); }
        else if(cy.y) { side0 = ceil(comin.y/-(cy.y*cscale.y))*(LM_MAXW + 1); sidestep = -(LM_MAXW + 1); if(cy.y < 0) { side0 = (LM_MAXW + 1) - side0; sidestep = -sidestep; } }
        else side0 = comin.y <= 0 ? LM_MAXW + 1 : -1;
    }

    int surftype = NO_SURFACE;
    if(preview)
    {
        surftype = previewlightmapalpha(w, lpu, origin1, xstep1, ystep1, origin2, xstep2, ystep2, side0, sidestep);
    }
    else
    {
        lerpvert lv[MAXFACEVERTS];
        int numv = numverts;
        calclerpverts(c, n, lv, numv);

        if(!generatelightmap(w, lpu, lv, numv, origin1, xstep1, ystep1, origin2, xstep2, ystep2, side0, sidestep)) return NO_SURFACE;
        surftype = finishlightmap(w);
    }
    if(surftype<SURFACE_LIGHTMAP) return surftype;

    vec2 texscale(float(USHRT_MAX+1)/LM_PACKW, float(USHRT_MAX+1)/LM_PACKH);
    if(lw != w->w) texscale.x *= float(w->w - 1) / (lw - 1);
    if(lh != w->h) texscale.y *= float(w->h - 1) / (lh - 1);
    loopk(numverts)
    {
        litverts[k].u = ushort(floor(clamp(c[k].x*texscale.x, 0.0f, float(USHRT_MAX))));
        litverts[k].v = ushort(floor(clamp(c[k].y*texscale.y, 0.0f, float(USHRT_MAX))));
    }
    return surftype;
}

static void removelmalpha(lightmapworker *w)
{
    if(!(w->type&LM_ALPHA)) return;
    for(uchar *dst = w->colorbuf, *src = w->colorbuf, *end = &src[w->w*w->h*4];
        src < end;
        dst += 3, src += 4)
    {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
    }
    w->type &= ~LM_ALPHA;
    w->bpp = 3;
    w->lastlightmap->type = w->type;
    w->lastlightmap->bpp = w->bpp;
}

static lightmapinfo *setupsurfaces(lightmapworker *w, lightmaptask &task)
{
    cube &c = *task.c;
    const ivec &co = task.o;
    int size = task.size, usefacemask = task.usefaces;

    w->curlightmaps = NULL;
    w->c = &c;

    surfaceinfo surfaces[6];
    vertinfo litverts[6*2*MAXFACEVERTS];
    int numlitverts = 0;
    memclear(surfaces);
    loopi(6)
    {
        int usefaces = usefacemask&0xF;
        usefacemask >>= 4;
        if(!usefaces)
        {
            if(!c.ext) continue;
            surfaceinfo &surf = surfaces[i];
            surf = c.ext->surfaces[i];
            int numverts = surf.totalverts();
            if(numverts)
            {
                memcpy(&litverts[numlitverts], c.ext->verts() + surf.verts, numverts*sizeof(vertinfo));
                surf.verts = numlitverts;
                numlitverts += numverts;
            }
            continue;
        }

        VSlot &vslot = lookupvslot(c.texture[i], false),
             *layer = vslot.layer && !(c.material&MAT_ALPHA) ? &lookupvslot(vslot.layer, false) : NULL;
        Shader *shader = vslot.slot->shader;
        int shadertype = shader->type;
        if(layer) shadertype |= layer->slot->shader->type;

        surfaceinfo &surf = surfaces[i];
        vertinfo *curlitverts = &litverts[numlitverts];
        int numverts = c.ext ? c.ext->surfaces[i].numverts&MAXFACEVERTS : 0;
        ivec mo(co);
        int msz = size, convex = 0;
        if(numverts)
        {
            vertinfo *verts = c.ext->verts() + c.ext->surfaces[i].verts;
            loopj(numverts) curlitverts[j].set(verts[j].getxyz());
            if(c.merged&(1<<i))
            {
                msz = 1<<calcmergedsize(i, mo, size, verts, numverts);
                mo.mask(~(msz-1));

                if(!(surf.numverts&MAXFACEVERTS))
                {
                    surf.verts = numlitverts;
                    surf.numverts |= numverts;
                    numlitverts += numverts;
                }
            }
            else if(!flataxisface(c, i)) convex = faceconvexity(verts, numverts, size);
        }
        else
        {
            ivec v[4];
            genfaceverts(c, i, v);
            if(!flataxisface(c, i)) convex = faceconvexity(v);
            int order = usefaces&4 || convex < 0 ? 1 : 0;
            ivec vo = ivec(co).mask(0xFFF).shl(3);
            curlitverts[numverts++].set(v[order].mul(size).add(vo));
            if(usefaces&1) curlitverts[numverts++].set(v[order+1].mul(size).add(vo));
            curlitverts[numverts++].set(v[order+2].mul(size).add(vo));
            if(usefaces&2) curlitverts[numverts++].set(v[(order+3)&3].mul(size).add(vo));
        }

        vec pos[MAXFACEVERTS], n[MAXFACEVERTS], po(ivec(co).mask(~0xFFF));
        loopj(numverts) pos[j] = vec(curlitverts[j].getxyz()).mul(1.0f/8).add(po);

        int smooth = vslot.slot->smooth;
        plane planes[2];
        int numplanes = 0;
        planes[numplanes++].toplane(pos[0], pos[1], pos[2]);
        if(numverts < 4 || !convex) loopk(numverts) findnormal(pos[k], smooth, planes[0], n[k]);
        else
        {
            planes[numplanes++].toplane(pos[0], pos[2], pos[3]);
            vec avg = vec(planes[0]).add(planes[1]).normalize();
            findnormal(pos[0], smooth, avg, n[0]);
            findnormal(pos[1], smooth, planes[0], n[1]);
            findnormal(pos[2], smooth, avg, n[2]);
            for(int k = 3; k < numverts; k++) findnormal(pos[k], smooth, planes[1], n[k]);
        }

        if(shadertype&(SHADER_NORMALSLMS | SHADER_ENVMAP))
        {
            loopk(numverts) curlitverts[k].norm = encodenormal(n[k]);
            if(!(surf.numverts&MAXFACEVERTS))
            {
                surf.verts = numlitverts;
                surf.numverts |= numverts;
                numlitverts += numverts;
            }
        }

        if(!findlights(w, mo.x, mo.y, mo.z, msz, pos, n, numverts, *vslot.slot, vslot))
        {
            if(surf.numverts&MAXFACEVERTS) surf.numverts |= LAYER_TOP;
            continue;
        }

        w->slot = vslot.slot;
        w->vslot = &vslot;
        w->type = shader->type&SHADER_NORMALSLMS ? LM_BUMPMAP0 : LM_DIFFUSE;
        if(layer) w->type |= LM_ALPHA;
        if(hdrlightmaps && !(w->type&LM_ALPHA))
        {
            if((w->type&LM_TYPE)==LM_BUMPMAP0) { if(rnm) w->type = LM_RNM0|LM_HDR; }  // 3-basis RNM on bumped surfaces
            else w->type |= LM_HDR;                                                   // HDR (RGBE) on flat diffuse
        }
        w->bpp = (w->type&LM_TYPE)==LM_RNM0 ? 12 : (w->type&(LM_ALPHA|LM_HDR) ? 4 : 3);
        w->orient = i;
        w->rotate = vslot.rotation;
        int surftype = setupsurface(w, planes, numplanes, pos, n, numverts, curlitverts);
        switch(surftype)
        {
            case SURFACE_LIGHTMAP_BOTTOM:
                if((shader->type^layer->slot->shader->type)&SHADER_NORMALSLMS ||
                   (shader->type&SHADER_NORMALSLMS && vslot.rotation!=layer->rotation))
                {
                    freelightmap(w);
                    break;
                }
                // fall through
            case SURFACE_LIGHTMAP_BLEND:
            case SURFACE_LIGHTMAP_TOP:
            {
                if(!(surf.numverts&MAXFACEVERTS))
                {
                    surf.verts = numlitverts;
                    surf.numverts |= numverts;
                    numlitverts += numverts;
                }

                w->lastlightmap->surface = i;
                w->lastlightmap->layers = (surftype==SURFACE_LIGHTMAP_BOTTOM ? LAYER_BOTTOM : LAYER_TOP);
                if(surftype==SURFACE_LIGHTMAP_BLEND)
                {
                    surf.numverts |= LAYER_BLEND;
                    w->lastlightmap->layers = LAYER_TOP;
                    if((shader->type^layer->slot->shader->type)&SHADER_NORMALSLMS ||
                       (shader->type&SHADER_NORMALSLMS && vslot.rotation!=layer->rotation))
                        break;
                    w->lastlightmap->layers |= LAYER_BOTTOM;
                }
                else
                {
                    if(surftype==SURFACE_LIGHTMAP_BOTTOM)
                    {
                        surf.numverts |= LAYER_BOTTOM;
                        w->lastlightmap->layers = LAYER_BOTTOM;
                    }
                    else
                    {
                        surf.numverts |= LAYER_TOP;
                        w->lastlightmap->layers = LAYER_TOP;
                    }
                    if(w->type&LM_ALPHA) removelmalpha(w);
                }
                continue;
            }

            case SURFACE_AMBIENT_BOTTOM:
                freelightmap(w);
                surf.numverts |= layer ? LAYER_BOTTOM : LAYER_TOP;
                continue;

            case SURFACE_AMBIENT_TOP:
                freelightmap(w);
                surf.numverts |= LAYER_TOP;
                continue;

            default:
                freelightmap(w);
                continue;
        }

        w->slot = layer->slot;
        w->vslot = layer;
        w->type = layer->slot->shader->type&SHADER_NORMALSLMS ? LM_BUMPMAP0 : LM_DIFFUSE;
        w->bpp = 3;
        w->rotate = layer->rotation;
        vertinfo *blendverts = surf.numverts&MAXFACEVERTS ? &curlitverts[numverts] : curlitverts;
        switch(setupsurface(w, planes, numplanes, pos, n, numverts, blendverts))
        {
            case SURFACE_LIGHTMAP_TOP:
            {
                if(!(surf.numverts&MAXFACEVERTS))
                {
                    surf.verts = numlitverts;
                    surf.numverts |= numverts;
                    numlitverts += numverts;
                }
                else if(!(surf.numverts&LAYER_DUP))
                {
                    surf.numverts |= LAYER_DUP;
                    w->lastlightmap->layers |= LAYER_DUP;
                    loopk(numverts)
                    {
                        vertinfo &src = curlitverts[k];
                        vertinfo &dst = blendverts[k];
                        dst.setxyz(src.getxyz());
                        dst.norm = src.norm;
                    }
                    numlitverts += numverts;
                }
                surf.numverts |= LAYER_BOTTOM;
                w->lastlightmap->layers |= LAYER_BOTTOM;

                w->lastlightmap->surface = i;
                break;
            }

            case SURFACE_AMBIENT_TOP:
            {
                freelightmap(w);
                surf.numverts |= LAYER_BOTTOM;
                break;
            }

            default: freelightmap(w); break;
        }
    }
    loopk(6)
    {
        surfaceinfo &surf = surfaces[k];
        if(surf.used())
        {
            cubeext *ext = c.ext && c.ext->maxverts >= numlitverts ? c.ext : growcubeext(c.ext, numlitverts);
            memcpy(ext->surfaces, surfaces, sizeof(ext->surfaces));
            memcpy(ext->verts(), litverts, numlitverts*sizeof(vertinfo));
            task.ext = ext;
            break;
        }
    }
    return w->curlightmaps ? w->curlightmaps : (lightmapinfo *)-1;
}

int lightmapworker::work(void *data)
{
    lightmapworker *w = (lightmapworker *)data;
    SDL_LockMutex(tasklock);
    while(!w->doneworking)
    {
        if(allocidx < lightmaptasks[0].length())
        {
            lightmaptask &t = lightmaptasks[0][allocidx++];
            t.worker = w;
            SDL_UnlockMutex(tasklock);
            lightmapinfo *l = setupsurfaces(w, t);
            SDL_LockMutex(tasklock);
            t.lightmaps = l;
            packlightmaps(w);
        }
        else
        {
            if(packidx >= lightmaptasks[0].length()) SDL_CondSignal(emptycond);
            SDL_CondWait(fullcond, tasklock);
        }
    }
    SDL_UnlockMutex(tasklock);
    return 0;
}

static bool processtasks(bool finish = false)
{
    if(tasklock) SDL_LockMutex(tasklock);
    while(finish || lightmaptasks[1].length())
    {
        if(packidx >= lightmaptasks[0].length())
        {
            if(lightmaptasks[1].empty()) break;
            lightmaptasks[0].setsize(0);
            lightmaptasks[0].move(lightmaptasks[1]);
            packidx = allocidx = 0;
            if(fullcond) SDL_CondBroadcast(fullcond);
        }
        else if(lightmapping > 1)
        {
            SDL_CondWaitTimeout(emptycond, tasklock, 250);
            CHECK_PROGRESS_LOCKED({ SDL_UnlockMutex(tasklock); return false; }, SDL_UnlockMutex(tasklock), SDL_LockMutex(tasklock));
        }
        else
        {
            while(allocidx < lightmaptasks[0].length())
            {
                lightmaptask &t = lightmaptasks[0][allocidx++];
                t.worker = lightmapworkers[0];
                t.lightmaps = setupsurfaces(lightmapworkers[0], t);
                packlightmaps(lightmapworkers[0]);
                CHECK_PROGRESS(return false);
            }
        }
    }
    if(tasklock) SDL_UnlockMutex(tasklock);
    return true;
}

static void generatelightmaps(cube *c, const ivec &co, int size)
{
    CHECK_PROGRESS(return);

    taskprogress++;

    loopi(8)
    {
        ivec o(i, co, size);
        if(c[i].children)
            generatelightmaps(c[i].children, o, size >> 1);
        else if(!isempty(c[i]))
        {
            if(c[i].ext)
            {
                loopj(6)
                {
                    surfaceinfo &surf = c[i].ext->surfaces[j];
                    if(surf.lmid[0] >= LMID_RESERVED || surf.lmid[1] >= LMID_RESERVED) goto nextcube;
                    surf.clear();
                }
            }
            int usefacemask = 0;
            loopj(6) if(c[i].texture[j] != DEFAULT_SKY && (!(c[i].merged&(1<<j)) || (c[i].ext && c[i].ext->surfaces[j].numverts&MAXFACEVERTS)))
            {
                usefacemask |= visibletris(c[i], j, o, size)<<(4*j);
            }
            if(usefacemask)
            {
                lightmaptask &t = lightmaptasks[1].add();
                t.o = o;
                t.size = size;
                t.usefaces = usefacemask;
                t.c = &c[i];
                t.ext = NULL;
                t.lightmaps = NULL;
                t.progress = taskprogress;
                if(lightmaptasks[1].length() >= MAXLIGHTMAPTASKS && !processtasks()) return;
            }
        }
    nextcube:;
    }
}

static bool previewblends(lightmapworker *w, cube &c, const ivec &co, int size)
{
    if(isempty(c) || c.material&MAT_ALPHA) return false;

    int usefacemask = 0;
    loopi(6) if(c.texture[i] != DEFAULT_SKY && lookupvslot(c.texture[i], false).layer)
        usefacemask |= visibletris(c, i, co, size)<<(4*i);
    if(!usefacemask) return false;

    if(!setblendmaporigin(w->blendmapcache, co, size))
    {
        if(!c.ext) return false;
        bool blends = false;
        loopi(6) if(c.ext->surfaces[i].numverts&LAYER_BOTTOM)
        {
            c.ext->surfaces[i].brighten();
            blends = true;
        }
        return blends;
    }

    w->firstlightmap = w->lastlightmap = w->curlightmaps = NULL;
    w->bufstart = w->bufused = 0;
    w->c = &c;

    surfaceinfo surfaces[6];
    vertinfo litverts[6*2*MAXFACEVERTS];
    int numlitverts = 0;
    memcpy(surfaces, c.ext ? c.ext->surfaces : brightsurfaces, sizeof(surfaces));
    loopi(6)
    {
        int usefaces = usefacemask&0xF;
        usefacemask >>= 4;
        if(!usefaces)
        {
            surfaceinfo &surf = surfaces[i];
            int numverts = surf.totalverts();
            if(numverts)
            {
                memcpy(&litverts[numlitverts], c.ext->verts() + surf.verts, numverts*sizeof(vertinfo));
                surf.verts = numlitverts;
                numlitverts += numverts;
            }
            continue;
        }

        VSlot &vslot = lookupvslot(c.texture[i], false),
              &layer = lookupvslot(vslot.layer, false);
        Shader *shader = vslot.slot->shader;
        int shadertype = shader->type | layer.slot->shader->type;

        vertinfo *curlitverts = &litverts[numlitverts];
        int numverts = 0;
        ivec v[4];
        genfaceverts(c, i, v);
        int convex = flataxisface(c, i) ? 0 : faceconvexity(v),
            order = usefaces&4 || convex < 0 ? 1 : 0;
        ivec vo = ivec(co).mask(0xFFF).shl(3);
        curlitverts[numverts++].set(v[order].mul(size).add(vo));
        if(usefaces&1) curlitverts[numverts++].set(v[order+1].mul(size).add(vo));
        curlitverts[numverts++].set(v[order+2].mul(size).add(vo));
        if(usefaces&2) curlitverts[numverts++].set(v[(order+3)&3].mul(size).add(vo));

        vec pos[4], n[4], po(ivec(co).mask(~0xFFF));
        loopj(numverts) pos[j] = vec(curlitverts[j].getxyz()).mul(1.0f/8).add(po);

        plane planes[2];
        int numplanes = 0;
        planes[numplanes++].toplane(pos[0], pos[1], pos[2]);
        if(numverts < 4 || !convex) loopk(numverts) n[k] = planes[0];
        else
        {
            planes[numplanes++].toplane(pos[0], pos[2], pos[3]);
            vec avg = vec(planes[0]).add(planes[1]).normalize();
            n[0] = avg;
            n[1] = planes[0];
            n[2] = avg;
            for(int k = 3; k < numverts; k++) n[k] = planes[1];
        }

        surfaceinfo &surf = surfaces[i];
        w->slot = vslot.slot;
        w->vslot = &vslot;
        w->type = shadertype&SHADER_NORMALSLMS ? LM_BUMPMAP0|LM_ALPHA : LM_DIFFUSE|LM_ALPHA;
        w->bpp = 4;
        w->orient = i;
        w->rotate = vslot.rotation;
        int surftype = setupsurface(w, planes, numplanes, pos, n, numverts, curlitverts, true);
        switch(surftype)
        {
            case SURFACE_AMBIENT_TOP:
                surf = brightsurface;
                continue;

            case SURFACE_AMBIENT_BOTTOM:
                surf = brightbottomsurface;
                continue;

            case SURFACE_LIGHTMAP_BLEND:
            {
                if(surf.numverts == (LAYER_BLEND|numverts) &&
                   surf.lmid[0] == surf.lmid[1] &&
                   (surf.numverts&MAXFACEVERTS) == numverts &&
                   !memcmp(curlitverts, c.ext->verts() + surf.verts, numverts*sizeof(vertinfo)) &&
                   lightmaps.inrange(surf.lmid[0]-LMID_RESERVED) &&
                   lightmaps[surf.lmid[0]-LMID_RESERVED].type==w->type)
                {
                    vertinfo *oldverts = c.ext->verts() + surf.verts;
                    layoutinfo layout;
                    layout.w = w->w;
                    layout.h = w->h;
                    layout.x = (oldverts[0].x - curlitverts[0].x)/((USHRT_MAX+1)/LM_PACKW);
                    layout.y = (oldverts[0].y - curlitverts[0].y)/((USHRT_MAX+1)/LM_PACKH);
                    if(LM_PACKW - layout.x >= w->w && LM_PACKH - layout.y >= w->h)
                    {
                        layout.lmid = surf.lmid[0];
                        copylightmap(*w->lastlightmap, layout);
                        updatelightmap(layout);
                        surf.verts = numlitverts;
                        numlitverts += numverts;
                        continue;
                    }
                }

                surf.verts = numlitverts;
                surf.numverts = LAYER_BLEND|numverts;
                numlitverts += numverts;
                layoutinfo layout;
                if(packlightmap(*w->lastlightmap, layout)) updatelightmap(layout);
                surf.lmid[0] = surf.lmid[1] = layout.lmid;
                ushort offsetx = layout.x*((USHRT_MAX+1)/LM_PACKW), offsety = layout.y*((USHRT_MAX+1)/LM_PACKH);
                loopk(numverts)
                {
                    vertinfo &v = curlitverts[k];
                    v.u += offsetx;
                    v.v += offsety;
                }
                continue;
            }
        }
    }

    setsurfaces(c, surfaces, litverts, numlitverts);
    return true;
}

static bool previewblends(lightmapworker *w, cube *c, const ivec &co, int size, const ivec &bo, const ivec &bs)
{
    bool changed = false;
    loopoctabox(co, size, bo, bs)
    {
        ivec o(i, co, size);
        cubeext *ext = c[i].ext;
        if(ext && ext->va && ext->va->hasmerges)
        {
            changed = true;
            destroyva(ext->va);
            ext->va = NULL;
            invalidatemerges(c[i], co, size, true);
        }
        if(c[i].children ? previewblends(w, c[i].children, o, size/2, bo, bs) : previewblends(w, c[i], o, size))
        {
            changed = true;
            ext = c[i].ext;
            if(ext && ext->va)
            {
                destroyva(ext->va);
                ext->va = NULL;
            }
        }
    }
    return changed;
}

void previewblends(const ivec &bo, const ivec &bs)
{
    loadlayermasks();
    if(lightmapworkers.empty()) lightmapworkers.add(new lightmapworker);
    lightmapworkers[0]->reset();
    if(previewblends(lightmapworkers[0], worldroot, ivec(0, 0, 0), worldsize/2, bo, bs))
        commitchanges(true);
}

void cleanuplightmaps()
{
    loopv(lightmaps)
    {
        LightMap &lm = lightmaps[i];
        lm.tex = lm.offsetx = lm.offsety = -1;
    }
    loopv(lightmaptexs) glDeleteTextures(1, &lightmaptexs[i].id);
    lightmaptexs.shrink(0);
    if(progresstex) { glDeleteTextures(1, &progresstex); progresstex = 0; }
}

void resetlightmaps(bool fullclean)
{
    cleanuplightmaps();
    lightmaps.shrink(0);
    compressed.clear();
    clearlightcache();
    if(fullclean)
    {
        while(lightmapworkers.length()) delete lightmapworkers.pop();
        resetsmoothgroups();
    }
}

lightmapworker::lightmapworker()
{
    buf = new uchar[LIGHTMAPBUFSIZE];
    bufstart = bufused = 0;
    firstlightmap = lastlightmap = curlightmaps = NULL;
    ambient = new uchar[4*(LM_MAXW + 4)*(LM_MAXH + 4)];
    blur = new uchar[4*(LM_MAXW + 4)*(LM_MAXH + 4)];
    colordata = new vec[4*(LM_MAXW+1 + 4)*(LM_MAXH+1 + 4)];
    raydata = new vec[(LM_MAXW + 4)*(LM_MAXH + 4)];
    loopi(3) rnmdata[i] = new vec[(LM_MAXW + 4)*(LM_MAXH + 4)];
    gidata = new vec[(LM_MAXW + 4)*(LM_MAXH + 4)];
    shadowraycache = newshadowraycache();
    blendmapcache = newblendmapcache();
    needspace = doneworking = false;
    spacecond = NULL;
    thread = NULL;
}

lightmapworker::~lightmapworker()
{
    cleanupthread();
    delete[] buf;
    delete[] ambient;
    delete[] blur;
    delete[] colordata;
    delete[] raydata;
    loopi(3) delete[] rnmdata[i];
    delete[] gidata;
    freeshadowraycache(shadowraycache);
    freeblendmapcache(blendmapcache);
}

void lightmapworker::cleanupthread()
{
    if(spacecond) { SDL_DestroyCond(spacecond); spacecond = NULL; }
    thread = NULL;
}

void lightmapworker::reset()
{
    bufstart = bufused = 0;
    firstlightmap = lastlightmap = curlightmaps = NULL;
    needspace = doneworking = false;
    resetshadowraycache(shadowraycache);
}

bool lightmapworker::setupthread()
{
    if(!spacecond) spacecond = SDL_CreateCond();
    if(!spacecond) return false;
    thread = SDL_CreateThread(work, "lightmap worker", this);
    return thread!=NULL;
}

static Uint32 calclighttimer(Uint32 interval, void *param)
{
    check_calclight_progress = true;
    return interval;
}

bool setlightmapquality(int quality)
{
    switch(quality)
    {
        case  1: lmshadows = 2; lmaa = 3; lerptjoints = 1; break;
        case  0: lmshadows = lmshadows_; lmaa = lmaa_; lerptjoints = lerptjoints_; break;
        case -1: lmshadows = 1; lmaa = 0; lerptjoints = 0; break;
        default: return false;
    }
    return true;
}

VARP(lightthreads, -1, 0, 256);

#define ALLOCLOCK(name, init) { if(lightmapping > 1) name = init(); if(!name) lightmapping = 1; }
#define FREELOCK(name, destroy) { if(name) { destroy(name); name = NULL; } }

static void cleanuplocks()
{
    FREELOCK(lightlock, SDL_DestroyMutex);
    FREELOCK(tasklock, SDL_DestroyMutex);
    FREELOCK(fullcond, SDL_DestroyCond);
    FREELOCK(emptycond, SDL_DestroyCond);
}

static void setupthreads(int numthreads)
{
    loopi(2) lightmaptasks[i].setsize(0);
    lightmapexts.setsize(0);
    packidx = allocidx = 0;
    lightmapping = numthreads;
    if(lightmapping > 1)
    {
        ALLOCLOCK(lightlock, SDL_CreateMutex);
        ALLOCLOCK(tasklock, SDL_CreateMutex);
        ALLOCLOCK(fullcond, SDL_CreateCond);
        ALLOCLOCK(emptycond, SDL_CreateCond);
    }
    while(lightmapworkers.length() < lightmapping) lightmapworkers.add(new lightmapworker);
    loopi(lightmapping)
    {
        lightmapworker *w = lightmapworkers[i];
        w->reset();
        if(lightmapping <= 1 || w->setupthread()) continue;
        w->cleanupthread();
        lightmapping = i >= 1 ? max(i, 2) : 1;
        break;
    }
    if(lightmapping <= 1) cleanuplocks();
}

static void cleanupthreads()
{
    processtasks(true);
    if(lightmapping > 1)
    {
        SDL_LockMutex(tasklock);
        loopv(lightmapworkers) lightmapworkers[i]->doneworking = true;
        SDL_CondBroadcast(fullcond);
        loopv(lightmapworkers)
        {
            lightmapworker *w = lightmapworkers[i];
            if(w->needspace && w->spacecond) SDL_CondSignal(w->spacecond);
        }
        SDL_UnlockMutex(tasklock);
        loopv(lightmapworkers)
        {
            lightmapworker *w = lightmapworkers[i];
            if(w->thread) SDL_WaitThread(w->thread, NULL);
        }
    }
    loopv(lightmapexts)
    {
        lightmapext &e = lightmapexts[i];
        setcubeext(*e.c, e.ext);
    }
    loopv(lightmapworkers) lightmapworkers[i]->cleanupthread();
    cleanuplocks();
    lightmapping = 0;
}

void calclight(int *quality)
{
    if(!setlightmapquality(*quality))
    {
        conoutf(CON_ERROR, "valid range for calclight quality is -1..1");
        return;
    }
    renderbackground("computing lightmaps... (esc to abort)");
    mpremip(true);
    optimizeblendmap();
    loadlayermasks();
    int numthreads = lightthreads > 0 ? lightthreads : max(numcpus + lightthreads, 1);
    if(numthreads > 1) preloadusedmapmodels(false, true);
    resetlightmaps(false);
    clearsurfaces(worldroot);
    taskprogress = progress = 0;
    progresstexticks = 0;
    progresslightmap = -1;
    calclight_canceled = false;
    check_calclight_progress = false;
    SDL_TimerID timer = SDL_AddTimer(250, calclighttimer, NULL);
    Uint32 start = SDL_GetTicks();
    calcnormals(lerptjoints > 0);
    if(gi)   // precompute per-slot albedo (single-threaded) so the GI bounce can read it from workers
    {
        extern vector<Slot *> slots;
        extern void computeslotlighting(Slot &slot);
        loopv(slots) computeslotlighting(*slots[i]);   // per-slot albedo + emission (cache-independent, always reliable)
        buildgiemitsurfs();                // collect emissive lava/water/glass surfaces (gather mode)
        buildgiemitters();                 // collect emissive textures + materials as explicit area lights (giemitdirect)
        extern void setupatmospherelight();
        if(atmo) setupatmospherelight();   // cache Nishita atmosphere params for sky sampling
        if(giskybox && !atmo)              // skybox as light source: load its pixels + drive the direct sun from its brightest point (atmo overrides it)
        {
            extern void loadskyboxlight();
            extern bool setsunfromskybox(bool verbose);
            loadskyboxlight();
            setsunfromskybox(false);
        }
    }
    show_calclight_progress();
    setupthreads(numthreads);
    generatelightmaps(worldroot, ivec(0, 0, 0), worldsize >> 1);
    cleanupthreads();
    clearnormals();
    Uint32 end = SDL_GetTicks();
    if(timer) SDL_RemoveTimer(timer);
    uint total = 0, lumels = 0;
    loopv(lightmaps)
    {
        insertunlit(i);
        if(!editmode) lightmaps[i].finalize();
        total += lightmaps[i].lightmaps;
        lumels += lightmaps[i].lumels;
    }
    if(!editmode) compressed.clear();
    initlights();
    // bake the ambient-cube probe grid only when opted in (genlightprobes(false) is a no-op if lightprobes is off)
    if(!calclight_canceled) { renderbackground("generating light probes..."); genlightprobes(false); bakemapmodelprobes(); }
    renderbackground("lighting done...");
    allchanged();
    if(calclight_canceled)
        conoutf("calclight aborted");
    else
        conoutf("generated %d lightmaps using %d%% of %d textures (%.1f seconds)",
            total,
            lightmaps.length() ? lumels * 100 / (lightmaps.length() * LM_PACKW * LM_PACKH) : 0,
            lightmaps.length(),
            (end - start) / 1000.0f);
}

COMMAND(calclight, "i");

VAR(patchnormals, 0, 0, 1);

void patchlight(int *quality)
{
    if(noedit(true)) return;
    if(!setlightmapquality(*quality))
    {
        conoutf(CON_ERROR, "valid range for patchlight quality is -1..1");
        return;
    }
    renderbackground("patching lightmaps... (esc to abort)");
    loadlayermasks();
    int numthreads = lightthreads > 0 ? lightthreads : max(numcpus + lightthreads, 1);
    if(numthreads > 1) preloadusedmapmodels(false, true);
    cleanuplightmaps();
    taskprogress = progress = 0;
    progresstexticks = 0;
    progresslightmap = -1;
    int total = 0, lumels = 0;
    loopv(lightmaps)
    {
        if((lightmaps[i].type&LM_TYPE) != LM_BUMPMAP1) progresslightmap = i;
        total -= lightmaps[i].lightmaps;
        lumels -= lightmaps[i].lumels;
    }
    calclight_canceled = false;
    check_calclight_progress = false;
    SDL_TimerID timer = SDL_AddTimer(250, calclighttimer, NULL);
    if(patchnormals) renderprogress(0, "computing normals...");
    Uint32 start = SDL_GetTicks();
    if(patchnormals) calcnormals(lerptjoints > 0);
    show_calclight_progress();
    setupthreads(numthreads);
    generatelightmaps(worldroot, ivec(0, 0, 0), worldsize >> 1);
    cleanupthreads();
    if(patchnormals) clearnormals();
    Uint32 end = SDL_GetTicks();
    if(timer) SDL_RemoveTimer(timer);
    loopv(lightmaps)
    {
        total += lightmaps[i].lightmaps;
        lumels += lightmaps[i].lumels;
    }
    initlights();
    renderbackground("lighting done...");
    allchanged();
    if(calclight_canceled)
        conoutf("patchlight aborted");
    else
        conoutf("patched %d lightmaps using %d%% of %d textures (%.1f seconds)",
            total,
            lightmaps.length() ? lumels * 100 / (lightmaps.length() * LM_PACKW * LM_PACKH) : 0,
            lightmaps.length(),
            (end - start) / 1000.0f);
}

COMMAND(patchlight, "i");

void clearlightmaps()
{
    if(noedit(true)) return;
    renderprogress(0, "clearing lightmaps...");
    resetlightmaps(false);
    clearsurfaces(worldroot);
    initlights();
    allchanged();
}

COMMAND(clearlightmaps, "");

void setfullbrightlevel(int fullbrightlevel)
{
    if(lightmaptexs.length() > LMID_BRIGHT)
    {
        uchar bright[3] = { uchar(fullbrightlevel), uchar(fullbrightlevel), uchar(fullbrightlevel) };
        createtexture(lightmaptexs[LMID_BRIGHT].id, 1, 1, bright, 0, 1);
    }
    initlights();
}

VARF(fullbright, 0, 0, 1, if(lightmaptexs.length()) { initlights(); lightents(); });
VARF(fullbrightlevel, 0, 128, 255, setfullbrightlevel(fullbrightlevel));

vector<LightMapTexture> lightmaptexs;

// debug: switch lightmap atlas filtering to nearest-neighbour so the raw lumel grid is visible
// (reveals exact alignment + any per-lumel GI noise). Re-applied after each (re)bake via initlights.
void setlightmapfilter();
VARFP(lmnearest, 0, 0, 1, setlightmapfilter());
void setlightmapfilter()
{
    GLint f = lmnearest ? GL_NEAREST : GL_LINEAR;
    loopv(lightmaptexs)
    {
        if(!lightmaptexs[i].id) continue;
        glBindTexture(GL_TEXTURE_2D, lightmaptexs[i].id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    }
}

static void rotatenormals(LightMap &lmlv, int x, int y, int w, int h, bool flipx, bool flipy, bool swapxy)
{
    uchar *lv = lmlv.data + 3*(y*LM_PACKW + x);
    int stride = 3*(LM_PACKW-w);
    loopi(h)
    {
        loopj(w)
        {
            if(flipx) lv[0] = 255 - lv[0];
            if(flipy) lv[1] = 255 - lv[1];
            if(swapxy) swap(lv[0], lv[1]);
            lv += 3;
        }
        lv += stride;
    }
}

static void rotatenormals(cube *c)
{
    loopi(8)
    {
        cube &ch = c[i];
        if(ch.children)
        {
            rotatenormals(ch.children);
            continue;
        }
        else if(!ch.ext) continue;
        loopj(6) if(lightmaps.inrange(ch.ext->surfaces[j].lmid[0]+1-LMID_RESERVED))
        {
            VSlot &vslot = lookupvslot(ch.texture[j], false);
            if(!vslot.rotation) continue;
            surfaceinfo &surface = ch.ext->surfaces[j];
            int numverts = surface.numverts&MAXFACEVERTS;
            if(!numverts) continue;
            LightMap &lmlv = lightmaps[surface.lmid[0]+1-LMID_RESERVED];
            if((lmlv.type&LM_TYPE)!=LM_BUMPMAP1) continue;
            ushort x1 = USHRT_MAX, y1 = USHRT_MAX, x2 = 0, y2 = 0;
            vertinfo *verts = ch.ext->verts() + surface.verts;
            loopk(numverts)
            {
                vertinfo &v = verts[k];
                x1 = min(x1, v.u);
                y1 = min(y1, v.u);
                x2 = max(x2, v.u);
                y2 = max(y2, v.v);
            }
            if(x1 > x2 || y1 > y2) continue;
            x1 /= (USHRT_MAX+1)/LM_PACKW;
            y1 /= (USHRT_MAX+1)/LM_PACKH;
            x2 /= (USHRT_MAX+1)/LM_PACKW;
            y2 /= (USHRT_MAX+1)/LM_PACKH;
            const texrotation &r = texrotations[vslot.rotation < 4 ? 4-vslot.rotation : vslot.rotation];
            rotatenormals(lmlv, x1, y1, x2-x1, y1-y1, r.flipx, r.flipy, r.swapxy);
        }
    }
}

void fixlightmapnormals()
{
    rotatenormals(worldroot);
}

void fixrotatedlightmaps(cube &c, const ivec &co, int size)
{
    if(c.children)
    {
        loopi(8) fixrotatedlightmaps(c.children[i], ivec(i, co, size>>1), size>>1);
        return;
    }
    if(!c.ext) return;
    loopi(6)
    {
        if(c.merged&(1<<i)) continue;
        surfaceinfo &surf = c.ext->surfaces[i];
        int numverts = surf.numverts&MAXFACEVERTS;
        if(numverts!=4 || (surf.lmid[0] < LMID_RESERVED && surf.lmid[1] < LMID_RESERVED)) continue;
        vertinfo *verts = c.ext->verts() + surf.verts;
        int vis = visibletris(c, i, co, size);
        if(!vis || vis==3) continue;
        if((verts[0].u != verts[1].u || verts[0].v != verts[1].v) &&
           (verts[0].u != verts[3].u || verts[0].v != verts[3].v) &&
           (verts[2].u != verts[1].u || verts[2].v != verts[1].v) &&
           (verts[2].u != verts[3].u || verts[2].v != verts[3].v))
            continue;
        if(vis&4)
        {
            vertinfo tmp = verts[0];
            verts[0].x = verts[1].x; verts[0].y = verts[1].y; verts[0].z = verts[1].z;
            verts[1].x = verts[2].x; verts[1].y = verts[2].y; verts[1].z = verts[2].z;
            verts[2].x = verts[3].x; verts[2].y = verts[3].y; verts[2].z = verts[3].z;
            verts[3].x = tmp.x; verts[3].y = tmp.y; verts[3].z = tmp.z;
            if(surf.numverts&LAYER_DUP) loopk(4)
            {
                vertinfo &v = verts[k], &b = verts[k+4];
                b.x = v.x;
                b.y = v.y;
                b.z = v.z;
            }
        }
        surf.numverts = (surf.numverts & ~MAXFACEVERTS) | 3;
        if(vis&2)
        {
            verts[1] = verts[2]; verts[2] = verts[3];
            if(surf.numverts&LAYER_DUP) { verts[3] = verts[4]; verts[4] = verts[6]; verts[5] = verts[7]; }
        }
        else if(surf.numverts&LAYER_DUP) { verts[3] = verts[4]; verts[4] = verts[5]; verts[5] = verts[6]; }
    }
}

void fixrotatedlightmaps()
{
    loopi(8) fixrotatedlightmaps(worldroot[i], ivec(i, ivec(0, 0, 0), worldsize>>1), worldsize>>1);
}

static void copylightmap(LightMap &lm, uchar *dst, size_t stride)
{
    const uchar *c = lm.data;
    loopi(LM_PACKH)
    {
        memcpy(dst, c, lm.bpp*LM_PACKW);
        c += lm.bpp*LM_PACKW;
        dst += stride;
    }
}

void genreservedlightmaptexs()
{
    while(lightmaptexs.length() < LMID_RESERVED)
    {
        LightMapTexture &tex = lightmaptexs.add();
        tex.type = lightmaptexs.length()&1 ? LM_DIFFUSE : LM_BUMPMAP1;
        glGenTextures(1, &tex.id);
    }
    uchar unlit[3] = { ambientcolor[0], ambientcolor[1], ambientcolor[2] };
    createtexture(lightmaptexs[LMID_AMBIENT].id, 1, 1, unlit, 0, 1);
    bvec front(128, 128, 255);
    createtexture(lightmaptexs[LMID_AMBIENT1].id, 1, 1, &front, 0, 1);
    uchar bright[3] = { uchar(fullbrightlevel), uchar(fullbrightlevel), uchar(fullbrightlevel) };
    createtexture(lightmaptexs[LMID_BRIGHT].id, 1, 1, bright, 0, 1);
    createtexture(lightmaptexs[LMID_BRIGHT1].id, 1, 1, &front, 0, 1);
    uchar dark[3] = { 0, 0, 0 };
    createtexture(lightmaptexs[LMID_DARK].id, 1, 1, dark, 0, 1);
    createtexture(lightmaptexs[LMID_DARK1].id, 1, 1, &front, 0, 1);
}

static void findunlit(int i)
{
    LightMap &lm = lightmaps[i];
    if(lm.unlitx>=0) return;
    else if((lm.type&LM_TYPE)==LM_BUMPMAP0)
    {
        if(i+1>=lightmaps.length() || (lightmaps[i+1].type&LM_TYPE)!=LM_BUMPMAP1) return;
    }
    else if((lm.type&LM_TYPE)!=LM_DIFFUSE) return;
    uchar *data = lm.data;
    loop(y, 2) loop(x, LM_PACKW)
    {
        if(!data[0] && !data[1] && !data[2])
        {
            memcpy(data, ambientcolor.v, 3);
            if((lm.type&LM_TYPE)==LM_BUMPMAP0) ((bvec *)lightmaps[i+1].data)[y*LM_PACKW + x] = bvec(128, 128, 255);
            lm.unlitx = x;
            lm.unlity = y;
            return;
        }
        if(data[0]==ambientcolor[0] && data[1]==ambientcolor[1] && data[2]==ambientcolor[2])
        {
            if((lm.type&LM_TYPE)!=LM_BUMPMAP0 || ((bvec *)lightmaps[i+1].data)[y*LM_PACKW + x] == bvec(128, 128, 255))
            {
                lm.unlitx = x;
                lm.unlity = y;
                return;
            }
        }
        data += lm.bpp;
    }
}

VARF(roundlightmaptex, 0, 4, 16, { cleanuplightmaps(); initlights(); allchanged(); });
VARF(batchlightmaps, 0, 4, 256, { cleanuplightmaps(); initlights(); allchanged(); });

bool lightmapshdr = false;   // true if any loaded lightmap is RGBE/HDR (drives the world-shader decode)
bool lightmapsrnm = false;   // true if any loaded lightmap is 3-basis RNM (drives bump-slot shader swap)

void genlightmaptexs(int flagmask, int flagval)
{
    if(lightmaptexs.length() < LMID_RESERVED) genreservedlightmaptexs();

    int remaining[LM_TYPE+1] = { 0 }, total = 0;
    loopv(lightmaps)
    {
        LightMap &lm = lightmaps[i];
        if(lm.tex >= 0 || (lm.type&flagmask)!=flagval) continue;
        int type = lm.type&LM_TYPE;
        remaining[type]++;
        total++;
        if(lm.unlitx < 0) findunlit(i);
    }

    int sizelimit = (maxtexsize ? min(maxtexsize, hwtexsize) : hwtexsize)/max(LM_PACKW, LM_PACKH);
    sizelimit = min(batchlightmaps, sizelimit*sizelimit);
    while(total)
    {
        int type = LM_DIFFUSE;
        LightMap *firstlm = NULL;
        loopv(lightmaps)
        {
            LightMap &lm = lightmaps[i];
            if(lm.tex >= 0 || (lm.type&flagmask) != flagval) continue;
            type = lm.type&LM_TYPE;
            firstlm = &lm;
            break;
        }
        if(!firstlm) break;
        if(type == LM_RNM0)
        {
            // RNM lightmaps are bpp=12 (3 RGBE basis maps): deinterleave each into 3 consecutive RGBA8
            // basis textures so the surface's lmid maps to lmid/+1/+2 (basis 0/1/2)
            loopv(lightmaps)
            {
                LightMap &lm = lightmaps[i];
                if(lm.tex >= 0 || (lm.type&flagmask)!=flagval || (lm.type&LM_TYPE)!=LM_RNM0) continue;
                lm.tex = lightmaptexs.length();
                lm.offsetx = lm.offsety = 0;
                lightmapshdr = true;
                lightmapsrnm = true;
                // decode each basis from RGBE to linear and upload as fp16, so bilinear filtering is
                // correct (interpolating RGBE bytes would spike on exponent changes between texels)
                float *basis = new float[3*LM_PACKW*LM_PACKH];
                loopk(3)
                {
                    for(int t = 0; t < LM_PACKW*LM_PACKH; t++)
                    {
                        vec c = decodergbe(&lm.data[t*12 + k*4]);
                        basis[t*3] = c.x; basis[t*3+1] = c.y; basis[t*3+2] = c.z;
                    }
                    LightMapTexture &tex = lightmaptexs.add();
                    tex.type = LM_RNM0|LM_HDR;
                    tex.w = LM_PACKW;
                    tex.h = LM_PACKH;
                    glGenTextures(1, &tex.id);
                    createtexture(tex.id, LM_PACKW, LM_PACKH, basis, 3, 1, GL_RGB16F, GL_TEXTURE_2D, LM_PACKW, LM_PACKH, 0, true, GL_RGB);
                }
                delete[] basis;
                remaining[LM_RNM0]--;
                total--;
            }
            continue;
        }
        int used = 0, uselimit = min(remaining[type], sizelimit);
        do used++; while((1<<used) <= uselimit);
        used--;
        int oldval = remaining[type];
        remaining[type] -= 1<<used;
        if(remaining[type] && (2<<used) <= min(roundlightmaptex, sizelimit))
        {
            remaining[type] -= min(remaining[type], 1<<used);
            used++;
        }
        total -= oldval - remaining[type];
        LightMapTexture &tex = lightmaptexs.add();
        tex.type = firstlm->type;
        if(firstlm->type&LM_HDR) lightmapshdr = true;
        tex.w = LM_PACKW<<((used+1)/2);
        tex.h = LM_PACKH<<(used/2);
        int bpp = firstlm->bpp;
        uchar *data = used ? new uchar[bpp*tex.w*tex.h] : NULL;
        int offsetx = 0, offsety = 0;
        loopv(lightmaps)
        {
            LightMap &lm = lightmaps[i];
            if(lm.tex >= 0 || (lm.type&flagmask) != flagval || (lm.type&LM_TYPE) != type) continue;

            lm.tex = lightmaptexs.length()-1;
            lm.offsetx = offsetx;
            lm.offsety = offsety;
            if(tex.unlitx < 0 && lm.unlitx >= 0)
            {
                tex.unlitx = offsetx + lm.unlitx;
                tex.unlity = offsety + lm.unlity;
            }

            if(data) copylightmap(lm, &data[bpp*(offsety*tex.w + offsetx)], bpp*tex.w);

            offsetx += LM_PACKW;
            if(offsetx >= tex.w) { offsetx = 0; offsety += LM_PACKH; }
            if(offsety >= tex.h) break;
        }

        glGenTextures(1, &tex.id);
        uchar *src = data ? data : firstlm->data;
        if(firstlm->type&LM_HDR)
        {
            // decode RGBE -> linear and upload fp16, so bilinear filtering is correct (interpolating
            // RGBE bytes would spike on exponent changes between texels). Matches the RNM basis path.
            int n = tex.w*tex.h;
            float *fdata = new float[3*n];
            loopi(n) { vec c = decodergbe(&src[i*4]); fdata[i*3] = c.x; fdata[i*3+1] = c.y; fdata[i*3+2] = c.z; }
            createtexture(tex.id, tex.w, tex.h, fdata, 3, 1, GL_RGB16F, GL_TEXTURE_2D, tex.w, tex.h, 0, true, GL_RGB);
            delete[] fdata;
        }
        else createtexture(tex.id, tex.w, tex.h, src, 3, 1, bpp==4 ? GL_RGBA : GL_RGB);
        if(data) delete[] data;
    }
}

bool brightengeom = false, shouldlightents = false;

void clearlights()
{
    clearlightcache();
    const vector<extentity *> &ents = entities::getents();
    loopv(ents)
    {
        extentity &e = *ents[i];
        e.light.color = vec(1, 1, 1);
        e.light.dir = vec(0, 0, 1);
    }
    shouldlightents = false;

    lightmapshdr = false;
    lightmapsrnm = false;
    genlightmaptexs(LM_ALPHA|LM_HDR, 0);
    genlightmaptexs(LM_ALPHA|LM_HDR, LM_ALPHA);
    genlightmaptexs(LM_ALPHA|LM_HDR, LM_HDR);
    brightengeom = true;
}

void lightent(extentity &e, float height)
{
    if(e.type==ET_LIGHT) return;
    float ambient = 0.0f;
    if(e.type==ET_MAPMODEL)
    {
        model *m = loadmodel(NULL, e.attr2);
        if(m) height = m->above()*0.75f;
    }
    else if(e.type>=ET_GAMESPECIFIC) ambient = 0.4f;
    vec target(e.o.x, e.o.y, e.o.z + height);
    lightreaching(target, e.light.color, e.light.dir, false, &e, ambient);
}

void lightents(bool force)
{
    if(!force && !shouldlightents) return;

    const vector<extentity *> &ents = entities::getents();
    loopv(ents) lightent(*ents[i]);

    shouldlightents = false;
}

void initlights()
{
    if((fullbright && editmode) || lightmaps.empty())
    {
        clearlights();
        return;
    }

    clearlightcache();
    lightmapshdr = false;
    lightmapsrnm = false;
    genlightmaptexs(LM_ALPHA|LM_HDR, 0);
    genlightmaptexs(LM_ALPHA|LM_HDR, LM_ALPHA);
    genlightmaptexs(LM_ALPHA|LM_HDR, LM_HDR);
    if(lightmapsrnm)
    {
        // a map baked with RNM: point bumped slots at the dedicated 3-basis RNM world shader
        extern vector<Slot *> slots;
        Shader *rnmsh = lookupshaderbyname("bumprnmworld");
        if(rnmsh) loopv(slots) if(slots[i]->shader && (slots[i]->shader->type&SHADER_NORMALSLMS) && !(slots[i]->shader->type&SHADER_RNM)) slots[i]->shader = rnmsh;
    }
    brightengeom = false;
    shouldlightents = true;
    if(lmnearest) setlightmapfilter();   // keep nearest-neighbour debug filtering across rebakes
}

static inline void fastskylight(const vec &o, float tolerance, uchar *skylight, int flags = RAY_ALPHAPOLY, extentity *t = NULL, bool fast = false)
{
    flags |= RAY_SHADOW;
    if(skytexturelight) flags |= RAY_SKIPSKY | (useskytexture ? RAY_SKYTEX : 0);
    if(fast)
    {
        static const vec ray(0, 0, 1);
        if(shadowray(vec(ray).mul(tolerance).add(o), ray, 1e16f, flags, t)>1e15f)
            memcpy(skylight, skylightcolor.v, 3);
        else memcpy(skylight, ambientcolor.v, 3);
    }
    else
    {
        static const vec rays[5] =
        {
            vec(cosf(66*RAD)*cosf(65*RAD), sinf(66*RAD)*cosf(65*RAD), sinf(65*RAD)),
            vec(cosf(156*RAD)*cosf(65*RAD), sinf(156*RAD)*cosf(65*RAD), sinf(65*RAD)),
            vec(cosf(246*RAD)*cosf(65*RAD), sinf(246*RAD)*cosf(65*RAD), sinf(65*RAD)),
            vec(cosf(336*RAD)*cosf(65*RAD), sinf(336*RAD)*cosf(65*RAD), sinf(65*RAD)),
            vec(0, 0, 1),
        };
        int hit = 0;
        loopi(5) if(shadowray(vec(rays[i]).mul(tolerance).add(o), rays[i], 1e16f, flags, t)>1e15f) hit++;
        loopk(3) skylight[k] = uchar(ambientcolor[k] + (max(skylightcolor[k], ambientcolor[k]) - ambientcolor[k])*hit/5.0f);
    }
}

void lightreaching(const vec &target, vec &color, vec &dir, bool fast, extentity *t, float ambient)
{
    if((fullbright && editmode) || lightmaps.empty())
    {
        color = vec(1, 1, 1);
        dir = vec(0, 0, 1);
        return;
    }

    if(haslightprobes()) { lightprobesreaching(target, color, dir); return; }   // baked ambient-cube grid

    color = dir = vec(0, 0, 0);
    const vector<extentity *> &ents = entities::getents();
    const vector<int> &lights = checklightcache(int(target.x), int(target.y));
    loopv(lights)
    {
        extentity &e = *ents[lights[i]];
        if(e.type != ET_LIGHT)
            continue;

        vec ray(target);
        ray.sub(e.o);
        float mag = ray.magnitude();
        if(e.attr1 && mag >= float(e.attr1))
            continue;

        if(mag < 1e-4f) ray = vec(0, 0, -1);
        else
        {
            ray.div(mag);
            if(shadowray(e.o, ray, mag, RAY_SHADOW | RAY_POLY, t) < mag)
                continue;
        }

        float intensity = 1;
        if(e.attr1)
            intensity -= mag / float(e.attr1);
        if(e.attached && e.attached->type==ET_SPOTLIGHT)
        {
            vec spot = vec(e.attached->o).sub(e.o).normalize();
            float maxatten = sincos360[clamp(int(e.attached->attr1), 1, 89)].x, spotatten = (ray.dot(spot) - maxatten) / (1 - maxatten);
            if(spotatten <= 0) continue;
            intensity *= spotatten;
        }

        //if(target==player->o)
        //{
        //    conoutf(CON_DEBUG, "%d - %f %f", i, intensity, mag);
        //}

        vec lightcol = vec(e.attr2, e.attr3, e.attr4).mul(1.0f/255);
        color.add(vec(lightcol).mul(intensity));
        dir.add(vec(ray).mul(-intensity*(lightcol.x+lightcol.y+lightcol.z)*(1.0f/3)));
    }
    if(sunlight && shadowray(target, sunlightdir, 1e16f, RAY_SHADOW | RAY_POLY | (skytexturelight ? RAY_SKIPSKY | (useskytexture ? RAY_SKYTEX : 0) : 0), t) > 1e15f)
    {
        vec lightcol = vec(sunlightcolor.x, sunlightcolor.y, sunlightcolor.z).mul(sunlightscale/255);
        color.add(lightcol);
        dir.add(vec(sunlightdir).mul((lightcol.x+lightcol.y+lightcol.z)*(1.0f/3)));
    }
    if(hasskylight())
    {
        uchar skylight[3];
        if(t) calcskylight(NULL, target, vec(0, 0, 0), 0.5f, skylight, RAY_POLY, t);
        else fastskylight(target, 0.5f, skylight, RAY_POLY, t, fast);
        loopk(3) color[k] = min(1.5f, max(max(skylight[k]/255.0f, ambient), color[k]));
    }
    else loopk(3) color[k] = min(1.5f, max(max(ambientcolor[k]/255.0f, ambient), color[k]));
    if(dir.iszero()) dir = vec(0, 0, 1);
    else dir.normalize();
}

entity *brightestlight(const vec &target, const vec &dir)
{
    if(sunlight && sunlightdir.dot(dir) > 0 && shadowray(target, sunlightdir, 1e16f, RAY_SHADOW | RAY_POLY | (skytexturelight ? RAY_SKIPSKY | (useskytexture ? RAY_SKYTEX : 0) : 0)) > 1e15f)
        return &sunlightent;
    const vector<extentity *> &ents = entities::getents();
    const vector<int> &lights = checklightcache(int(target.x), int(target.y));
    extentity *brightest = NULL;
    float bintensity = 0;
    loopv(lights)
    {
        extentity &e = *ents[lights[i]];
        if(e.type != ET_LIGHT || vec(e.o).sub(target).dot(dir)<0)
            continue;

        vec ray(target);
        ray.sub(e.o);
        float mag = ray.magnitude();
        if(e.attr1 && mag >= float(e.attr1))
             continue;

        ray.div(mag);
        if(shadowray(e.o, ray, mag, RAY_SHADOW | RAY_POLY) < mag)
            continue;
        float intensity = 1;
        if(e.attr1)
            intensity -= mag / float(e.attr1);
        if(e.attached && e.attached->type==ET_SPOTLIGHT)
        {
            vec spot = vec(e.attached->o).sub(e.o).normalize();
            float maxatten = sincos360[clamp(int(e.attached->attr1), 1, 89)].x, spotatten = (ray.dot(spot) - maxatten) / (1 - maxatten);
            if(spotatten <= 0) continue;
            intensity *= spotatten;
        }

        if(!brightest || intensity > bintensity)
        {
            brightest = &e;
            bintensity = intensity;
        }
    }
    return brightest;
}

void dumplms()
{
    loopv(lightmaps)
    {
        LightMap &lm = lightmaps[i];
        const char *map = game::getclientmap(), *name = strrchr(map, '/');
        defformatstring(buf, "lightmap_%s_%d.png", name ? name+1 : map, i);
        if(lm.type&LM_HDR)
        {
            // decode HDR (RGBE) / RNM (3 RGBE bases) to a viewable, tonemapped RGB image
            ImageData out(LM_PACKW, LM_PACKH, 3);
            loopj(LM_PACKW*LM_PACKH)
            {
                vec c;
                if((lm.type&LM_TYPE)==LM_RNM0)
                    c = vec(decodergbe(&lm.data[j*12])).add(decodergbe(&lm.data[j*12+4])).add(decodergbe(&lm.data[j*12+8])).mul(1.0f/3);
                else c = decodergbe(&lm.data[j*4]);
                loopk(3) out.data[j*3+k] = uchar(255.0f * (c[k] / (c[k] + 1.0f)));   // reinhard for viewing
            }
            savepng(buf, out, true);
        }
        else
        {
            ImageData temp(LM_PACKW, LM_PACKH, lm.bpp, lm.data);
            savepng(buf, temp, true);
        }
    }
}

COMMAND(dumplms, "");

// DEBUG: read back the actual GPU lightmap atlas TEXTURES (post-upload) and dump them, reporting each
// texture's real dimensions/type. Comparing these to dumplms (CPU data) localises an upload shear/shift.
ICOMMAND(dumplmtexs, "", (),
{
    const char *map = game::getclientmap(); const char *name = strrchr(map, '/'); if(name) name++; else name = map;
    loopv(lightmaptexs)
    {
        LightMapTexture &t = lightmaptexs[i];
        if(!t.id || t.w <= 1 || t.h <= 1) continue;
        bool hdr = (t.type&LM_HDR) != 0;
        conoutf("lmtex %d: %dx%d type=0x%02x %s", i, t.w, t.h, t.type, hdr ? "HDR(fp16)" : "LDR(rgb8)");
        glBindTexture(GL_TEXTURE_2D, t.id);
        ImageData out(t.w, t.h, 3);
        if(hdr)
        {
            float *fbuf = new float[size_t(t.w)*t.h*3];
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_FLOAT, fbuf);
            loop(p, t.w*t.h) loopk(3) out.data[p*3+k] = uchar(255.0f * (fbuf[p*3+k] / (fbuf[p*3+k] + 1.0f)));
            delete[] fbuf;
        }
        else glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, out.data);
        defformatstring(buf, "lmtex_%s_%d.png", name, i);
        savepng(buf, out, true);
    }
});

// DEBUG: overwrite every lightmap with a distinct solid colour and re-upload. If world surfaces then
// show these colours, the lightmaps ARE being sampled; if they stay unchanged (e.g. white/fullbright),
// the lighting is not coming from the lightmaps (check `fullbright 0`, brightengeom, lmid binding).
ICOMMAND(debuglm, "", (),
{
    loopv(lightmaps)
    {
        LightMap &lm = lightmaps[i];
        int cr = (i*101 + 40)%256;
        int cg = (i*53 + 90)%256;
        int cb = (i*197 + 10)%256;
        loopj(LM_PACKW*LM_PACKH)
        {
            if(lm.type&LM_HDR)
            {
                vec c(cr/255.0f, cg/255.0f, cb/255.0f);
                if((lm.type&LM_TYPE)==LM_RNM0) loopk(3) encodergbe(c, &lm.data[j*12 + 4*k]);
                else encodergbe(c, &lm.data[j*4]);
            }
            else
            {
                lm.data[j*lm.bpp] = cr;
                if(lm.bpp > 1) lm.data[j*lm.bpp + 1] = cg;
                if(lm.bpp > 2) lm.data[j*lm.bpp + 2] = cb;
                if(lm.bpp > 3) lm.data[j*lm.bpp + 3] = 255;
            }
        }
    }
    cleanuplightmaps();
    initlights();
    conoutf("debuglm: filled %d lightmaps with solid debug colours", lightmaps.length());
});
