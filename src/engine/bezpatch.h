// bezpatch.h: Quake3-style bezier patches (clean-room implementation).
//
// A patch is a grid of (cols x rows) control points, both dimensions odd (3,5,7,...).
// It is composed of ((cols-1)/2) x ((rows-1)/2) biquadratic (3x3) Bezier sub-patches that
// share edge control points, exactly like the patches GtkRadiant edits for Quake3.
// The control mesh is tessellated into a triangle mesh for rendering, lightmapping and collision.

#ifndef __BEZPATCH_H__
#define __BEZPATCH_H__

struct bezpatch
{
    int cols, rows;          // control-point grid dimensions (odd, >=3)
    vector<vec> ctrl;        // cols*rows control points, row-major (index = y*cols + x)
    int vslot;               // texture/material slot index (as in cube.texture[])
    int tess;                // subdivision segments per sub-patch axis (>=1)

    // tessellated mesh, rebuilt lazily when dirty
    vector<vec> verts;       // positions
    vector<vec> norms;       // normals
    vector<vec> tangents;    // surface tangent (d/du), orthonormalized vs normal -- for normal mapping
    vector<vec2> tcs;        // natural (arc-length) texture coords, world units
    vector<vec2> lmtc;       // per-vertex lightmap coords (grid fraction 0..1)
    vector<ushort> tris;     // triangle indices into the per-vertex arrays
    bool dirty;

    uint lmtex[3];           // baked 3-basis RNM HDR lightmap textures (0 = unbaked -> forward lighting)

    bezpatch() : cols(0), rows(0), vslot(0), tess(4), dirty(true) { lmtex[0] = lmtex[1] = lmtex[2] = 0; setdims(3, 3); }

    vec &cp(int x, int y) { return ctrl[y*cols + x]; }
    const vec &cp(int x, int y) const { return ctrl[y*cols + x]; }

    void setdims(int c, int r)         // (re)size the control grid, preserving overlapping points
    {
        vector<vec> old; old.move(ctrl);
        int oc = cols, orr = rows;
        cols = c; rows = r;
        ctrl.setsize(0);
        loop(y, rows) loop(x, cols)
            ctrl.add(x < oc && y < orr ? old[y*oc + x] : vec(0, 0, 0));
        dirty = true;
    }

    int usub() const { return (cols-1)/2; }   // sub-patches along u
    int vsub() const { return (rows-1)/2; }    // sub-patches along v

    void evalsubpatch(int iu, int iv, float u, float v, vec &pos, vec &du, vec &dv) const;
    void tessellate();
    void boundsphere(vec &center, float &radius) const;
#ifndef STANDALONE
    void uploadlm(uchar *rgbe0, uchar *rgbe1, uchar *rgbe2, int gw, int gh);   // baked RNM basis -> fp16 textures
    void freelm();
#endif
};

extern vector<bezpatch *> patches;

extern void clearpatches();
extern void saveworldpatches(stream *f);   // self-describing section (leading count), version>=35
extern void loadworldpatches(stream *f);
#ifndef STANDALONE
extern void renderpatches();
extern void editpatches(const vec &ray);   // continue an in-progress control-point drag
extern float raypatchcp(const vec &o, const vec &ray);  // pick control point under crosshair (entity-style)
extern int patchmoving, patchhover, patchhovercp;
#endif

#endif
