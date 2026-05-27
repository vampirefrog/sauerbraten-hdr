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
    verts.setsize(0); norms.setsize(0); tcs.setsize(0); tris.setsize(0);
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
        tcs.add(vec2(gu, gv));
    }

    int stride = su+1;
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

// ---- creation -------------------------------------------------------------

VARP(patchtess, 1, 4, 16);   // default subdivision level for new patches

ICOMMAND(newpatch, "i", (int *size),
{
    if(noedit(true)) return;
    int s = *size > 0 ? *size : 32;
    vec center = vec(camdir).mul(48).add(camera1->o);   // in front of the camera
    bezpatch *p = new bezpatch;
    p->tess = patchtess;
    loop(y, 3) loop(x, 3)                                // flat horizontal 3x3 patch
        p->cp(x, y) = vec(center.x + (x-1)*0.5f*s, center.y + (y-1)*0.5f*s, center.z);
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

// ---- rendering (Phase 1: flat-shaded solid + control net in edit mode) -----

void renderpatches()
{
    if(patches.empty()) return;
    loopv(patches) if(patches[i]->dirty) patches[i]->tessellate();

    glDisable(GL_CULL_FACE);     // patches are two-sided until normals/culling settle in later phases
    glDisable(GL_BLEND);
    notextureshader->set();
    gle::defvertex();

    gle::colorub(150, 150, 160);
    loopv(patches)
    {
        bezpatch *p = patches[i];
        if(p->tris.empty()) continue;
        gle::begin(GL_TRIANGLES);
        loopvj(p->tris) gle::attrib(p->verts[p->tris[j]]);
        xtraverts += gle::end();
    }

    if(editmode)
    {
        // control net: lines between adjacent control points
        gle::colorub(60, 120, 220);
        loopv(patches)
        {
            bezpatch *p = patches[i];
            gle::begin(GL_LINES);
            loop(y, p->rows) loop(x, p->cols)
            {
                if(x+1 < p->cols) { gle::attrib(p->cp(x, y)); gle::attrib(p->cp(x+1, y)); }
                if(y+1 < p->rows) { gle::attrib(p->cp(x, y)); gle::attrib(p->cp(x, y+1)); }
            }
            xtraverts += gle::end();
        }
    }

    glEnable(GL_CULL_FACE);
}

#endif // !STANDALONE
