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

// interpolate the stored control-point UVs with the same biquadratic basis as the positions
vec2 bezpatch::evalsubpatchuv(int iu, int iv, float u, float v) const
{
    float bu[3] = { (1-u)*(1-u), 2*u*(1-u), u*u };
    float bv[3] = { (1-v)*(1-v), 2*v*(1-v), v*v };
    vec2 uv(0, 0);
    loop(b, 3) loop(a, 3)
        uv.add(vec2(cpuv[(2*iv+b)*cols + (2*iu+a)]).mul(bu[a]*bv[b]));
    return uv;
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
        norms.add(n.iszero() ? vec(0, 0, 1) : n.normalize());
        verts.add(pos);
        tcs.add(evalsubpatchuv(iu, iv, gu - iu, gv - iv));               // interpolated stored UVs
        lmtc.add(vec2(su ? float(i)/su : 0, sv ? float(j)/sv : 0));      // grid fraction for the baked lightmap
        tangents.add(vec(0, 0, 0));                                      // accumulated from the UV gradient below
    }

    int stride = su+1;
    loopj(sv) loopi(su)
    {
        ushort a = j*stride + i, b = a+1, c = a+stride, d = c+1;
        tris.add(a); tris.add(b); tris.add(c);
        tris.add(c); tris.add(b); tris.add(d);
    }

    // per-vertex tangent from the position/UV gradient (Lengyel), so the normal map lines up with whatever
    // UV mapping is stored. Accumulate per triangle, then orthonormalize against the normal.
    for(int t = 0; t+2 < tris.length(); t += 3)
    {
        int i0 = tris[t], i1 = tris[t+1], i2 = tris[t+2];
        vec e1 = vec(verts[i1]).sub(verts[i0]), e2 = vec(verts[i2]).sub(verts[i0]);
        vec2 d1 = vec2(tcs[i1]).sub(tcs[i0]), d2 = vec2(tcs[i2]).sub(tcs[i0]);
        float denom = d1.x*d2.y - d2.x*d1.y;
        if(denom > -1e-9f && denom < 1e-9f) continue;
        vec tan = vec(e1).mul(d2.y).sub(vec(e2).mul(d1.y)).mul(1.0f/denom);
        tangents[i0].add(tan); tangents[i1].add(tan); tangents[i2].add(tan);
    }
    loopv(tangents)
    {
        vec t = tangents[i];
        t.sub(vec(norms[i]).mul(norms[i].dot(t)));
        tangents[i] = t.iszero() ? vec(1, 0, 0) : t.normalize();
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

// default control-point UVs: planar projection onto the patch's dominant axis at a fixed scale. Computed
// from the control points alone (no VSlot), so it works on the server and for old maps. patchprojaxis
// refines it to match an adjacent cube (VSlot scale/rotation).
void defaultpatchuv(bezpatch *p)
{
    vec e1 = vec(p->cp(p->cols-1, 0)).sub(p->cp(0, 0)), e2 = vec(p->cp(0, p->rows-1)).sub(p->cp(0, 0)), n;
    n.cross(e1, e2);
    if(n.iszero()) n = vec(0, 0, 1);
    vec a(fabs(n.x), fabs(n.y), fabs(n.z));
    int dim = a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);
    static const int si[] = { 1, 0, 0 }, ti[] = { 2, 2, 1 };
    float k = 1.0f/32.0f;
    loop(y, p->rows) loop(x, p->cols) { const vec &c = p->cp(x, y); p->uv(x, y) = vec2(c[si[dim]]*k, c[ti[dim]]*k); }
    p->dirty = true;
}

// ---- serialization (.ogz section, MAPVERSION>=35; runs on client and server; UVs added in v36) ----

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
        loopvj(p->cpuv) { f->putlil<float>(p->cpuv[j].x); f->putlil<float>(p->cpuv[j].y); }   // v36+
    }
}

void loadworldpatches(stream *f, int mapversion)
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
        if(mapversion >= 36) loopvj(p->cpuv) { p->cpuv[j].x = f->getlil<float>(); p->cpuv[j].y = f->getlil<float>(); }
        else defaultpatchuv(p);   // pre-UV patches: synthesize a planar mapping
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
extern void moveentgroup(int d, float dr, float dc);                     // world.cpp: drag selected entities too
extern void clearentgroup();                                             // world.cpp: deselect entities

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

// The bake uses the tessellation grid directly (its tangents are derived from the active UV mapping, so the
// baked lightmap and the rendered normal map stay consistent). Lightmap resolution = tessellation; raise
// patchtess for finer baked lighting.
bool getpatchgrid(int i, const vec *&verts, const vec *&norms, const vec *&tangents, int &gw, int &gh)
{
    if(!patches.inrange(i)) return false;
    bezpatch *p = patches[i];
    if(p->dirty) p->tessellate();
    int nu = p->usub(), nv = p->vsub();
    if(nu < 1 || nv < 1) return false;
    gw = nu*p->tess + 1;
    gh = nv*p->tess + 1;
    if(p->verts.length() != gw*gh) return false;
    verts = p->verts.getbuf();
    norms = p->norms.getbuf();
    tangents = p->tangents.getbuf();
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
int patchhover = -1, patchhovercp = -1;     // patch + control-point index under the crosshair (transient)
int patchhoversurf = -1;                     // patch whose SURFACE (body) is under the crosshair (no CP hit)
vec patchsurfnormal(0, 0, 1);                // surface normal at that hit (for the drag plane)
int patchorient = 0;                         // box face under the crosshair (like entorient)
int patchmoving = 0;                         // 0 idle, 1 first drag frame, 2 dragging

// selected control points (a group, possibly spanning multiple patches), like the entity entgroup
struct patchcpsel { int patch, cp; };
vector<patchcpsel> patchgroup;
static int patchgroupfind(int p, int c) { loopv(patchgroup) if(patchgroup[i].patch == p && patchgroup[i].cp == c) return i; return -1; }

// deselect all control points (called from cancelsel() so SPACE clears patches with everything else)
void patchcancel() { patchgroup.setsize(0); }

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
    extern void projectpatchuv_axis(bezpatch *p);
    projectpatchuv_axis(p);                     // default to axis projection (matches a coplanar cube face)
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
    // flat ambient floor for unbaked patches: visible grey rather than near-black before /calclight
    GLOBALPARAMF(pambient, max(max(ambientcolor.x, skylightcolor.x)/255.0f, 0.35f),
                            max(max(ambientcolor.y, skylightcolor.y)/255.0f, 0.35f),
                            max(max(ambientcolor.z, skylightcolor.z)/255.0f, 0.35f));

    gle::defvertex();
    gle::defnormal();
    gle::deftangent(3);
    gle::deftexcoord0();
    gle::deftexcoord1();

    GLOBALPARAM(camera, camera1->o);   // for tangent-space view dir (parallax/spec)

    loopv(patches)
    {
        bezpatch *p = patches[i];
        if(p->tris.empty()) continue;
        VSlot &vs = lookupvslot(p->vslot, true);
        Slot &s = *vs.slot;
        Texture *diffuse = s.sts.inrange(0) ? s.sts[0].t : notexture;
        Texture *norm = slottex(s, TEX_NORMAL), *glow = slottex(s, TEX_GLOW), *spec = slottex(s, TEX_SPEC);
        bool baked = p->lmtex[0] != 0;
        if(baked) SETSHADER(bezpatchlm); else SETSHADER(bezpatch);
        glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, diffuse->id);
        glActiveTexture_(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, (norm ? norm : notexture)->id);
        glActiveTexture_(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, (glow ? glow : notexture)->id);
        if(baked)
        {
            loopk(3) { glActiveTexture_(GL_TEXTURE3+k); glBindTexture(GL_TEXTURE_2D, p->lmtex[k]); }
            glActiveTexture_(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, (spec ? spec : notexture)->id);
            GLOBALPARAMF(pspec, spec ? 1.0f : 0.0f);
            bool height = (s.texmask & (1<<TEX_DEPTH)) != 0;
            GLOBALPARAMF(pparallax, height ? 0.06f : 0.0f, height ? -0.03f : 0.0f);
        }
        glActiveTexture_(GL_TEXTURE0);
        GLOBALPARAMF(pcolor, 2*vs.colorscale.x, 2*vs.colorscale.y, 2*vs.colorscale.z, 1.0f);   // 2x overbright, like world surfaces
        GLOBALPARAMF(pbump, norm ? 1.0f : 0.0f);
        GLOBALPARAMF(pglow, glow ? 1.0f : 0.0f);
        gle::begin(GL_TRIANGLES);
        loopvj(p->tris)
        {
            ushort idx = p->tris[j];
            gle::attrib(p->verts[idx]);
            gle::attrib(p->norms[idx]);
            gle::attrib(p->tangents[idx]);
            gle::attrib(p->tcs[idx]);          // stored (interpolated) control-point UVs
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
    for(int i = patchgroup.length(); --i >= 0; )   // drop stale selections (e.g. after a map load / delete)
        if(!patches.inrange(patchgroup[i].patch) || !patches[patchgroup[i].patch]->ctrl.inrange(patchgroup[i].cp))
            patchgroup.remove(i);
    patchhover = patchhovercp = patchhoversurf = -1;
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
    if(patchhover >= 0) return best;   // a control-point handle takes priority over the body

    // otherwise, which patch's surface (body) is under the crosshair -> used to select the whole patch
    loopv(patches)
    {
        bezpatch *p = patches[i];
        if(p->dirty) p->tessellate();
        if(p->tris.empty()) continue;
        vec bc; float br;
        p->boundsphere(bc, br);
        vec oc = vec(bc).sub(o);
        float proj = clamp(oc.dot(ray), 0.0f, best);
        if(vec(ray).mul(proj).add(o).squaredist(bc) > br*br) continue;
        for(int j = 0; j+2 < p->tris.length(); j += 3)
        {
            float t;
            if(raytri(o, ray, p->verts[p->tris[j]], p->verts[p->tris[j+1]], p->verts[p->tris[j+2]], t) && t < best)
            {
                best = t;
                patchhoversurf = i;
                patchsurfnormal = p->norms[p->tris[j]];
            }
        }
    }
    return best;
}

// editing a patch's geometry invalidates its baked lightmap -> drop it (falls back to forward lighting
// until /calclight re-bakes). UV/texture changes keep the lightmap.
static void invalidatepatch(bezpatch *p) { p->dirty = true; p->freelm(); }

// move all selected control points by a delta in the R[d]/C[d] plane (called when dragging an entity too,
// so a mixed entity+control-point selection moves together)
void movepatchgroup(int d, float dr, float dc)
{
    loopv(patchgroup)
    {
        patchcpsel &s = patchgroup[i];
        if(!patches.inrange(s.patch) || !patches[s.patch]->ctrl.inrange(s.cp)) continue;
        vec &cc = patches[s.patch]->ctrl[s.cp];
        cc[R[d]] += dr;
        cc[C[d]] += dc;
        invalidatepatch(patches[s.patch]);
    }
}

// move the whole selected control-point group by the delta of the focus point (the just-clicked one),
// within the plane of its grabbed box face -- the same scheme entdrag() uses for a group of entities.
void patchdrag(const vec &ray)
{
    if(patchgroup.empty()) { patchmoving = 0; return; }
    patchcpsel f = patchgroup.last();   // focus = last-added (just-clicked) control point
    if(!patches.inrange(f.patch) || !patches[f.patch]->ctrl.inrange(f.cp)) { patchmoving = 0; return; }
    vec &c = patches[f.patch]->ctrl[f.cp];
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
    float dr = (entselsnap ? g[R[d]] : dest[R[d]]) - c[R[d]];   // delta from the focus point
    float dcl = (entselsnap ? g[C[d]] : dest[C[d]]) - c[C[d]];
    patchmoving = 2;
    if(dr == 0 && dcl == 0) return;
    movepatchgroup(d, dr, dcl);   // move selected control points...
    moveentgroup(d, dr, dcl);     // ...and any selected entities, together
}

// called from rendereditcursor while a drag is in progress
void editpatches(const vec &ray)
{
    if(patchmoving) patchdrag(ray);
}

static void drawpatchnet(bezpatch *p)
{
    gle::begin(GL_LINES);
    loop(y, p->rows) loop(x, p->cols)
    {
        if(x+1 < p->cols) { gle::attrib(p->cp(x, y)); gle::attrib(p->cp(x+1, y)); }
        if(y+1 < p->rows) { gle::attrib(p->cp(x, y)); gle::attrib(p->cp(x, y+1)); }
    }
    xtraverts += gle::end();
}

void renderpatchhandles()
{
    gle::colorub(60, 120, 220);
    gle::defvertex();
    if(patchgroup.length())
    {
        // selection exists: draw the skeleton of every patch that has a selected control point
        loopv(patches)
        {
            bool sel = false;
            loopvj(patchgroup) if(patchgroup[j].patch == i) { sel = true; break; }
            if(sel) drawpatchnet(patches[i]);
        }
    }
    else if(patches.inrange(patchhover)) drawpatchnet(patches[patchhover]);            // none selected: preview the hovered CP's patch
    else if(patches.inrange(patchhoversurf)) drawpatchnet(patches[patchhoversurf]);    // ...or the patch body under the crosshair

    // markers are the blue PART_EDIT sparkles (renderparticles.cpp). Selected control points get a green
    // box; the hovered one gets the red grab-face -- same colours as entity selection.
    gle::colorub(0, 40, 0);
    loopv(patchgroup)
    {
        patchcpsel &s = patchgroup[i];
        if(!patches.inrange(s.patch) || !patches[s.patch]->ctrl.inrange(s.cp)) continue;
        vec eo, es;
        cpbox(patches[s.patch]->ctrl[s.cp], eo, es);
        boxs3D(eo, es, 1);
    }
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

// make the hovered control point the focus (last in the group). add=false replaces the selection with
// just it (unless it's already selected -> keep the group, so a drag moves the whole selection); add=true
// adds it to the selection. Then starts a drag. Mirrors how we want entities to behave too.
static void patchstartmove(bool add)
{
    if(noedit(true) || patchmoving) { if(patchmoving) return; patchmoving = 0; return; }
    if(patchhover >= 0)   // a control-point handle
    {
        if(!add)   // left click: this control point becomes the ONLY selection
        {
            patchgroup.setsize(0);
            clearentgroup();
            patchcpsel &s = patchgroup.add();
            s.patch = patchhover; s.cp = patchhovercp;
        }
        else       // right click: add to the selection (or just re-focus it if already selected)
        {
            int idx = patchgroupfind(patchhover, patchhovercp);
            if(idx >= 0) { patchcpsel s = patchgroup.remove(idx); patchgroup.add(s); }
            else { patchcpsel &s = patchgroup.add(); s.patch = patchhover; s.cp = patchhovercp; }
        }
    }
    else if(patches.inrange(patchhoversurf))   // the patch body: (left) select / (right) add ALL its control points
    {
        bezpatch *p = patches[patchhoversurf];
        if(!add) { patchgroup.setsize(0); clearentgroup(); }
        loopv(p->ctrl) if(patchgroupfind(patchhoversurf, i) < 0) { patchcpsel &s = patchgroup.add(); s.patch = patchhoversurf; s.cp = i; }
        // drag plane perpendicular to the surface normal at the hit (so the whole patch slides naturally)
        vec a(fabs(patchsurfnormal.x), fabs(patchsurfnormal.y), fabs(patchsurfnormal.z));
        patchorient = 2 * (a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2));
    }
    else { patchmoving = 0; return; }
    patchmoving = 1;
}

ICOMMAND(patchmoving,    "b", (int *n), { if(*n >= 0) { if(!*n) patchmoving = 0; else patchstartmove(false); } intret(patchmoving); });
ICOMMAND(patchmovingadd, "b", (int *n), { if(*n >= 0) { if(!*n) patchmoving = 0; else patchstartmove(true);  } intret(patchmoving); });

// active patch for ops/skeleton: the focus (last-selected) control point's patch, else the hovered, else last
static bezpatch *activepatch()
{
    if(patchgroup.length() && patches.inrange(patchgroup.last().patch)) return patches[patchgroup.last().patch];
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
    p->freelm();
    conoutf("patch grid %dx%d", p->cols, p->rows);
});

// set one control point explicitly (scripting + basis for fix/align commands)
ICOMMAND(patchcp, "iifff", (int *patch, int *cp, float *x, float *y, float *z),
{
    if(noedit(true)) return;
    if(!patches.inrange(*patch) || !patches[*patch]->ctrl.inrange(*cp)) return;
    patches[*patch]->ctrl[*cp] = vec(*x, *y, *z);
    patches[*patch]->dirty = true;
    patches[*patch]->freelm();
});

// set a patch's texture/material vslot directly (scripting/testing)
ICOMMAND(patchsetvslot, "ii", (int *patch, int *vslot),
{
    if(noedit(true)) return;
    if(!patches.inrange(*patch)) return;
    patches[*patch]->vslot = *vslot;
    patches[*patch]->dirty = true;   // texture change keeps the lightmap (lighting is independent of diffuse)
});

// move a control point by a delta (scripting/testing)
ICOMMAND(patchnudge, "iifff", (int *patch, int *cp, float *dx, float *dy, float *dz),
{
    if(noedit(true)) return;
    if(!patches.inrange(*patch) || !patches[*patch]->ctrl.inrange(*cp)) return;
    patches[*patch]->ctrl[*cp].add(vec(*dx, *dy, *dz));
    patches[*patch]->dirty = true;
    patches[*patch]->freelm();
});

// ---- texture alignment: recompute the stored control-point UVs (GtkRadiant-style) ----------------

// project from the patch's dominant axis-aligned plane, matching a coplanar cube face (slot scale/rotation)
void projectpatchuv_axis(bezpatch *p)
{
    if(p->dirty) p->tessellate();
    int dim = patchdim(p);
    vec4 sgen, tgen;
    calctexgen(lookupvslot(p->vslot, false), dim, sgen, tgen);
    loop(y, p->rows) loop(x, p->cols)
    {
        const vec &c = p->cp(x, y);
        p->uv(x, y) = vec2(sgen.x*c.x + sgen.y*c.y + sgen.z*c.z + sgen.w,
                           tgen.x*c.x + tgen.y*c.y + tgen.z*c.z + tgen.w);
    }
    p->dirty = true;
}

// natural: arc length along the control net, at the slot's texel scale (good for curved patches)
static void naturalpatchuv(bezpatch *p)
{
    VSlot &vs = lookupvslot(p->vslot, false);
    Texture *tex = vs.slot && vs.slot->sts.inrange(0) ? vs.slot->sts[0].t : notexture;
    float sc = vs.scale > 0 ? vs.scale : 1;
    float sk = (TEX_SCALE/sc)/max(tex->xs, 1), tk = (TEX_SCALE/sc)/max(tex->ys, 1);
    loop(y, p->rows) { float s = 0; loop(x, p->cols) { if(x) s += p->cp(x, y).dist(p->cp(x-1, y)); p->uv(x, y).x = s*sk; } }
    loop(x, p->cols) { float t = 0; loop(y, p->rows) { if(y) t += p->cp(x, y).dist(p->cp(x, y-1)); p->uv(x, y).y = t*tk; } }
    p->dirty = true;
}

ICOMMAND(patchprojaxis, "", (), { if(noedit(true)) return; bezpatch *p = activepatch(); if(p) projectpatchuv_axis(p); });
ICOMMAND(patchnaturalize, "", (), { if(noedit(true)) return; bezpatch *p = activepatch(); if(p) naturalpatchuv(p); });
ICOMMAND(patchfit, "ff", (float *tu, float *tv),
{
    if(noedit(true)) return;
    bezpatch *p = activepatch();
    if(!p) return;
    float su = *tu > 0 ? *tu : 1;
    float sv = *tv > 0 ? *tv : 1;
    loop(y, p->rows) loop(x, p->cols)
        p->uv(x, y) = vec2(su*x/max(p->cols-1, 1), sv*y/max(p->rows-1, 1));
    p->dirty = true;
});

// ---- geometry fix commands ----------------------------------------------------------------------

// rotate the hovered patch 90 degrees about the axis of the control-point box face under the crosshair
// (R + mouse wheel). The rotation happens in the plane perpendicular to that face normal. Returns 1 if it
// rotated a patch so the edit binding can fall through to cube/entity rotation otherwise.
ICOMMAND(patchrotate, "i", (int *dir),
{
    if(noedit(true) || !patches.inrange(patchhover)) { intret(0); return; }
    bezpatch *p = patches[patchhover];
    int d = dimension(patchorient);
    int a0 = (d+1)%3;
    int a1 = (d+2)%3;
    vec c(0, 0, 0);
    loopv(p->ctrl) c.add(p->ctrl[i]);
    c.div(p->ctrl.length());
    loopv(p->ctrl)
    {
        vec r = vec(p->ctrl[i]).sub(c);
        float r0 = r[a0];
        float r1 = r[a1];
        if(*dir > 0) { r[a0] = -r1; r[a1] = r0; } else { r[a0] = r1; r[a1] = -r0; }
        p->ctrl[i] = vec(c).add(r);
    }
    invalidatepatch(p);
    intret(1);
});

// flip the surface normals (reverse the u parametric direction; keeps UVs aligned)
ICOMMAND(patchinvert, "", (),
{
    if(noedit(true)) return;
    bezpatch *p = activepatch();
    if(!p) return;
    loop(y, p->rows) loop(x, p->cols/2)
    {
        swap(p->cp(x, y), p->cp(p->cols-1-x, y));
        swap(p->uv(x, y), p->uv(p->cols-1-x, y));
    }
    invalidatepatch(p);
});

// even out control-point spacing: each sub-patch midpoint moves to the average of its endpoints
ICOMMAND(patchredisperse, "", (),
{
    if(noedit(true)) return;
    bezpatch *p = activepatch();
    if(!p) return;
    for(int x = 1; x < p->cols; x += 2) loop(y, p->rows)
        p->cp(x, y) = vec(p->cp(x-1, y)).add(p->cp(x+1, y)).mul(0.5f);
    for(int y = 1; y < p->rows; y += 2) loop(x, p->cols)
        p->cp(x, y) = vec(p->cp(x, y-1)).add(p->cp(x, y+1)).mul(0.5f);
    invalidatepatch(p);
});

// ---- delete / copy / paste ----------------------------------------------------------------------

// delete the patch whose control point is under the crosshair; returns 1 if it deleted one (so the edit
// DEL binding can fall through to cube/entity deletion otherwise). Bound via editdel in stdedit.cfg.
ICOMMAND(delpatch, "", (),
{
    if(noedit(true)) { intret(0); return; }
    // distinct patches that have a selected control point; else the hovered patch
    vector<bezpatch *> del;
    loopv(patchgroup) if(patches.inrange(patchgroup[i].patch) && del.find(patches[patchgroup[i].patch]) < 0) del.add(patches[patchgroup[i].patch]);
    if(del.empty() && patches.inrange(patchhover)) del.add(patches[patchhover]);
    if(del.empty()) { intret(0); return; }
    loopv(del) { int idx = patches.find(del[i]); if(idx >= 0) { del[i]->freelm(); delete patches.remove(idx); } }
    patchgroup.setsize(0);
    patchhover = patchhovercp = -1;
    patchmoving = 0;
    intret(1);
});

// 1 when a patch control point is under the crosshair (so the edit copy/paste/del binds target the patch)
ICOMMAND(patchhovering, "", (), { intret(patches.inrange(patchhover) ? 1 : 0); });

static bezpatch patchclipboard;
static bool haspatchclip = false;

ICOMMAND(patchcopy, "", (),
{
    if(noedit(true)) return;
    bezpatch *p = activepatch();
    if(!p) { conoutf("no patch"); return; }
    patchclipboard.setdims(p->cols, p->rows);
    patchclipboard.vslot = p->vslot;
    patchclipboard.tess = p->tess;
    loopv(p->ctrl) patchclipboard.ctrl[i] = p->ctrl[i];
    loopv(p->cpuv) patchclipboard.cpuv[i] = p->cpuv[i];
    haspatchclip = true;
    conoutf("copied patch (%dx%d)", p->cols, p->rows);
});

ICOMMAND(patchpaste, "", (),
{
    if(noedit(true) || !haspatchclip) return;
    bezpatch *p = new bezpatch;
    p->setdims(patchclipboard.cols, patchclipboard.rows);
    p->vslot = patchclipboard.vslot;
    p->tess = patchclipboard.tess;
    loopv(p->ctrl) p->ctrl[i] = patchclipboard.ctrl[i];
    loopv(p->cpuv) p->cpuv[i] = patchclipboard.cpuv[i];
    // translate so the patch's centre lands at the edit cursor (selected location)
    vec centroid(0, 0, 0);
    loopv(p->ctrl) centroid.add(p->ctrl[i]);
    centroid.div(p->ctrl.length());
    vec off = vec(sel.o).add(vec(sel.grid*0.5f, sel.grid*0.5f, sel.grid*0.5f)).sub(centroid);
    loopv(p->ctrl) p->ctrl[i].add(off);
    p->dirty = true;
    int idx = patches.length();
    patches.add(p);
    // select the whole pasted patch so it's active and can be dragged into place
    patchgroup.setsize(0);
    loopv(p->ctrl) { patchcpsel &s = patchgroup.add(); s.patch = idx; s.cp = i; }
    conoutf("pasted patch %d", idx);
});

#endif // !STANDALONE
