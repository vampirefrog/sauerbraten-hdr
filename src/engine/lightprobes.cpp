// lightprobes.cpp: a baked grid of ambient cubes for lighting dynamic models (mapmodels, players,
// items, particles), as in Valve's Source engine (Mitchell 2006, "Ambient Cube").
//
// Each probe stores 6 directional radiance samples (one per axis: +X,-X,+Y,-Y,+Z,-Z). The grid is
// baked at calclight time by gathering static lights/sun/skylight at each cell, and serialized into
// the .ogz (MAPVERSION 34). At render time lightreaching() trilinearly interpolates the surrounding
// probes instead of ray-tracing the lights live. Everything is gated behind the `lightprobes` var;
// when off (or no baked grid) the stock runtime lightreaching path is used unchanged.

#include "engine.h"

extern int sunlight;
extern const vector<int> &checklightcache(int x, int y);

struct ambientcube { vec faces[6]; };   // +X,-X,+Y,-Y,+Z,-Z

static const vec cubeaxis[6] =
{
    vec( 1, 0, 0), vec(-1, 0, 0),
    vec( 0, 1, 0), vec( 0,-1, 0),
    vec( 0, 0, 1), vec( 0, 0,-1)
};

static vector<ambientcube> probes;
static vector<uchar> probesolid; // 1 = probe sits inside solid geometry (buried/black) -> skip in interpolation
static ivec probedim(0, 0, 0);   // probe counts per axis
static int probestep = 0;        // world units between probes (probe centers at (i+0.5)*step)

VARR(lightprobes, 0, 0, 1);          // bake (at calclight) and use the ambient-cube grid for model lighting
VARR(lightprobegrid, 8, 128, 1024);  // target probe spacing in world units (bake time)
VAR(showlightprobes, 0, 0, 1);       // debug: print/inspect probe state (see /lightprobeinfo)

bool haslightprobes() { return lightprobes && probes.length() && probestep > 0; }

void clearlightprobes()
{
    probes.shrink(0);
    probesolid.shrink(0);
    probedim = ivec(0, 0, 0);
    probestep = 0;
}

// a probe whose cell is solid geometry bakes black and would drag down any model standing on/near that
// surface; detect those so sampleprobes() can exclude them and renormalise over the valid neighbours.
static bool probeinsolid(const vec &pos)
{
    ivec ip(int(floor(pos.x)), int(floor(pos.y)), int(floor(pos.z)));
    if(ip.x < 0 || ip.y < 0 || ip.z < 0 || ip.x >= worldsize || ip.y >= worldsize || ip.z >= worldsize)
        return false;            // outside the octree = open void/sky, keep it
    ivec ro; int rsize;
    cube &c = lookupcube(ip, 0, ro, rsize);
    return !isempty(c) && isentirelysolid(c);
}

static void markprobesolidity()
{
    probesolid.setsize(0);
    if(probes.empty() || probestep <= 0) return;
    loop(z, probedim.z) loop(y, probedim.y) loop(x, probedim.x)
        probesolid.add(probeinsolid(vec((x+0.5f)*probestep, (y+0.5f)*probestep, (z+0.5f)*probestep)) ? 1 : 0);
}

// the per-probe gather (ambient + direct + sun + radiosity) lives in lightmap.cpp next to calcgi; bakeprobes
// runs it across worker threads (ESC cancels, returns false), writing 6 faces per probe into a flat vec array.
extern int gi;                                  // GI bake toggle (lightmap.cpp)
extern bool bakeprobes(const vec *positions, vec *out, int count);

void genlightprobes(bool force)
{
    clearlightprobes();
    if(!lightprobes && !force) return;

    // pick a spacing that keeps the total probe count sane on large maps
    int step = max(lightprobegrid, 8);
    while(worldsize/step > 96) step *= 2;
    probestep = step;
    probedim = ivec(worldsize/step, worldsize/step, worldsize/step);
    loopk(3) probedim[k] = max(probedim[k], 1);

    int total = probedim.x*probedim.y*probedim.z;
    vector<vec> positions;
    positions.growbuf(total);
    loop(z, probedim.z) loop(y, probedim.y) loop(x, probedim.x)
        positions.add(vec((x+0.5f)*step, (y+0.5f)*step, (z+0.5f)*step));

    probes.growbuf(total);
    loopi(total) probes.add();   // ambientcube = 6 contiguous vecs, so the bake fills it as a flat vec[total*6]
    bool ok = bakeprobes(positions.getbuf(), (vec *)probes.getbuf(), total);
    if(!ok) { clearlightprobes(); conoutf("light probe bake aborted"); return; }   // don't keep a half-baked grid

    markprobesolidity();
    int buried = 0; loopv(probesolid) if(probesolid[i]) buried++;
    conoutf("generated %d light probes (%dx%dx%d, step %d, %d buried in geometry)", probes.length(), probedim.x, probedim.y, probedim.z, probestep, buried);
}
ICOMMAND(genlightprobes, "", (), genlightprobes(true));

static ambientcube sampleprobes(const vec &pos)
{
    ambientcube ac;
    if(!haslightprobes()) { loopi(6) ac.faces[i] = vec(ambientcolor.tocolor()); return ac; }
    // continuous grid coords (probe i centered at (i+0.5)*step)
    vec g = vec(pos).div(probestep).sub(vec(0.5f, 0.5f, 0.5f));
    ivec b(int(floor(g.x)), int(floor(g.y)), int(floor(g.z)));
    vec f(g.x-b.x, g.y-b.y, g.z-b.z);
    loopi(6) ac.faces[i] = vec(0, 0, 0);
    float wsum = 0;
    loop(dz, 2) loop(dy, 2) loop(dx, 2)
    {
        ivec c(clamp(b.x+dx, 0, probedim.x-1), clamp(b.y+dy, 0, probedim.y-1), clamp(b.z+dz, 0, probedim.z-1));
        int idx = (c.z*probedim.y + c.y)*probedim.x + c.x;
        if(idx < probesolid.length() && probesolid[idx]) continue;   // skip probes buried in geometry
        float w = (dx ? f.x : 1-f.x) * (dy ? f.y : 1-f.y) * (dz ? f.z : 1-f.z);
        loopi(6) ac.faces[i].add(vec(probes[idx].faces[i]).mul(w));
        wsum += w;
    }
    // renormalise over the valid neighbours so buried/black probes don't darken the result; if every
    // surrounding probe is buried, fall back to flat ambient rather than black.
    if(wsum > 1e-4f) loopi(6) ac.faces[i].mul(1.0f/wsum);
    else loopi(6) ac.faces[i] = vec(ambientcolor.tocolor());
    return ac;
}

// model-lighting interface: derive an ambient colour + dominant direction from the local probe.
void lightprobesreaching(const vec &target, vec &color, vec &dir)
{
    ambientcube ac = sampleprobes(target);
    // omnidirectional fill (so back-facing parts never go pure black)
    vec amb(0, 0, 0);
    dir = vec(0, 0, 0);
    loopi(6)
    {
        amb.add(ac.faces[i]);
        dir.add(vec(cubeaxis[i]).mul(ac.faces[i].x + ac.faces[i].y + ac.faces[i].z));
    }
    amb.mul(1.0f/6);
    if(dir.iszero()) dir = vec(0, 0, 1); else dir.normalize();
    // radiance arriving from the dominant direction — matches stock lightreaching's
    // "full light intensity" scale (a single sunlit face must read ~suncol, not suncol/6).
    color = vec(0, 0, 0);
    loopi(6)
    {
        float d = vec(cubeaxis[i]).dot(dir);
        if(d > 0) color.add(vec(ac.faces[i]).mul(d));
    }
    loopk(3) color[k] = min(max(color[k], amb[k]), 1.5f);
}

// model lighting: the 6 ambient-cube faces at a world position (for per-normal Valve ambient-cube shading).
// faces order matches cubeaxis: +X,-X,+Y,-Y,+Z,-Z. Radiances are on the same 0-1.5 scale as lightreaching.
void getprobecube(const vec &pos, vec *faces)
{
    ambientcube ac = sampleprobes(pos);
    loopi(6) faces[i] = ac.faces[i];
}

// ----- serialization (.ogz section, MAPVERSION >= 34) -----
void savelightprobes(stream *f)
{
    f->putlil<int>(probes.length());
    if(probes.empty()) return;
    f->putlil<int>(probestep);
    f->putlil<int>(probedim.x); f->putlil<int>(probedim.y); f->putlil<int>(probedim.z);
    uchar rgbe[4];
    loopv(probes) loopj(6) { encodergbe(probes[i].faces[j], rgbe); f->write(rgbe, 4); }
}

void loadlightprobes(stream *f)
{
    clearlightprobes();
    int n = f->getlil<int>();
    if(n <= 0) return;
    probestep = f->getlil<int>();
    probedim.x = f->getlil<int>(); probedim.y = f->getlil<int>(); probedim.z = f->getlil<int>();
    probes.growbuf(n);
    uchar rgbe[4];
    loopi(n) { ambientcube &ac = probes.add(); loopj(6) { f->read(rgbe, 4); ac.faces[j] = decodergbe(rgbe); } }
    markprobesolidity();   // recompute (not stored): the world geometry is loaded by now
}

ICOMMAND(lightprobeinfo, "", (),
{
    conoutf("light probes: %d cells, dim %d %d %d, step %d, active=%d",
        probes.length(), probedim.x, probedim.y, probedim.z, probestep, haslightprobes() ? 1 : 0);
    if(probes.length())
    {
        ambientcube ac = sampleprobes(camera1->o);
        loopi(6) conoutf("  face %d: %.2f %.2f %.2f", i, ac.faces[i].x, ac.faces[i].y, ac.faces[i].z);
        vec col(0,0,0); vec dir(0,0,0);
        lightprobesreaching(camera1->o, col, dir);
        conoutf("  reaching: color %.2f %.2f %.2f  dir %.2f %.2f %.2f", col.x, col.y, col.z, dir.x, dir.y, dir.z);
    }
});
