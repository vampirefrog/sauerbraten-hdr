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

    // per-vertex tangent from the position/UV gradient (Lengyel), so the normal map lines up with
    // whatever UV mapping is stored. We accumulate BOTH T and B (UV-gradient bitangent) per triangle,
    // then orthonormalize T against N and flip T if cross(N, T) ends up opposite the gradient-derived
    // B -- without this handedness check the patch's effective bitangent mirrors the cube's for
    // odd UV windings, putting normal-map bumps and the specular peak on the wrong side. Matches
    // the cube's vtangent.w convention (orientation_bitangent.scalartriple(n, t) sign in
    // octarender.cpp:852) but folded into the stored tangent so the shader's `cross(N,T)` is canonical.
    vector<vec> bitangents;
    loopv(tangents) bitangents.add(vec(0, 0, 0));
    for(int t = 0; t+2 < tris.length(); t += 3)
    {
        int i0 = tris[t], i1 = tris[t+1], i2 = tris[t+2];
        vec e1 = vec(verts[i1]).sub(verts[i0]), e2 = vec(verts[i2]).sub(verts[i0]);
        vec2 d1 = vec2(tcs[i1]).sub(tcs[i0]), d2 = vec2(tcs[i2]).sub(tcs[i0]);
        float denom = d1.x*d2.y - d2.x*d1.y;
        if(denom > -1e-9f && denom < 1e-9f) continue;
        float inv = 1.0f/denom;
        vec tan = vec(e1).mul(d2.y).sub(vec(e2).mul(d1.y)).mul(inv);
        vec bit = vec(e2).mul(d1.x).sub(vec(e1).mul(d2.x)).mul(inv);
        tangents[i0].add(tan);   tangents[i1].add(tan);   tangents[i2].add(tan);
        bitangents[i0].add(bit); bitangents[i1].add(bit); bitangents[i2].add(bit);
    }
    loopv(tangents)
    {
        vec t = tangents[i];
        t.sub(vec(norms[i]).mul(norms[i].dot(t)));
        if(t.iszero()) { tangents[i] = vec(1, 0, 0); continue; }
        t.normalize();
        // cross(N, T) is what the shader uses as the bitangent. If that points opposite the
        // gradient-derived bitangent for this triangle's UV winding, flip T so cross(N, T) lands
        // canonical -- otherwise normal-map green is mirrored and the spec peak shifts.
        vec cnt; cnt.cross(norms[i], t);
        if(cnt.dot(bitangents[i]) < 0) t.neg();
        tangents[i] = t;
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
        // v37+: baked RNM lightmap bytes (RGBE per basis). lmw=0 marks "not baked" -- writer must
        // also gate on lmrgbe[0] being non-null since freelm() clears the bytes on geometry edit.
        bool baked = p->lmw > 0 && p->lmh > 0 && p->lmrgbe[0] && p->lmrgbe[1] && p->lmrgbe[2];
        f->putlil<ushort>(baked ? p->lmw : 0);
        f->putlil<ushort>(baked ? p->lmh : 0);
        if(baked) loopk(3) f->write(p->lmrgbe[k], p->lmw*p->lmh*4);
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
        if(mapversion >= 37)
        {
            int lmw = f->getlil<ushort>(), lmh = f->getlil<ushort>();
            if(lmw > 0 && lmh > 0 && lmw <= 256 && lmh <= 256)   // sanity bound (tess*usub+1 with tess<=16, dims<=99 -> well under 256)
            {
                int bytes = lmw*lmh*4;
                uchar *rgbe[3] = { new uchar[bytes], new uchar[bytes], new uchar[bytes] };
                loopk(3) f->read(rgbe[k], bytes);
#ifndef STANDALONE
                // tessellate so the patch has the lightmap UVs ready before uploading textures
                p->tessellate();
                p->uploadlm(rgbe[0], rgbe[1], rgbe[2], lmw, lmh);
#else
                // server build: just keep the bytes in memory so a /savemap on the server preserves them
                loopk(3) { p->lmrgbe[k] = rgbe[k]; rgbe[k] = NULL; }
                p->lmw = lmw; p->lmh = lmh;
#endif
                loopk(3) if(rgbe[k]) delete[] rgbe[k];
            }
        }
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
    // also drop the RGBE source bytes -- the bake is no longer valid, so saving stale bytes
    // would re-load a lightmap that doesn't match the (now-edited) geometry.
    loopk(3) if(lmrgbe[k]) { delete[] lmrgbe[k]; lmrgbe[k] = NULL; }
    lmw = lmh = 0;
}

// decode the 3 baked RGBE basis buffers to linear fp16 and upload as 3 lightmap textures (RNM).
// We also keep a private copy of the source RGBE bytes so the bake survives /savemap, /reload, and
// future coop-edit sync (no need to /calclight again after loading a previously-baked map).
void bezpatch::uploadlm(uchar *rgbe0, uchar *rgbe1, uchar *rgbe2, int gw, int gh)
{
    uchar *srcs[3] = { rgbe0, rgbe1, rgbe2 };
    int bytes = gw*gh*4;
    // shrink/grow the kept buffers to the new size; the bake's resolution can change if patchtess does.
    loopk(3) if(lmrgbe[k]) { delete[] lmrgbe[k]; lmrgbe[k] = NULL; }
    lmw = gw; lmh = gh;
    float *fdata = new float[gw*gh*3];
    loopk(3)
    {
        if(!lmtex[k]) { GLuint id = 0; glGenTextures(1, &id); lmtex[k] = id; }
        loopi(gw*gh) { vec c = decodergbe(&srcs[k][i*4]); fdata[i*3] = c.x; fdata[i*3+1] = c.y; fdata[i*3+2] = c.z; }
        createtexture(lmtex[k], gw, gh, fdata, 1, 1, GL_RGB16F, GL_TEXTURE_2D, gw, gh, 0, false, GL_RGB, false);
        lmrgbe[k] = new uchar[bytes];
        memcpy(lmrgbe[k], srcs[k], bytes);
    }
    delete[] fdata;
}

// --- bake interface: raw-pointer accessors so the (fast-math) lightmap.cpp bake never touches the struct ---
int numpatches() { return patches.length(); }

#endif // !STANDALONE -- close the block above, exposing the coop-edit mp* helpers below to BOTH the
       // client and the standalone server build (the server maintains a mirror of the patches list).

// --- coop-edit sync hooks: serialize one patch's editable state into ints (matching the wire format
//     in client.cpp's patchedittrigger_upsert) so the game module can broadcast it; apply received
//     UPSERT/DELETE/CLEAR messages onto the local patches list without re-broadcasting.
//
//     Wire format: ctrl positions are int(coord * DMF) so we keep ~1/16 unit precision (same as
//     N_EDITENT); cpuv values are int(coord * 1000) for ~0.001 texel precision.
// ---
#define PATCH_NET_POS_SCALE 16.0f   // matches DMF in fpsgame/game.h
#define PATCH_NET_UV_SCALE  1000.0f

bool getpatchstate(int idx, int &cols, int &rows, int &vslot, int &tess, vector<int> &ctrlbuf, vector<int> &uvbuf)
{
    if(!patches.inrange(idx)) return false;
    bezpatch *p = patches[idx];
    cols = p->cols; rows = p->rows; vslot = p->vslot; tess = p->tess;
    int n = p->cols * p->rows;
    loopi(n)
    {
        ctrlbuf.add(int(p->ctrl[i].x * PATCH_NET_POS_SCALE));
        ctrlbuf.add(int(p->ctrl[i].y * PATCH_NET_POS_SCALE));
        ctrlbuf.add(int(p->ctrl[i].z * PATCH_NET_POS_SCALE));
    }
    loopi(n)
    {
        uvbuf.add(int(p->cpuv[i].x * PATCH_NET_UV_SCALE));
        uvbuf.add(int(p->cpuv[i].y * PATCH_NET_UV_SCALE));
    }
    return true;
}

// apply an UPSERT received over the network. If idx is past the end of patches, append; otherwise
// replace the patch in place (preserving the index so subsequent edits target the same slot).
// Drops the patch's baked lightmap because remote geometry might differ -- /calclight on any
// client re-bakes (lightmaps don't sync per-edit; that's done via savemap or a future bake msg).
void mppatchupsert(int idx, int cols, int rows, int vslot, int tess, const int *ctrl, int nctrl, const int *cpuv, int nuv)
{
    if(idx < 0 || idx > 0xFFFF) return;
    if(cols < 3 || rows < 3 || cols > 99 || rows > 99 || !(cols&1) || !(rows&1)) return;
    int n = cols * rows;
    if(nctrl != 3*n || nuv != 2*n) return;
    while(patches.length() <= idx) patches.add(new bezpatch);   // pad-create if needed
    bezpatch *p = patches[idx];
#ifndef STANDALONE
    p->freelm();   // any local lightmap is now stale
#endif
    p->setdims(cols, rows);
    p->vslot = vslot;
    p->tess = clamp(tess, 1, 16);
    loopi(n) p->ctrl[i] = vec(ctrl[3*i]/PATCH_NET_POS_SCALE, ctrl[3*i+1]/PATCH_NET_POS_SCALE, ctrl[3*i+2]/PATCH_NET_POS_SCALE);
    loopi(n) p->cpuv[i] = vec2(cpuv[2*i]/PATCH_NET_UV_SCALE, cpuv[2*i+1]/PATCH_NET_UV_SCALE);
    p->dirty = true;
}

#ifndef STANDALONE
// forward decls -- patchcpsel/patchgroup are defined further down for the editor; mppatchdelete
// needs them to update selections/hover state when a remote delete shifts indices.
struct patchcpsel;
extern vector<patchcpsel> patchgroup;
extern int patchhover, patchhovercp, patchhoversurf;
static void patchadjustselections_for_remove(int removedidx);
#endif

void mppatchdelete(int idx)
{
    if(!patches.inrange(idx)) return;
#ifndef STANDALONE
    patches[idx]->freelm();
#endif
    delete patches.remove(idx);
#ifndef STANDALONE
    patchadjustselections_for_remove(idx);
#endif
}

void mppatchclear() { clearpatches(); }

#ifndef STANDALONE   // re-open the editor-only section that starts above with `#ifndef STANDALONE`

// game-module hooks for coop-edit broadcast (real impl in fpsgame/client.cpp, no-op stubs in
// engine/serverengine.cpp). Each local patch edit calls one of these so other clients receive
// the change via N_EDITPATCH.
namespace game
{
    extern void patchedittrigger_upsert(int idx);
    extern void patchedittrigger_delete(int idx);
    extern void patchedittrigger_clear();
}

// Re-broadcast the full patches list (clear + upsert each). Called after undo/redo since those
// can change any subset of patches and even count -- it's simpler than diffing.
static void rebroadcast_all_patches()
{
    game::patchedittrigger_clear();
    loopv(patches) game::patchedittrigger_upsert(i);
}

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

// when a remote delete (or any other op that pops a patch out of the array) lands, our local
// patchhover / patchhoversurf / patchgroup may still reference the removed slot or higher indices --
// drop or shift them so subsequent edits target the right patches.
static void patchadjustselections_for_remove(int removedidx)
{
    if(patchhover == removedidx) { patchhover = patchhovercp = -1; }
    else if(patchhover > removedidx) patchhover--;
    if(patchhoversurf == removedidx) patchhoversurf = -1;
    else if(patchhoversurf > removedidx) patchhoversurf--;
    for(int i = patchgroup.length(); --i >= 0; )
    {
        if(patchgroup[i].patch == removedidx) patchgroup.remove(i);
        else if(patchgroup[i].patch > removedidx) patchgroup[i].patch--;
    }
}

// ---- undo/redo ------------------------------------------------------------

// patchstate: the minimal editable description of one patch, snapshotted into the undo stack.
// Tessellated geometry (verts/norms/tcs/...) is RE-DERIVED via tessellate() after restore, so
// we don't pay for it here -- snapshot cost is ~150 bytes per patch for a 3x3 grid.
struct patchstate
{
    int rows, cols, vslot, tess;
    vector<vec> ctrl;
    vector<vec2> cpuv;
};
struct patchundoentry { vector<patchstate> patches; int timestamp; };
static vector<patchundoentry *> patchundos, patchredos;
static const int PATCH_MAX_UNDOS = 256;

static patchundoentry *patchsnapshot()
{
    patchundoentry *e = new patchundoentry;
    e->timestamp = totalmillis;
    loopv(patches)
    {
        bezpatch *p = patches[i];
        patchstate &s = e->patches.add();
        s.rows = p->rows; s.cols = p->cols; s.vslot = p->vslot; s.tess = p->tess;
        loopvj(p->ctrl) s.ctrl.add(p->ctrl[j]);
        loopvj(p->cpuv) s.cpuv.add(p->cpuv[j]);
    }
    return e;
}

// snapshot the current patches state into the undo stack before mutating. Clear the redo stack so
// a fresh edit after a sequence of undos discards the future (standard undo semantics).
void patchmakeundo()
{
    patchundoentry *e = patchsnapshot();
    patchundos.add(e);
    while(!patchredos.empty()) delete patchredos.pop();
    while(patchundos.length() > PATCH_MAX_UNDOS) { delete patchundos[0]; patchundos.remove(0); }
}

static void patchrestore(patchundoentry *e)
{
    // rebuild patches[] from the snapshot. clears any selection that references a now-stale patch
    // (the snapshot may have a different count than the current state).
    clearpatches();
    patchgroup.setsize(0);
    loopv(e->patches)
    {
        const patchstate &s = e->patches[i];
        bezpatch *p = new bezpatch;
        p->setdims(s.cols, s.rows);
        p->vslot = s.vslot;
        p->tess = s.tess;
        loopvj(s.ctrl) if(p->ctrl.inrange(j)) p->ctrl[j] = s.ctrl[j];
        p->cpuv.setsize(0);
        loopvj(s.cpuv) p->cpuv.add(s.cpuv[j]);
        p->dirty = true;
        patches.add(p);
    }
}

// move the most-recent entry from `from` to `to` after swapping it with the current state.
// Returns true if an entry was popped. Mirrors the pattern of swapundo in octaedit.cpp.
static bool patchswapundo(vector<patchundoentry *> &from, vector<patchundoentry *> &to)
{
    if(from.empty()) return false;
    patchundoentry *cur = patchsnapshot();
    to.add(cur);
    patchundoentry *e = from.pop();
    patchrestore(e);
    delete e;
    return true;
}

bool patchundo_pop() { bool r = patchswapundo(patchundos, patchredos); if(r) rebroadcast_all_patches(); return r; }
bool patchredo_pop() { bool r = patchswapundo(patchredos, patchundos); if(r) rebroadcast_all_patches(); return r; }

// timestamp of the most-recent patch undo entry, or -1 if the stack is empty -- lets octaedit's
// editundo/editredo decide whether a patch edit or a cube edit happened more recently.
int patchundo_latest_ts() { return patchundos.empty() ? -1 : patchundos.last()->timestamp; }
int patchredo_latest_ts() { return patchredos.empty() ? -1 : patchredos.last()->timestamp; }

ICOMMAND(patchclearundos, "", (), {
    while(!patchundos.empty()) delete patchundos.pop();
    while(!patchredos.empty()) delete patchredos.pop();
});

// ---- creation -------------------------------------------------------------

VARP(patchtess, 1, 4, 16);   // default subdivision level for new patches

// Spawn a flat patch on the selected face of the selection volume, spanning the selection (or a
// default size centred on the face when nothing is selected), offset slightly outward along the
// face normal -- mirroring how dropentity() places a new entity (see world.cpp).
ICOMMAND(newpatch, "i", (int *size),
{
    if(noedit(true)) return;
    patchmakeundo();
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
    // texture inheritance: like cube-extrude/subdivide, take the underlying face's texture when it
    // is uniform across the selection; otherwise the default checkerboard (DEFAULT_GEOM, the engine's
    // canonical fallback) so a mixed-texture face spawns a clearly-unstyled patch the user can re-
    // texture deliberately. Require the candidate vslot to have at least one loaded sub-texture so
    // calctexgen()/the shader's tex0 sampler don't deref a null Texture*.
    extern int seluniformfacetex();
    extern vector<VSlot *> vslots;
    auto vslot_usable = [&](int idx) -> bool {
        return vslots.inrange(idx) && vslots[idx]->slot && !vslots[idx]->slot->sts.empty() && vslots[idx]->slot->sts[0].t;
    };
    int facetex = seluniformfacetex();
    if(vslot_usable(facetex)) p->vslot = facetex;
    else if(vslot_usable(DEFAULT_GEOM)) p->vslot = DEFAULT_GEOM;
    else p->vslot = currentedittex();
    loop(y, 3) loop(x, 3)
    {
        vec &cp = p->cp(x, y);
        cp[R[d]] = base[R[d]] + x*0.5f*er;
        cp[C[d]] = base[C[d]] + y*0.5f*ec;
        cp[D[d]] = dplane + off;
    }
    p->dirty = true;
    int newidx = patches.length();
    patches.add(p);
    extern void projectpatchuv_axis(bezpatch *p);
    projectpatchuv_axis(p);                     // default to axis projection (matches a coplanar cube face)
    conoutf("created patch %d (%d total)", patches.length()-1, patches.length());
    game::patchedittrigger_upsert(newidx);
});

ICOMMAND(clearpatches, "", (),
{
    if(noedit(true)) return;
    int n = patches.length();
    if(n) patchmakeundo();
    clearpatches();
    conoutf("cleared %d patches", n);
    game::patchedittrigger_clear();
});

ICOMMAND(patchcount, "", (), { conoutf("%d patches", patches.length()); intret(patches.length()); });

// scripting helpers (cheap; used by autoexec / debug scripts, not bound in defaults.cfg).
ICOMMAND(patchgetvslot, "i", (int *pi), { intret(patches.inrange(*pi) ? patches[*pi]->vslot : -1); });
ICOMMAND(patchgetcols,  "i", (int *pi), { intret(patches.inrange(*pi) ? patches[*pi]->cols  : 0);  });
ICOMMAND(patchgetrows,  "i", (int *pi), { intret(patches.inrange(*pi) ? patches[*pi]->rows  : 0);  });
ICOMMAND(patchgetcp,    "iii", (int *pi, int *cp, int *axis), {
    if(!patches.inrange(*pi) || !patches[*pi]->ctrl.inrange(*cp) || *axis < 0 || *axis > 2) { floatret(0); return; }
    floatret(patches[*pi]->ctrl[*cp][*axis]);
});
// debug/test helper: prime patchhover state so patchextend can be exercised without mouse input.
ICOMMAND(patchsethover, "iii", (int *pi, int *cp, int *orient), {
    patchhover = *pi; patchhovercp = *cp; patchorient = *orient;
});
ICOMMAND(applypatchtex, "i", (int *v), { extern void patchedittex(int); patchedittex(*v); });
ICOMMAND(patchselectall, "i", (int *pi), {
    if(!patches.inrange(*pi)) return;
    patchgroup.setsize(0);
    bezpatch *p = patches[*pi];
    loopv(p->ctrl) { patchcpsel s; s.patch = *pi; s.cp = i; patchgroup.add(s); }
});
// raycube against patches probe: returns distance from camera1->o along camera1->aim() to the
// nearest world (octree+patch) hit, capped at 4096. Lets autoexec scripts verify patches actually
// stop hitscans without spawning a real bullet.
ICOMMAND(patchraytest, "", (), {
    extern physent *camera1;
    extern float raycube(const vec &o, const vec &ray, float radius, int mode, int size, extentity *t);
    vec dir;
    vecfromyawpitch(camera1->yaw, camera1->pitch, 1, 0, dir);
    float d = raycube(camera1->o, dir, 4096.0f, RAY_CLIPMAT|RAY_ALPHAPOLY, 0, NULL);
    conoutf("raycube dist=%.2f", d);
    floatret(d);
});

// exposed so octaedit.cpp's edittex/edittex_ can route around the cube-selection codepath when patches
// are the focus (avoids retexturing a stale cube selection and the "selection not in view" gate).
bool haspatchsel() { return !patchgroup.empty(); }

// the vslot used by the first selected patch (or -1 if none). edittex_ snaps curtexindex onto this
// before advancing so the wheel moves AWAY from the patch's current texture -- same intent as gettex
// for cubes (otherwise the scroll lands on texmru[0] which is often the patch's existing texture).
int patchfirstvslot()
{
    loopv(patchgroup)
    {
        int pi = patchgroup[i].patch;
        if(patches.inrange(pi)) return patches[pi]->vslot;
    }
    return -1;
}

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

    // psun/pambient/psundir for the unbaked path are set per-patch inside the loop below.

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
        Texture *norm = slottex(s, TEX_NORMAL), *glow = slottex(s, TEX_GLOW);
        // texmask reflects the slot's textures including ones combined into siblings (Sauer packs
        // spec into the diffuse's alpha and depth into the normal map's alpha at load time, so
        // slottex(_, TEX_SPEC) returns NULL even when the slot HAS a spec map). The world shader
        // detects spec via the "S" build option which is just `texmask & (1<<TEX_SPEC)` -- mirror
        // that here so patches get the same spec contribution as cubes for combined-slot textures.
        bool hasspec = (s.texmask & (1<<TEX_SPEC)) != 0;
        bool baked = p->lmtex[0] != 0;
        // IMPORTANT: SETSHADER's flushparams() pushes the CURRENT global-param state to the new shader's
        // uniforms; any GLOBALPARAMF call AFTER SETSHADER won't reach the shader until the next bind.
        // So compute and set all our globals first, THEN SETSHADER, THEN bind textures and draw.
        GLOBALPARAMF(pcolor, 2*vs.colorscale.x, 2*vs.colorscale.y, 2*vs.colorscale.z, 1.0f);   // 2x overbright, like world surfaces
        GLOBALPARAMF(pbump, norm ? 1.0f : 0.0f);
        GLOBALPARAMF(pglow, glow ? 1.0f : 0.0f);
        // LDR-mode parity: RNM bakes the cos-modulated radiance per basis (HDR storage), so patches
        // come out ~1.5x brighter than the LDR cubes for the same scene. The cube path caps lmc at
        // 1.0 storage then multiplies by dot(lmlv,bump) at render; we don't have lmlv so just scale
        // the reconstructed lm. Empirical: pixel-diff against a cube-at-same-position reference (see
        // hdrhome/screenshot/reference_specular_patch.png) gave a ~1.5x ratio for this map.
        extern int hdrlightmaps;
        // empirical fit: RNM-baked patch lm is ~1.2x the cube's LDR-clamped lm*dot(lmlv,bump) after
        // we synthesize lmlv from the basis maps in the shader. Without the synthesis it was ~1.5x.
        // See hdrhome/screenshot/{reference,current,diff}_specular_patch.png for the empirical fit.
        GLOBALPARAMF(pldrscale, hdrlightmaps ? 1.0f : 0.95f);
        // psun reaches both shaders: bezpatch uses it for the diffuse-sun term, bezpatchlm uses it to
        // scale the spec peak up to cube-LDR brightness (RNM's `lm` blend dims it). Plain 1x sun colour
        // here; bezpatchlm applies its own overbright factor.
        GLOBALPARAM(psundir, sunlightdir);
        GLOBALPARAMF(psun, sunlightcolor.x/255.0f*sunlightscale,
                           sunlightcolor.y/255.0f*sunlightscale,
                           sunlightcolor.z/255.0f*sunlightscale);
        if(baked)
        {
            GLOBALPARAMF(pspec, hasspec ? 1.0f : 0.0f);
            bool height = (s.texmask & (1<<TEX_DEPTH)) != 0;
            GLOBALPARAMF(pparallax, height ? 0.06f : 0.0f, height ? -0.03f : 0.0f);
        }
        else
        {
            // unbaked fallback (forward bezpatch shader): smooth sun + ambient.
            float sx = sunlightcolor.x/255.0f*sunlightscale,
                  sy = sunlightcolor.y/255.0f*sunlightscale,
                  sz = sunlightcolor.z/255.0f*sunlightscale;
            // ambient floor of 0.25: the world-shader path uses ambientcolor directly (typically dark);
            // for unbaked patches we lift it so they're not invisible until calclight, but don't go so
            // bright that they look out-of-place. A baked patch will replace this with proper RNM math.
            float ax = max(max(ambientcolor.x, skylightcolor.x)/255.0f, 0.25f),
                  ay = max(max(ambientcolor.y, skylightcolor.y)/255.0f, 0.25f),
                  az = max(max(ambientcolor.z, skylightcolor.z)/255.0f, 0.25f);
            if(haslightprobes())
            {
                // sample the baked probe grid at the patch centre, lifted off the surface (like
                // bakemapmodelprobes does for items) so we don't hit a buried/solid grid cell.
                vec center; float r;
                p->boundsphere(center, r);
                vec n(0, 0, 0);
                loopvj(p->norms) n.add(p->norms[j]);
                if(!n.iszero()) { n.normalize(); center.add(vec(n).mul(4.0f)); } else center.z += 4.0f;
                vec faces[6];
                getprobecube(center, faces);
                vec avg(0, 0, 0);
                loopk(6) avg.add(faces[k]);
                avg.div(6*255.0f);
                ax = avg.x; ay = avg.y; az = avg.z;
                sx = sy = sz = 0;   // the probe already contains the baked sun + sky + bounce
            }
            // psundir/psun set above (used by both shaders); re-set psun here only if the probe
            // path overrode them to 0 to avoid double-counting baked sun.
            if(haslightprobes()) GLOBALPARAMF(psun, sx, sy, sz);
            GLOBALPARAMF(pambient, ax, ay, az);
        }
        if(baked) SETSHADER(bezpatchlm); else SETSHADER(bezpatch);
        glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, diffuse->id);
        glActiveTexture_(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, (norm ? norm : notexture)->id);
        glActiveTexture_(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, (glow ? glow : notexture)->id);
        if(baked)
        {
            loopk(3) { glActiveTexture_(GL_TEXTURE3+k); glBindTexture(GL_TEXTURE_2D, p->lmtex[k]); }
            // spec is packed into the diffuse texture's alpha channel by the engine's slot combine
            // (see VSlot::combinealpha); the patch shader reads it from tex0.a -- no separate sampler needed.
        }
        glActiveTexture_(GL_TEXTURE0);
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

// apply a vslot to every distinct patch in the current control-point selection (called from edittex
// in octaedit.cpp so Y+scroll and the F2 browser retexture selected patches alongside cube faces).
// re-projects UVs from the new slot and clears the patch's lightmap (texture change is a material change,
// so the bake needs to re-emit colour-modulated radiance).
void patchedittex(int vslot)
{
    if(patchgroup.empty()) return;
    // lookupvslot(_, true) loads/links the slot lazily, so a vslot that's known but not yet uploaded
    // here (rare -- typically a slot the user just touched) becomes usable before projectpatchuv_axis
    // calls calctexgen on it. Guard for a NULL slot or empty sts to avoid the crash that bit newpatch.
    extern vector<VSlot *> vslots;
    if(!vslots.inrange(vslot)) { conoutf(CON_ERROR, "patchedittex: vslot %d out of range", vslot); return; }
    VSlot &vs = lookupvslot(vslot, true);
    if(!vs.slot || vs.slot->sts.empty() || !vs.slot->sts[0].t)
    {
        conoutf(CON_ERROR, "patchedittex: vslot %d has no loaded texture", vslot);
        return;
    }
    patchmakeundo();
    vector<int> uniq;
    loopv(patchgroup)
    {
        int pi = patchgroup[i].patch;
        if(patches.inrange(pi) && uniq.find(pi) < 0) uniq.add(pi);
    }
    extern void projectpatchuv_axis(bezpatch *p);
    loopv(uniq)
    {
        bezpatch *p = patches[uniq[i]];
        p->vslot = vslot;
        invalidatepatch(p);
        projectpatchuv_axis(p);
    }
    if(uniq.length()) conoutf("patch tex applied to %d patch%s", uniq.length(), uniq.length()==1 ? "" : "es");
    loopv(uniq) game::patchedittrigger_upsert(uniq[i]);
}

// move all selected control points by a delta in the R[d]/C[d] plane (called when dragging an entity too,
// so a mixed entity+control-point selection moves together). Broadcasts every modified patch once per
// drag-frame -- matches how entity dragging syncs (per-frame N_EDITENT).
void movepatchgroup(int d, float dr, float dc)
{
    vector<int> uniq;
    loopv(patchgroup)
    {
        patchcpsel &s = patchgroup[i];
        if(!patches.inrange(s.patch) || !patches[s.patch]->ctrl.inrange(s.cp)) continue;
        vec &cc = patches[s.patch]->ctrl[s.cp];
        cc[R[d]] += dr;
        cc[C[d]] += dc;
        invalidatepatch(patches[s.patch]);
        if(uniq.find(s.patch) < 0) uniq.add(s.patch);
    }
    loopv(uniq) game::patchedittrigger_upsert(uniq[i]);
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
    // snapshot at drag start (not every frame during the drag) so U undoes the entire drag as one step.
    bool willmove = (patchhover >= 0) || patches.inrange(patchhoversurf);
    if(willmove) patchmakeundo();
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

// Extrude one edge of the patch by `delta` whole sub-patches (each = 2 control points), linearly
// extrapolating the new CPs from the existing edge so they land in the patch's plane instead of at
// world origin. `edge` is 0=-U (left), 1=+U (right), 2=-V (bottom), 3=+V (top). delta>0 grows,
// delta<0 shrinks (just drops outer CPs). Returns false if the result would fall outside [3..99].
static bool patchextrude(bezpatch *p, int edge, int delta)
{
    if(delta == 0) return false;
    int oc = p->cols, orr = p->rows;
    int nc = oc, nr = orr;
    if(edge < 2) nc = clamp(oc + 2*delta, 3, 99); else nr = clamp(orr + 2*delta, 3, 99);
    if(!(nc&1)) nc--;
    if(!(nr&1)) nr--;
    if(nc == oc && nr == orr) return false;

    // Snapshot the old control mesh so we can re-pack into the resized buffer with the right shift.
    vector<vec>  oldctrl; loopv(p->ctrl) oldctrl.add(p->ctrl[i]);
    vector<vec2> olduv;   loopv(p->cpuv) olduv.add(p->cpuv[i]);
    p->setdims(nc, nr);   // resizes ctrl/cpuv; we re-fill below from oldctrl.
    auto src   = [&](int x, int y) -> vec   { return oldctrl[y*oc + x]; };
    auto srcuv = [&](int x, int y) -> vec2  { return olduv.inrange(y*oc + x) ? olduv[y*oc + x] : vec2(0, 0); };

    // SHIFT: for the negative-side edges (-U, -V) growing means moving existing CPs to higher
    // indices (so the new ones land at x/y=0); for the same edges shrinking we DROP CPs from the
    // start instead -- so the existing CPs effectively shift to LOWER indices (the rest get cut).
    // We unify both with a single signed shift: dst[x,y] = src[x-xshift, y-yshift]. xshift>0 for
    // -U grow, xshift<0 for -U shrink, 0 for the +U side (growth is at the end, no shift).
    int xshift = 0, yshift = 0;
    if(edge == 0) xshift = nc  - oc;
    if(edge == 2) yshift = nr  - orr;

    // Copy region: every (x, y) in the new grid that has a valid source position (x - xshift,
    // y - yshift) within the old grid. max/min handle both positive and negative shifts safely
    // so we never index a negative slot.
    int xstart = max(0, xshift), xend = min(nc, oc + xshift);
    int ystart = max(0, yshift), yend = min(nr, orr + yshift);
    for(int y = ystart; y < yend; y++) for(int x = xstart; x < xend; x++)
    {
        p->cp(x, y) = src(x - xshift, y - yshift);
        if(p->cpuv.inrange(y*nc + x)) p->uv(x, y) = srcuv(x - xshift, y - yshift);
    }

    // Fill the freshly-grown CPs (only when delta>0; shrink has no new CPs to populate). For each
    // new column/row k beyond the old edge, new_cp = edge_cp + k * (edge_cp - inner_cp): a linear
    // extrapolation of the surface slope so a flat patch stays flat and a curved one continues
    // along its tangent. Users can fine-tune by dragging.
    if(delta > 0)
    {
        if(edge == 1)        // +U: new x in [oc..nc)
        {
            loop(y, orr)
            {
                vec  eedge = src(oc-1, y), einner = src(max(oc-2, 0), y);
                vec  d   = vec(eedge).sub(einner);
                vec2 uve = srcuv(oc-1, y), uvi    = srcuv(max(oc-2, 0), y);
                vec2 duv = vec2(uve).sub(uvi);
                for(int x = oc; x < nc; x++)
                {
                    int k = x - (oc-1);
                    p->cp(x, y) = vec(eedge).add(vec(d).mul(float(k)));
                    if(p->cpuv.inrange(y*nc + x)) p->uv(x, y) = vec2(uve).add(vec2(duv).mul(float(k)));
                }
            }
        }
        else if(edge == 0)   // -U: new x in [0..xshift); existing CPs already shifted into [xshift..nc)
        {
            loop(y, orr)
            {
                vec  eedge = src(0, y), einner = src(min(1, oc-1), y);
                vec  d   = vec(eedge).sub(einner);
                vec2 uve = srcuv(0, y), uvi    = srcuv(min(1, oc-1), y);
                vec2 duv = vec2(uve).sub(uvi);
                loop(x, xshift)
                {
                    int k = xshift - x;   // furthest-out CP gets the largest stride
                    p->cp(x, y) = vec(eedge).add(vec(d).mul(float(k)));
                    if(p->cpuv.inrange(y*nc + x)) p->uv(x, y) = vec2(uve).add(vec2(duv).mul(float(k)));
                }
            }
        }
        else if(edge == 3)   // +V
        {
            loop(x, oc)
            {
                vec  eedge = src(x, orr-1), einner = src(x, max(orr-2, 0));
                vec  d   = vec(eedge).sub(einner);
                vec2 uve = srcuv(x, orr-1), uvi    = srcuv(x, max(orr-2, 0));
                vec2 duv = vec2(uve).sub(uvi);
                for(int y = orr; y < nr; y++)
                {
                    int k = y - (orr-1);
                    p->cp(x, y) = vec(eedge).add(vec(d).mul(float(k)));
                    if(p->cpuv.inrange(y*nc + x)) p->uv(x, y) = vec2(uve).add(vec2(duv).mul(float(k)));
                }
            }
        }
        else if(edge == 2)   // -V
        {
            loop(x, oc)
            {
                vec  eedge = src(x, 0), einner = src(x, min(1, orr-1));
                vec  d   = vec(eedge).sub(einner);
                vec2 uve = srcuv(x, 0), uvi    = srcuv(x, min(1, orr-1));
                vec2 duv = vec2(uve).sub(uvi);
                loop(y, yshift)
                {
                    int k = yshift - y;
                    p->cp(x, y) = vec(eedge).add(vec(d).mul(float(k)));
                    if(p->cpuv.inrange(y*nc + x)) p->uv(x, y) = vec2(uve).add(vec2(duv).mul(float(k)));
                }
            }
        }
    }
    p->dirty = true;
    return true;
}

// Decide which edge a CP belongs to. Non-edge CPs return -1. Corners (on two edges) are
// disambiguated by the box face the cursor is on (patchorient): the world axis of that face's
// normal picks the edge whose direction most aligns with it.
static int patchedge_for_cp(bezpatch *p, int cpx, int cpy, int orient)
{
    bool left = (cpx == 0), right = (cpx == p->cols-1);
    bool bot  = (cpy == 0), top   = (cpy == p->rows-1);
    int hcount = (left ? 1 : 0) + (right ? 1 : 0) + (bot ? 1 : 0) + (top ? 1 : 0);
    if(hcount == 0) return -1;
    // Non-corner edge: only one of the four is true (or two from a thin 3xN patch -- we still
    // prefer U vs V by patchorient to be safe).
    int worldaxis = dimension(orient);
    // average U direction (across rows, ctrl[1,y] - ctrl[0,y]) and V direction
    vec udir(0, 0, 0), vdir(0, 0, 0);
    int us = 0, vs = 0;
    if(p->cols >= 2) loop(y, p->rows) { udir.add(vec(p->cp(p->cols-1, y)).sub(p->cp(0, y))); us++; }
    if(p->rows >= 2) loop(x, p->cols) { vdir.add(vec(p->cp(x, p->rows-1)).sub(p->cp(x, 0))); vs++; }
    if(us) udir.div(us);
    if(vs) vdir.div(vs);
    float ua = fabs(udir[worldaxis]), va = fabs(vdir[worldaxis]);
    bool preferU = ua >= va;
    if(left && right) preferU = true;     // 3-wide patch, no real -U/+U distinction; pick one
    if(bot  && top)   preferU = false;
    if(preferU)
    {
        if(left)  return 0;
        if(right) return 1;
    }
    if(bot)  return 2;
    if(top)  return 3;
    if(left) return 0;
    if(right) return 1;
    return -1;
}

ICOMMAND(patchextend, "i", (int *dir),
{
    if(noedit(true)) { intret(0); return; }
    if(!patches.inrange(patchhover) || patchhovercp < 0) { intret(0); return; }
    bezpatch *p = patches[patchhover];
    int cpx = patchhovercp % p->cols;
    int cpy = patchhovercp / p->cols;
    int edge = patchedge_for_cp(p, cpx, cpy, patchorient);
    if(edge < 0) { intret(0); return; }     // not an edge CP -- let the caller fall through to cube edit
    // The cursor is on an edge CP: the scroll is "ours" even if we can't actually grow/shrink
    // (e.g. shrink at 3x3 minimum, grow at 99x99 max). Return 1 unconditionally so the cube edit
    // path doesn't run too.
    // Invert wheel direction: scroll-DOWN (`delta_do -1`) extends the patch, scroll-UP shrinks.
    // defaults.cfg: WheelUp -> `delta_do 1`, WheelDown -> `delta_do -1`, so we want
    // dir<0 -> grow and dir>0 -> shrink.
    int delta = *dir > 0 ? -1 : 1;
    patchmakeundo();                   // snapshot pre-change so U restores; harmless no-op snapshot at limits
    if(patchextrude(p, edge, delta))
    {
        p->freelm();
        game::patchedittrigger_upsert(patches.find(p));
    }
    else if(delta > 0) conoutf("patch already at max size (99x99)");
    else conoutf("patch already at min size (3x3)");
    intret(1);
});

// grow/shrink by whole biquadratic sub-patches (+/-2 control points per axis), Quake3/Radiant style.
// Uses the smart extrude so new CPs land in line with the existing edge instead of at world origin.
ICOMMAND(patchgrow, "ii", (int *du, int *dv),
{
    if(noedit(true)) return;
    bezpatch *p = activepatch();
    if(!p) { conoutf("no patch"); return; }
    patchmakeundo();
    if(*du) patchextrude(p, 1, *du);   // +U side
    if(*dv) patchextrude(p, 3, *dv);   // +V side
    p->freelm();
    conoutf("patch grid %dx%d", p->cols, p->rows);
    game::patchedittrigger_upsert(patches.find(p));
});

// set one control point explicitly (scripting + basis for fix/align commands)
ICOMMAND(patchcp, "iifff", (int *patch, int *cp, float *x, float *y, float *z),
{
    if(noedit(true)) return;
    if(!patches.inrange(*patch) || !patches[*patch]->ctrl.inrange(*cp)) return;
    patchmakeundo();
    patches[*patch]->ctrl[*cp] = vec(*x, *y, *z);
    patches[*patch]->dirty = true;
    patches[*patch]->freelm();
    game::patchedittrigger_upsert(*patch);
});

// set a patch's texture/material vslot directly (scripting/testing)
ICOMMAND(patchsetvslot, "ii", (int *patch, int *vslot),
{
    if(noedit(true)) return;
    if(!patches.inrange(*patch)) return;
    patchmakeundo();
    patches[*patch]->vslot = *vslot;
    patches[*patch]->dirty = true;   // texture change keeps the lightmap (lighting is independent of diffuse)
    game::patchedittrigger_upsert(*patch);
});

// move a control point by a delta (scripting/testing)
ICOMMAND(patchnudge, "iifff", (int *patch, int *cp, float *dx, float *dy, float *dz),
{
    if(noedit(true)) return;
    if(!patches.inrange(*patch) || !patches[*patch]->ctrl.inrange(*cp)) return;
    patchmakeundo();
    patches[*patch]->ctrl[*cp].add(vec(*dx, *dy, *dz));
    patches[*patch]->dirty = true;
    patches[*patch]->freelm();
    game::patchedittrigger_upsert(*patch);
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

ICOMMAND(patchprojaxis, "", (), { if(noedit(true)) return; bezpatch *p = activepatch(); if(p) { patchmakeundo(); projectpatchuv_axis(p); game::patchedittrigger_upsert(patches.find(p)); } });
ICOMMAND(patchnaturalize, "", (), { if(noedit(true)) return; bezpatch *p = activepatch(); if(p) { patchmakeundo(); naturalpatchuv(p); game::patchedittrigger_upsert(patches.find(p)); } });
ICOMMAND(patchfit, "ff", (float *tu, float *tv),
{
    if(noedit(true)) return;
    bezpatch *p = activepatch();
    if(!p) return;
    patchmakeundo();
    float su = *tu > 0 ? *tu : 1;
    float sv = *tv > 0 ? *tv : 1;
    loop(y, p->rows) loop(x, p->cols)
        p->uv(x, y) = vec2(su*x/max(p->cols-1, 1), sv*y/max(p->rows-1, 1));
    p->dirty = true;
    game::patchedittrigger_upsert(patches.find(p));
});

// ---- geometry fix commands ----------------------------------------------------------------------

// rotate the hovered patch 90 degrees (R + mouse wheel), in the plane perpendicular to the face normal.
// Pointing at a control-point box face -> pivot about that control point, axis = its box face. Pointing at
// the patch body -> pivot about the patch centre, axis = the surface normal. Returns 1 if it rotated a patch
// so the edit binding can fall through to cube/entity rotation otherwise.
ICOMMAND(patchrotate, "i", (int *dir),
{
    if(noedit(true)) { intret(0); return; }
    bezpatch *p = NULL;
    int d = 2;
    vec c(0, 0, 0);
    if(patches.inrange(patchhover) && patches[patchhover]->ctrl.inrange(patchhovercp))
    {
        p = patches[patchhover];
        d = dimension(patchorient);
        c = p->ctrl[patchhovercp];                 // pivot about the control point under the crosshair
    }
    else if(patches.inrange(patchhoversurf))
    {
        p = patches[patchhoversurf];
        vec a(fabs(patchsurfnormal.x), fabs(patchsurfnormal.y), fabs(patchsurfnormal.z));
        d = a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);   // axis = surface normal's dominant axis
        loopv(p->ctrl) c.add(p->ctrl[i]);
        c.div(p->ctrl.length());                   // pivot about the patch centre
    }
    else { intret(0); return; }
    patchmakeundo();
    int a0 = (d+1)%3;
    int a1 = (d+2)%3;
    loopv(p->ctrl)
    {
        vec r = vec(p->ctrl[i]).sub(c);
        float r0 = r[a0];
        float r1 = r[a1];
        if(*dir > 0) { r[a0] = -r1; r[a1] = r0; } else { r[a0] = r1; r[a1] = -r0; }
        p->ctrl[i] = vec(c).add(r);
    }
    invalidatepatch(p);
    game::patchedittrigger_upsert(patches.find(p));
    intret(1);
});

// mirror the hovered patch across the plane through the crosshair-pointed control point box face.
// Pointing at a control-point face -> plane through the CP, perpendicular to the face's world-axis
// normal. Pointing at the patch body -> plane through the patch centre, perpendicular to the surface
// normal's dominant axis. Each CP's coordinate along that axis becomes (2*plane - coord). Reflection
// reverses the surface orientation; the tessellator's winding handles that automatically. Returns 1
// when it flipped a patch so the X edit binding can fall through to cube/entity flip otherwise.
ICOMMAND(patchflip, "", (),
{
    if(noedit(true)) { intret(0); return; }
    bezpatch *p = NULL;
    int d = 2;
    float pcoord = 0;
    if(patches.inrange(patchhover) && patches[patchhover]->ctrl.inrange(patchhovercp))
    {
        p = patches[patchhover];
        d = dimension(patchorient);
        pcoord = p->ctrl[patchhovercp][d];
    }
    else if(patches.inrange(patchhoversurf))
    {
        p = patches[patchhoversurf];
        vec a(fabs(patchsurfnormal.x), fabs(patchsurfnormal.y), fabs(patchsurfnormal.z));
        d = a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);
        // plane through patch centre on axis d
        loopv(p->ctrl) pcoord += p->ctrl[i][d];
        pcoord /= p->ctrl.length();
    }
    else { intret(0); return; }
    patchmakeundo();
    loopv(p->ctrl) p->ctrl[i][d] = 2.0f*pcoord - p->ctrl[i][d];
    invalidatepatch(p);
    game::patchedittrigger_upsert(patches.find(p));
    intret(1);
});

// flip the surface normals (reverse the u parametric direction; keeps UVs aligned)
ICOMMAND(patchinvert, "", (),
{
    if(noedit(true)) return;
    bezpatch *p = activepatch();
    if(!p) return;
    patchmakeundo();
    loop(y, p->rows) loop(x, p->cols/2)
    {
        swap(p->cp(x, y), p->cp(p->cols-1-x, y));
        swap(p->uv(x, y), p->uv(p->cols-1-x, y));
    }
    invalidatepatch(p);
    game::patchedittrigger_upsert(patches.find(p));
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
    game::patchedittrigger_upsert(patches.find(p));
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
    patchmakeundo();
    // collect indices BEFORE removal so we can broadcast deletes; broadcast in DESCENDING order so
    // each remote receiver applies them with the correct indices (they shift down as items are removed).
    vector<int> delidx;
    loopv(del) { int idx = patches.find(del[i]); if(idx >= 0) delidx.add(idx); }
    delidx.sort();   // ascending
    loopv(del) { int idx = patches.find(del[i]); if(idx >= 0) { del[i]->freelm(); delete patches.remove(idx); } }
    patchgroup.setsize(0);
    patchhover = patchhovercp = -1;
    patchmoving = 0;
    for(int i = delidx.length(); --i >= 0; ) game::patchedittrigger_delete(delidx[i]);
    intret(1);
});

// 1 when the C/V/DEL edit binds should target patches: either a control point is hovered, OR there
// are selected patch CPs (so copying with only a body-click selection works without the user having
// to keep the crosshair on a CP).
ICOMMAND(patchhovering, "", (), { intret((patches.inrange(patchhover) || !patchgroup.empty()) ? 1 : 0); });

// patch clipboard: a list of (cols, rows, vslot, tess, ctrl, cpuv) tuples so copying a multi-patch
// selection preserves their relative positions when pasted.
struct patchclipentry
{
    int cols, rows, vslot, tess;
    vector<vec> ctrl;
    vector<vec2> cpuv;
};
static vector<patchclipentry> patchclipboard;

static void copy_patch_into_clipboard(bezpatch *p)
{
    patchclipentry &e = patchclipboard.add();
    e.cols = p->cols; e.rows = p->rows; e.vslot = p->vslot; e.tess = p->tess;
    loopv(p->ctrl) e.ctrl.add(p->ctrl[i]);
    loopv(p->cpuv) e.cpuv.add(p->cpuv[i]);
}

ICOMMAND(patchcopy, "", (),
{
    if(noedit(true)) return;
    patchclipboard.shrink(0);
    // Selected patch CPs win: collect every distinct patch that has at least one selected CP.
    // No selection? Fall back to the hovered/active patch so the legacy hover-and-C workflow still works.
    vector<int> uniq;
    loopv(patchgroup) if(patches.inrange(patchgroup[i].patch) && uniq.find(patchgroup[i].patch) < 0) uniq.add(patchgroup[i].patch);
    if(uniq.empty()) { bezpatch *p = activepatch(); if(p) { copy_patch_into_clipboard(p); conoutf("copied patch (%dx%d)", p->cols, p->rows); return; } }
    if(uniq.empty()) { conoutf("no patch"); return; }
    loopv(uniq) copy_patch_into_clipboard(patches[uniq[i]]);
    conoutf("copied %d patch%s", patchclipboard.length(), patchclipboard.length()==1 ? "" : "es");
});

ICOMMAND(patchpaste, "", (),
{
    if(noedit(true) || patchclipboard.empty()) return;
    patchmakeundo();
    // Drop the pasted patch(es) onto the selected face of the selection volume, slightly offset
    // outward -- same scheme newpatch uses for fresh patches (and dropentity for entities). For
    // multi-patch clipboards the relative positions among the copies are preserved.
    int d = dimension(sel.orient);
    int dc = dimcoord(sel.orient);
    int g = sel.grid;
    vec spawnctr(sel.o);
    spawnctr[R[d]] += sel.s[R[d]]*g*0.5f;
    spawnctr[C[d]] += sel.s[C[d]]*g*0.5f;
    spawnctr[D[d]] = sel.o[D[d]] + (dc ? sel.s[D[d]]*g : 0);
    float off = (dc ? 1 : -1) * max(2.0f, g*0.5f);   // outward offset (same as newpatch)
    spawnctr[D[d]] += off;
    // combined centroid of all clipboard patches -> shift so the combined centre lands at spawnctr
    vec ccentroid(0, 0, 0);
    int total = 0;
    loopv(patchclipboard) { loopvj(patchclipboard[i].ctrl) { ccentroid.add(patchclipboard[i].ctrl[j]); total++; } }
    if(total) ccentroid.div(total);
    vec shift = vec(spawnctr).sub(ccentroid);
    patchgroup.setsize(0);
    loopv(patchclipboard)
    {
        patchclipentry &e = patchclipboard[i];
        bezpatch *p = new bezpatch;
        p->setdims(e.cols, e.rows);
        p->vslot = e.vslot;
        p->tess = e.tess;
        loopvj(e.ctrl)
        {
            if(!p->ctrl.inrange(j)) break;
            p->ctrl[j] = vec(e.ctrl[j]).add(shift);
        }
        p->cpuv.setsize(0);
        loopvj(e.cpuv) p->cpuv.add(e.cpuv[j]);
        p->dirty = true;
        int idx = patches.length();
        patches.add(p);
        // select every pasted patch's CPs so the result is immediately draggable as a group
        loopvj(p->ctrl) { patchcpsel &s = patchgroup.add(); s.patch = idx; s.cp = j; }
        game::patchedittrigger_upsert(idx);
    }
    conoutf("pasted %d patch%s", patchclipboard.length(), patchclipboard.length()==1 ? "" : "es");
});

#endif // !STANDALONE
