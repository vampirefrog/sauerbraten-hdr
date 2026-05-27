// bezpatch.cpp: Quake3-style bezier patches (clean-room implementation).
// Phase 1: data model, biquadratic tessellation, creation command, basic rendering.

#include "engine.h"

vector<bezpatch *> patches;

void clearpatches()
{
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
    verts.setsize(0); norms.setsize(0); tangents.setsize(0); tcs.setsize(0); tris.setsize(0);
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
    SETSHADER(bezpatch);

    // forward sun + ambient lighting (shared by all patches this frame), HDR-scaled into the fp16 buffer
    GLOBALPARAM(psundir, sunlightdir);
    GLOBALPARAMF(psun, sunlightcolor.x/255.0f*sunlightscale, sunlightcolor.y/255.0f*sunlightscale, sunlightcolor.z/255.0f*sunlightscale);
    GLOBALPARAMF(pambient, max(max(ambientcolor.x, skylightcolor.x)/255.0f, 0.15f),
                            max(max(ambientcolor.y, skylightcolor.y)/255.0f, 0.15f),
                            max(max(ambientcolor.z, skylightcolor.z)/255.0f, 0.15f));

    gle::defvertex();
    gle::defnormal();
    gle::deftangent(3);
    gle::deftexcoord0();

    loopv(patches)
    {
        bezpatch *p = patches[i];
        if(p->tris.empty()) continue;
        VSlot &vs = lookupvslot(p->vslot, true);
        Slot &s = *vs.slot;
        Texture *diffuse = s.sts.inrange(0) ? s.sts[0].t : notexture;
        Texture *norm = slottex(s, TEX_NORMAL), *glow = slottex(s, TEX_GLOW);
        glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, diffuse->id);
        glActiveTexture_(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, (norm ? norm : notexture)->id);
        glActiveTexture_(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, (glow ? glow : notexture)->id);
        glActiveTexture_(GL_TEXTURE0);
        GLOBALPARAMF(pcolor, vs.colorscale.x, vs.colorscale.y, vs.colorscale.z, 1.0f);
        GLOBALPARAMF(pbump, norm ? 1.0f : 0.0f);
        GLOBALPARAMF(pglow, glow ? 1.0f : 0.0f);
        float inv = 1.0f / (patchtexscale * (vs.scale > 0 ? vs.scale : 1));
        gle::begin(GL_TRIANGLES);
        loopvj(p->tris)
        {
            ushort idx = p->tris[j];
            gle::attrib(p->verts[idx]);
            gle::attrib(p->norms[idx]);
            gle::attrib(p->tangents[idx]);
            gle::attrib(vec2(p->tcs[idx]).mul(inv));
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
