// bezpatch.cpp: Quake3-style bezier patches (clean-room implementation).
// Phase 1: data model, biquadratic tessellation, creation command, basic rendering.

#include "engine.h"

vector<bezpatch *> patches;

void clearpatches()
{
#ifndef STANDALONE
    loopv(patches) patches[i]->freelm();
#endif
    patches.deletecontents();
}

// Evaluate a single biquadratic (3x3) sub-patch (iu,iv) at local parameters (u,v) in [0,1],
// returning the surface position and the two parametric tangents (for the normal).
void bezpatch::evalsubpatch(int iu, int iv, float u, float v, vec &pos, vec &du, vec &dv) const
{
    float bu[3]  = { (1-u)*(1-u), 2*u*(1-u), u*u };
    float bv[3]  = { (1-v)*(1-v), 2*v*(1-v), v*v };
    float dbu[3] = { -2*(1-u),    2*(1-2*u), 2*u };
    float dbv[3] = { -2*(1-v),    2*(1-2*v), 2*v };
    pos = du = dv = vec(0, 0, 0);
    loop(b, 3) loop(a, 3)
    {
        const vec &p = cp(2*iu + a, 2*iv + b);
        pos.add(vec(p).mul(bu[a]  * bv[b]));
        du.add (vec(p).mul(dbu[a] * bv[b]));
        dv.add (vec(p).mul(bu[a]  * dbv[b]));
    }
}

void bezpatch::tessellate()
{
    verts.setsize(0); norms.setsize(0); tangents.setsize(0); tcs.setsize(0); lmtc.setsize(0); tris.setsize(0);
    dirty = false;
    int nu = usub(), nv = vsub();
    if(nu < 1 || nv < 1 || tess < 1) return;
    int su = nu*tess, sv = nv*tess;            // grid segments along each axis
    if((su+1)*(sv+1) > 0xFFFF) return;          // ushort index budget (raise tess limits later)

    loopj(sv+1) loopi(su+1)
    {
        float gu = float(i)/tess, gv = float(j)/tess;   // global params in [0,nu]x[0,nv]
        int iu = min(int(gu), nu-1), iv = min(int(gv), nv-1);
        vec pos, tu, tv;
        evalsubpatch(iu, iv, gu - iu, gv - iv, pos, tu, tv);
        vec n;
        n.cross(tu, tv);
        n = n.iszero() ? vec(0, 0, 1) : n.normalize();
        // tangent = d/du orthonormalized against the normal (matches the diffuse-u direction)
        vec tang = tu.iszero() ? vec(1, 0, 0) : vec(tu).normalize();
        tang.sub(vec(n).mul(n.dot(tang)));
        tang = tang.iszero() ? vec(1, 0, 0) : tang.normalize();
        norms.add(n);
        tangents.add(tang);
        verts.add(pos);
        tcs.add(vec2(0, 0));   // filled below with world arc length
    }

    int stride = su+1;
    // natural (arc-length) texture coords in world units: distance along u (row 0) and v (col 0),
    // so texel density stays uniform regardless of patch size/curvature.
    loopj(sv+1) loopi(su+1)
    {
        float s = 0, t = 0;
        loopk(i) s += verts[j*stride + k+1].dist(verts[j*stride + k]);
        loopk(j) t += verts[(k+1)*stride + i].dist(verts[k*stride + i]);
        tcs[j*stride + i] = vec2(s, t);
        lmtc.add(vec2(su ? float(i)/su : 0, sv ? float(j)/sv : 0));   // grid fraction for the baked lightmap
    }

    loopj(sv) loopi(su)
    {
        ushort a = j*stride + i, b = a+1, c = a+stride, d = c+1;
        tris.add(a); tris.add(b); tris.add(c);
        tris.add(c); tris.add(b); tris.add(d);
    }
}

void bezpatch::boundsphere(vec &center, float &radius) const
{
    center = vec(0, 0, 0);
    if(ctrl.empty()) { radius = 0; return; }
    loopv(ctrl) center.add(ctrl[i]);
    center.div(ctrl.length());
    float r2 = 0;
    loopv(ctrl) r2 = max(r2, center.squaredist(ctrl[i]));
    radius = sqrtf(r2);
}

// ---- serialization (.ogz section, MAPVERSION>=35; runs on client and server) ----

void saveworldpatches(stream *f)
{
    f->putlil<int>(patches.length());
    loopv(patches)
    {
        bezpatch *p = patches[i];
        f->putlil<ushort>(p->cols);
        f->putlil<ushort>(p->rows);
        f->putlil<int>(p->vslot);
        f->putlil<ushort>(p->tess);
        loopvj(p->ctrl)
        {
            f->putlil<float>(p->ctrl[j].x);
            f->putlil<float>(p->ctrl[j].y);
            f->putlil<float>(p->ctrl[j].z);
        }
    }
}

void loadworldpatches(stream *f)
{
    clearpatches();
    int n = f->getlil<int>();
    if(n < 0 || n > 100000) return;             // guard against corruption
    loopi(n)
    {
        int cols = f->getlil<ushort>(), rows = f->getlil<ushort>();
        int vslot = f->getlil<int>(), tess = f->getlil<ushort>();
        // sanity: odd dims in a sane range; bail out cleanly on garbage
        if(cols < 3 || rows < 3 || cols > 99 || rows > 99 || !(cols&1) || !(rows&1)) return;
        bezpatch *p = new bezpatch;
        p->setdims(cols, rows);
        p->vslot = vslot;
        p->tess = clamp(tess, 1, 16);
        loopvj(p->ctrl)
        {
            p->ctrl[j].x = f->getlil<float>();
            p->ctrl[j].y = f->getlil<float>();
            p->ctrl[j].z = f->getlil<float>();
        }
        p->dirty = true;
        patches.add(p);
    }
}

#ifndef STANDALONE

extern int currentedittex();   // octaedit.cpp: editor's current texture (declared locally, like entdrag)
extern selinfo sel;            // current edit selection (world.cpp); used to place new patches
extern void createtexture(int tnum, int w, int h, void *pixels, int clamp, int filter, GLenum component, GLenum subtarget, int pw, int ph, int pitch, bool resize, GLenum format, bool swizzle);
extern void calctexgen(VSlot &vslot, int dim, vec4 &sgen, vec4 &tgen);   // cube texture projection (octarender.cpp)
extern const vec orientation_tangent[8][3];                              // per-orientation tangent the cube uses

// dominant axis of a patch's average normal -- the "face" it projects onto, like a cube face's dimension
static int patchdim(bezpatch *p)
{
    vec n(0, 0, 0);
    loopv(p->norms) n.add(p->norms[i]);
    if(n.iszero()) return 2;
    vec a(fabs(n.x), fabs(n.y), fabs(n.z));
    return a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);
}

void bezpatch::freelm()
{
    loopk(3) if(lmtex[k]) { glDeleteTextures(1, (GLuint *)&lmtex[k]); lmtex[k] = 0; }
}

// decode the 3 baked RGBE basis buffers to linear fp16 and upload as 3 lightmap textures (RNM)
void bezpatch::uploadlm(uchar *rgbe0, uchar *rgbe1, uchar *rgbe2, int gw, int gh)
{
    uchar *srcs[3] = { rgbe0, rgbe1, rgbe2 };
    float *fdata = new float[gw*gh*3];
    loopk(3)
    {
        if(!lmtex[k]) { GLuint id = 0; glGenTextures(1, &id); lmtex[k] = id; }
        loopi(gw*gh) { vec c = decodergbe(&srcs[k][i*4]); fdata[i*3] = c.x; fdata[i*3+1] = c.y; fdata[i*3+2] = c.z; }
        createtexture(lmtex[k], gw, gh, fdata, 1, 1, GL_RGB16F, GL_TEXTURE_2D, gw, gh, 0, false, GL_RGB, false);
    }
    delete[] fdata;
}

// --- bake interface: raw-pointer accessors so the (fast-math) lightmap.cpp bake never touches the struct ---
int numpatches() { return patches.length(); }

VARP(patchlmscale, 1, 8, 64);   // world units per baked lightmap texel (smaller = finer/slower)

// Build a bake grid sampled at a world-space density (independent of the render tessellation), so the
// baked lightmap stays smooth on large patches. The render mesh uses grid-fraction lm coords (lmtc),
// which address this texture regardless of its resolution. Uses static buffers (the bake is single-threaded).
bool getpatchgrid(int i, const vec *&verts, const vec *&norms, const vec *&tangents, int &gw, int &gh)
{
    static vector<vec> bpos, bnrm, btang;
    if(!patches.inrange(i)) return false;
    bezpatch *p = patches[i];
    if(p->dirty) p->tessellate();
    int nu = p->usub(), nv = p->vsub();
    if(nu < 1 || nv < 1) return false;
    float ulen = 0, vlen = 0;                         // patch arc lengths (from the natural tex coords)
    loopvj(p->tcs) { ulen = max(ulen, p->tcs[j].x); vlen = max(vlen, p->tcs[j].y); }
    gw = clamp(int(ulen/patchlmscale) + 1, 2, 128);
    gh = clamp(int(vlen/patchlmscale) + 1, 2, 128);
    // tangent frame matches the cube's (orientation_tangent for the patch's dominant face), so the
    // normal map -- sampled in the same calctexgen space the render uses -- bumps consistently.
    int dim = patchdim(p);
    vec stan = orientation_tangent[lookupvslot(p->vslot, false).rotation][dim];
    bpos.setsize(0); bnrm.setsize(0); btang.setsize(0);
    loopj(gh) loopi(gw)
    {
        float gu = nu*float(i)/(gw-1), gv = nv*float(j)/(gh-1);
        int iu = min(int(gu), nu-1), iv = min(int(gv), nv-1);
        vec pos, tu, tv;
        p->evalsubpatch(iu, iv, gu-iu, gv-iv, pos, tu, tv);
        vec n;
        n.cross(tu, tv);
        n = n.iszero() ? vec(0, 0, 1) : n.normalize();
        vec tang = vec(stan);
        tang.sub(vec(n).mul(n.dot(tang)));
        tang = tang.iszero() ? vec(1, 0, 0) : tang.normalize();
        bpos.add(pos); bnrm.add(n); btang.add(tang);
    }
    verts = bpos.getbuf();
    norms = bnrm.getbuf();
    tangents = btang.getbuf();
    return true;
}

void setpatchlm(int i, uchar *rgbe0, uchar *rgbe1, uchar *rgbe2, int gw, int gh)
{
    if(patches.inrange(i)) patches[i]->uploadlm(rgbe0, rgbe1, rgbe2, gw, gh);
}

void tessellateallpatches()   // ensure every patch mesh is current before a bake reads it
{
    loopv(patches) if(patches[i]->dirty) patches[i]->tessellate();
}

// patches occlude light during the lightmap bake (set only while calclight runs, so runtime shadow
// rays skip this entirely). Read-only over the (already tessellated) patch meshes -> bake-thread safe.
bool patchcastshadows = false;

static inline bool raytri(const vec &o, const vec &dir, const vec &v0, const vec &v1, const vec &v2, float &t)
{
    vec e1 = vec(v1).sub(v0), e2 = vec(v2).sub(v0), p;
    p.cross(dir, e2);
    float det = e1.dot(p);
    if(det > -1e-6f && det < 1e-6f) return false;
    float inv = 1.0f/det;
    vec tv = vec(o).sub(v0);
    float u = tv.dot(p)*inv;
    if(u < 0 || u > 1) return false;
    vec q;
    q.cross(tv, e1);
    float vv = dir.dot(q)*inv;
    if(vv < 0 || u+vv > 1) return false;
    t = e2.dot(q)*inv;
    return t > 1e-3f;
}

// --- collision: players/projectiles collide with the tessellated patch triangles ---
VARP(patchclip, 0, 1, 1);   // 0 disables patch collision
extern bool collidetriangle(physent *d, const vec &dir, float cutoff, const vec &a, const vec &b, const vec &c);

bool patchcollide(physent *d, const vec &dir, float cutoff)
{
    if(!patchclip || patches.empty()) return false;
    vec pmin(d->o.x - d->radius, d->o.y - d->radius, d->o.z - d->eyeheight);
    vec pmax(d->o.x + d->radius, d->o.y + d->radius, d->o.z + d->aboveeye);
    loopv(patches)
    {
        bezpatch *p = patches[i];
        if(p->dirty) p->tessellate();
        if(p->tris.empty()) continue;
        vec bc; float br;
        p->boundsphere(bc, br);
        if(fabs(bc.x - d->o.x) > br + d->radius || fabs(bc.y - d->o.y) > br + d->radius ||
           bc.z + br < pmin.z || bc.z - br > pmax.z) continue;
        for(int j = 0; j+2 < p->tris.length(); j += 3)
        {
            const vec &a = p->verts[p->tris[j]], &b = p->verts[p->tris[j+1]], &c = p->verts[p->tris[j+2]];
            if(min(a.x, min(b.x, c.x)) > pmax.x || max(a.x, max(b.x, c.x)) < pmin.x ||
               min(a.y, min(b.y, c.y)) > pmax.y || max(a.y, max(b.y, c.y)) < pmin.y ||
               min(a.z, min(b.z, c.z)) > pmax.z || max(a.z, max(b.z, c.z)) < pmin.z) continue;
            if(collidetriangle(d, dir, cutoff, a, b, c)) return true;
        }
    }
    return false;
}

// distance to the nearest patch triangle hit by the ray (o + dir*t, dir unit), else radius
float patchshadowdist(const vec &o, const vec &dir, float radius)
{
    float best = radius;
    loopv(patches)
    {
        bezpatch *p = patches[i];
        if(p->dirty || p->tris.empty()) continue;
        vec c; float r;
        p->boundsphere(c, r);
        // ray-vs-bounding-sphere reject
        vec oc = vec(c).sub(o);
        float proj = clamp(oc.dot(dir), 0.0f, best);
        if(vec(dir).mul(proj).add(o).squaredist(c) > r*r) continue;
        for(int j = 0; j+2 < p->tris.length(); j += 3)
        {
            float t;
            if(raytri(o, dir, p->verts[p->tris[j]], p->verts[p->tris[j+1]], p->verts[p->tris[j+2]], t) && t < best)
                best = t;
        }
    }
    return best;
}

// edit state (defined up here so the creation commands can reference them)
int patchhover = -1, patchhovercp = -1;   // patch + control-point index under the crosshair
int patchorient = 0;                       // box face under the crosshair (like entorient)
int patchmoving = 0;                       // 0 idle, 1 first drag frame, 2 dragging

// ---- creation -------------------------------------------------------------

VARP(patchtess, 1, 4, 16);   // default subdivision level for new patches

// Spawn a flat patch on the selected face of the selection volume, spanning the selection (or a
// default size centred on the face when nothing is selected), offset slightly outward along the
// face normal -- mirroring how dropentity() places a new entity (see world.cpp).
ICOMMAND(newpatch, "i", (int *size),
{
    if(noedit(true)) return;
    int d = dimension(sel.orient);
    int dc = dimcoord(sel.orient);
    int g = sel.grid;
    float er = sel.s[R[d]]*g;       // selection extent in the two in-plane axes
    float ec = sel.s[C[d]]*g;
    float def = *size > 0 ? *size : g*4;
    if(er < 1) er = def;
    if(ec < 1) ec = def;
    vec base(sel.o);
    if(sel.s[R[d]] < 1) base[R[d]] = sel.o[R[d]] + g*0.5f - er*0.5f;   // no extent: centre on the face cell
    if(sel.s[C[d]] < 1) base[C[d]] = sel.o[C[d]] + g*0.5f - ec*0.5f;
    float dplane = sel.o[D[d]] + (dc ? sel.s[D[d]]*g : 0);
    float off = (dc ? 1 : -1) * max(2.0f, g*0.5f);      // lift off the face like an entity's radius offset

    bezpatch *p = new bezpatch;
    p->tess = patchtess;
    p->vslot = currentedittex();                        // adopt the editor's current texture
    loop(y, 3) loop(x, 3)
    {
        vec &cp = p->cp(x, y);
        cp[R[d]] = base[R[d]] + x*0.5f*er;
        cp[C[d]] = base[C[d]] + y*0.5f*ec;
        cp[D[d]] = dplane + off;
    }
    p->dirty = true;
    patches.add(p);
    conoutf("created patch %d (%d total)", patches.length()-1, patches.length());
});

ICOMMAND(clearpatches, "", (),
{
    if(noedit(true)) return;
    int n = patches.length();
    clearpatches();
    conoutf("cleared %d patches", n);
});

ICOMMAND(patchcount, "", (), { conoutf("%d patches", patches.length()); intret(patches.length()); });

// assign the editor's current texture/material to the hovered (or last) patch
ICOMMAND(patchsettex, "", (),
{
    if(noedit(true)) return;
    bezpatch *p = patches.inrange(patchhover) ? patches[patchhover] : (patches.empty() ? NULL : patches.last());
    if(!p) { conoutf("no patch"); return; }
    p->vslot = currentedittex();
    conoutf("patch texture set to %d", p->vslot);
});

// ---- rendering ------------------------------------------------------------

void renderpatchhandles();
extern int currentedittex();

VARP(patchtexscale, 1, 32, 512);   // world units per texture tile (before the slot's own scale)

// find a slot texture of a given type (TEX_NORMAL/TEX_GLOW/...), like changeslottmus does
static Texture *slottex(Slot &s, int type)
{
    loopvj(s.sts) if(s.sts[j].type == type && s.sts[j].t) return s.sts[j].t;
    return NULL;
}

void renderpatches()
{
    if(patches.empty()) return;
    loopv(patches) if(patches[i]->dirty) patches[i]->tessellate();

    glDisable(GL_CULL_FACE);     // patches are two-sided until per-face culling settles in a later phase
    glDisable(GL_BLEND);

    // forward sun + ambient (used by unbaked patches; baked patches sample their RNM lightmaps instead)
    GLOBALPARAM(psundir, sunlightdir);
    GLOBALPARAMF(psun, sunlightcolor.x/255.0f*sunlightscale, sunlightcolor.y/255.0f*sunlightscale, sunlightcolor.z/255.0f*sunlightscale);
    GLOBALPARAMF(pambient, max(max(ambientcolor.x, skylightcolor.x)/255.0f, 0.15f),
                            max(max(ambientcolor.y, skylightcolor.y)/255.0f, 0.15f),
                            max(max(ambientcolor.z, skylightcolor.z)/255.0f, 0.15f));

    gle::defvertex();
    gle::defnormal();
    gle::deftangent(3);
    gle::deftexcoord0();
    gle::deftexcoord1();

    loopv(patches)
    {
        bezpatch *p = patches[i];
        if(p->tris.empty()) continue;
        VSlot &vs = lookupvslot(p->vslot, true);
        Slot &s = *vs.slot;
        Texture *diffuse = s.sts.inrange(0) ? s.sts[0].t : notexture;
        Texture *norm = slottex(s, TEX_NORMAL), *glow = slottex(s, TEX_GLOW);
        bool baked = p->lmtex[0] != 0;
        if(baked) SETSHADER(bezpatchlm); else SETSHADER(bezpatch);
        glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, diffuse->id);
        glActiveTexture_(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, (norm ? norm : notexture)->id);
        glActiveTexture_(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, (glow ? glow : notexture)->id);
        if(baked) loopk(3) { glActiveTexture_(GL_TEXTURE3+k); glBindTexture(GL_TEXTURE_2D, p->lmtex[k]); }
        glActiveTexture_(GL_TEXTURE0);
        GLOBALPARAMF(pcolor, 2*vs.colorscale.x, 2*vs.colorscale.y, 2*vs.colorscale.z, 1.0f);   // 2x overbright, like world surfaces
        GLOBALPARAMF(pbump, norm ? 1.0f : 0.0f);
        GLOBALPARAMF(pglow, glow ? 1.0f : 0.0f);
        // diffuse/normal coords via the same planar texgen the cube uses (matches orientation/scale/rotation)
        int dim = patchdim(p);
        vec4 sgen, tgen;
        calctexgen(vs, dim, sgen, tgen);
        gle::begin(GL_TRIANGLES);
        loopvj(p->tris)
        {
            ushort idx = p->tris[j];
            const vec &pos = p->verts[idx];
            gle::attrib(pos);
            gle::attrib(p->norms[idx]);
            gle::attrib(p->tangents[idx]);
            gle::attrib(vec2(sgen.x*pos.x + sgen.y*pos.y + sgen.z*pos.z + sgen.w,
                             tgen.x*pos.x + tgen.y*pos.y + tgen.z*pos.z + tgen.w));
            gle::attrib(p->lmtc[idx]);
        }
        xtraverts += gle::end();
    }

    glEnable(GL_CULL_FACE);
    // control-point handles are NOT drawn here: they are rendered from rendereditcursor() so they
    // share the entity-selection render state (additive blend + line offset) and thus look identical.
}

// ---- control-point editing (mirrors the entity drag mechanism) -------------

extern bool editmoveplane(const vec &o, const vec &ray, int d, float off, vec &handle, vec &dest, bool first);
extern void boxs3D(const vec &o, vec s, int g);
extern void boxs(int orient, vec o, const vec &s);
extern void boxs(int orient, vec o, const vec &s, float size);
extern int entselradius;   // world.cpp: size of an entity position marker; control points match it
extern int entselsnap;
extern selinfo sel;

// the axis-aligned box that represents a control-point handle (identical to an entity marker box)
static void cpbox(const vec &c, vec &eo, vec &es)
{
    eo = vec(c).sub(entselradius);
    es = vec(entselradius*2);
}

// Pick the control point under the crosshair exactly like an entity marker: ray vs the handle box,
// capturing which face was hit (mirrors rayent/entorient). Sets patchhover/patchhovercp/patchorient
// and returns the hit distance (1e16 if none) so the editor can compare it against world/entity hits.
float raypatchcp(const vec &o, const vec &ray)
{
    patchhover = patchhovercp = -1;
    float best = 1e16f;
    loopv(patches)
    {
        bezpatch *p = patches[i];
        loopvj(p->ctrl)
        {
            vec eo, es;
            cpbox(p->ctrl[j], eo, es);
            float dist = 1e16f;
            int orient = 0;
            if(rayboxintersect(eo, es, o, ray, dist, orient) && dist < best)
            {
                best = dist;
                patchhover = i;
                patchhovercp = j;
                patchorient = orient;
            }
        }
    }
    return best;
}

// move the hovered control point within the plane of the grabbed box face -- the exact mechanism
// entdrag() uses (d = the face dimension, slide within R[d]/C[d]).
void patchdrag(const vec &ray)
{
    if(!patches.inrange(patchhover)) { patchmoving = 0; return; }
    bezpatch *p = patches[patchhover];
    if(!p->ctrl.inrange(patchhovercp)) { patchmoving = 0; return; }
    vec &c = p->ctrl[patchhovercp];
    int d = dimension(patchorient), dc = dimcoord(patchorient);
    vec eo, es;
    cpbox(c, eo, es);
    static vec handle, dest;
    if(!editmoveplane(c, ray, d, eo[d] + (dc ? es[d] : 0), handle, dest, patchmoving==1)) return;
    // grid snap within the face plane -- identical to entdrag()'s entselsnap handling
    ivec g(dest);
    int z = g[d]&(~(sel.grid-1));
    g.add(sel.grid/2).mask(~(sel.grid-1));
    g[d] = z;
    c[R[d]] = entselsnap ? g[R[d]] : dest[R[d]];
    c[C[d]] = entselsnap ? g[C[d]] : dest[C[d]];
    p->dirty = true;
    patchmoving = 2;
}

// called from rendereditcursor while a drag is in progress
void editpatches(const vec &ray)
{
    if(patchmoving) patchdrag(ray);
}

void renderpatchhandles()
{
    loopv(patches)
    {
        bezpatch *p = patches[i];
        // control net
        gle::colorub(60, 120, 220);
        gle::defvertex();
        gle::begin(GL_LINES);
        loop(y, p->rows) loop(x, p->cols)
        {
            if(x+1 < p->cols) { gle::attrib(p->cp(x, y)); gle::attrib(p->cp(x+1, y)); }
            if(y+1 < p->rows) { gle::attrib(p->cp(x, y)); gle::attrib(p->cp(x, y+1)); }
        }
        xtraverts += gle::end();
    }

    // control-point markers themselves are the blue PART_EDIT sparkles emitted in renderparticles.cpp
    // (same as entities). Only the hovered one gets the selection box + red grab-face, exactly like
    // renderentselection() does for enthover.
    if(patches.inrange(patchhover) && patches[patchhover]->ctrl.inrange(patchhovercp))
    {
        vec eo, es;
        cpbox(patches[patchhover]->ctrl[patchhovercp], eo, es);
        gle::colorub(0, 40, 0);
        boxs3D(eo, es, 1);
        gle::colorub(150, 0, 0);
        boxs(patchorient, eo, es);
        boxs(patchorient, eo, es, clamp(0.015f*camera1->o.dist(eo)*tan(fovy*0.5f*RAD), 0.1f, 1.0f));
    }
}

// hovering a control point starts/continues a drag (bind a key to this, like entmoving)
ICOMMAND(patchmoving, "b", (int *n),
{
    if(*n >= 0)
    {
        if(!*n || patchhover < 0 || noedit(true)) patchmoving = 0;
        else if(!patchmoving) patchmoving = 1;
    }
    intret(patchmoving);
});

// active patch for grow/shrink/material ops: the hovered one, else the last created
static bezpatch *activepatch()
{
    if(patches.inrange(patchhover)) return patches[patchhover];
    return patches.empty() ? NULL : patches.last();
}

// grow/shrink by whole biquadratic sub-patches (+/-2 control points per axis), Quake3/Radiant style
ICOMMAND(patchgrow, "ii", (int *du, int *dv),
{
    if(noedit(true)) return;
    bezpatch *p = activepatch();
    if(!p) { conoutf("no patch"); return; }
    int nc = clamp(p->cols + 2*(*du), 3, 99);
    int nr = clamp(p->rows + 2*(*dv), 3, 99);
    if(!(nc&1)) nc--;
    if(!(nr&1)) nr--;
    p->setdims(nc, nr);
    conoutf("patch grid %dx%d", p->cols, p->rows);
});

// set one control point explicitly (scripting + basis for fix/align commands)
ICOMMAND(patchcp, "iifff", (int *patch, int *cp, float *x, float *y, float *z),
{
    if(noedit(true)) return;
    if(!patches.inrange(*patch) || !patches[*patch]->ctrl.inrange(*cp)) return;
    patches[*patch]->ctrl[*cp] = vec(*x, *y, *z);
    patches[*patch]->dirty = true;
});

// set a patch's texture/material vslot directly (scripting/testing)
ICOMMAND(patchsetvslot, "ii", (int *patch, int *vslot),
{
    if(noedit(true)) return;
    if(!patches.inrange(*patch)) return;
    patches[*patch]->vslot = *vslot;
    patches[*patch]->dirty = true;
});

// move a control point by a delta (scripting/testing)
ICOMMAND(patchnudge, "iifff", (int *patch, int *cp, float *dx, float *dy, float *dz),
{
    if(noedit(true)) return;
    if(!patches.inrange(*patch) || !patches[*patch]->ctrl.inrange(*cp)) return;
    patches[*patch]->ctrl[*cp].add(vec(*dx, *dy, *dz));
    patches[*patch]->dirty = true;
});

#endif // !STANDALONE
