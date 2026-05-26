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
static vector<uchar> probesolid; // 1 = cell is fully solid (no air to place a probe) -> skip in interpolation
static vector<vec> probepos;     // actual baked position of each probe (relocated out of geometry; cell index kept)
static ivec probeorigin(0, 0, 0); // world origin of the grid (grid is fitted to the geometry, not the whole world)
static ivec probedim(0, 0, 0);   // probe counts per axis
static int probestep = 0;        // world units between grid cells (cell index i centered at origin + (i+0.5)*step)

VARR(lightprobes, 0, 0, 1);          // bake (at calclight) and use the ambient-cube grid for model lighting
VARR(lightprobegrid, 8, 128, 1024);  // target probe spacing in world units (bake time)
VAR(showlightprobes, 0, 0, 1);       // debug: draw a heatmap cross at each probe (blue=dim..red=bright); also /lightprobeinfo

bool haslightprobes() { return lightprobes && probes.length() && probestep > 0; }

void clearlightprobes()
{
    probes.shrink(0);
    probesolid.shrink(0);
    probepos.shrink(0);
    probeorigin = ivec(0, 0, 0);
    probedim = ivec(0, 0, 0);
    probestep = 0;
}

// is this world point in open air (not inside solid geometry)?
static bool pointinair(const vec &p)
{
    ivec ip(int(floor(p.x)), int(floor(p.y)), int(floor(p.z)));
    if(ip.x < 0 || ip.y < 0 || ip.z < 0 || ip.x >= worldsize || ip.y >= worldsize || ip.z >= worldsize)
        return true;             // outside the octree = open void/sky
    ivec ro; int rsize;
    cube &c = lookupcube(ip, 0, ro, rsize);
    return isempty(c);
}

// relocate a cell-centre probe to open space WITHIN its cell (the grid index is kept, so trilinear interpolation
// is unchanged; only the sampled value comes from nearby air). returns false if the whole cell is solid.
static bool relocateprobe(const vec &center, float cell, vec &out)
{
    if(pointinair(center)) { out = center; return true; }
    float best = 1e30f; bool found = false;
    for(int dz = -1; dz <= 1; dz++) for(int dy = -1; dy <= 1; dy++) for(int dx = -1; dx <= 1; dx++)
    {
        if(!dx && !dy && !dz) continue;
        vec p = vec(center).add(vec(dx, dy, dz).mul(cell*0.33f));   // stays within the cell (|offset| < cell/2)
        if(pointinair(p))
        {
            float d = dx*dx + dy*dy + dz*dz;                       // prefer the offset nearest the centre
            if(d < best) { best = d; out = p; found = true; }
        }
    }
    return found;
}

// (re)compute each cell's baked position (relocated out of solid) and solid flag from the current grid params.
// deterministic from the geometry, so it can run at bake time and again on map load to match the saved values.
static void placeprobes()
{
    probepos.setsize(0);
    probesolid.setsize(0);
    if(probestep <= 0) return;
    loop(z, probedim.z) loop(y, probedim.y) loop(x, probedim.x)
    {
        vec center(probeorigin.x + (x+0.5f)*probestep, probeorigin.y + (y+0.5f)*probestep, probeorigin.z + (z+0.5f)*probestep);
        vec p; bool ok = relocateprobe(center, probestep, p);
        probepos.add(ok ? p : center);
        probesolid.add(ok ? 0 : 1);
    }
}

// bounding box of open (air) leaf cubes -- the region worth covering with probes (vs the whole world cube)
static void airbounds(cube *c, const ivec &co, int size, ivec &bbmin, ivec &bbmax)
{
    loopi(8)
    {
        ivec o(i, co, size);
        if(c[i].children) airbounds(c[i].children, o, size/2, bbmin, bbmax);
        else if(isempty(c[i])) { bbmin.min(o); bbmax.max(ivec(o).add(size)); }
    }
}

// the per-probe gather (ambient + direct + sun + radiosity) lives in lightmap.cpp next to calcgi; bakeprobes
// runs it across worker threads (ESC cancels, returns false), writing 6 faces per probe into a flat vec array.
extern int gi;                                  // GI bake toggle (lightmap.cpp)
extern bool bakeprobes(const vec *positions, vec *out, int count);

void genlightprobes(bool force)
{
    clearlightprobes();
    if(!lightprobes && !force) return;

    // fit the grid to the open-space (air) bounding box, not the whole world cube -- so the probe budget goes
    // where models actually are, and the spacing can be finer for the same count
    ivec bbmin(worldsize, worldsize, worldsize), bbmax(0, 0, 0);
    if(worldroot) airbounds(worldroot, ivec(0, 0, 0), worldsize/2, bbmin, bbmax);
    if(bbmin.x >= bbmax.x) { bbmin = ivec(0, 0, 0); bbmax = ivec(worldsize, worldsize, worldsize); }   // no air -> whole world
    ivec bbsize = ivec(bbmax).sub(bbmin);

    int step = max(lightprobegrid, 8), maxdim = max(bbsize.x, max(bbsize.y, bbsize.z));
    while(maxdim/step > 96) step *= 2;                  // cap cells/axis (now measured over the fitted box)
    probestep = step;
    probeorigin = bbmin;
    loopk(3) probedim[k] = max(1, (bbsize[k] + step - 1)/step);

    placeprobes();   // fill probepos[] (relocated out of solid) + probesolid[]
    int total = probedim.x*probedim.y*probedim.z;
    vector<vec> positions;
    positions.growbuf(total);
    loopv(probepos) positions.add(probepos[i]);

    probes.growbuf(total);
    loopi(total) probes.add();   // ambientcube = 6 contiguous vecs, so the bake fills it as a flat vec[total*6]
    bool ok = bakeprobes(positions.getbuf(), (vec *)probes.getbuf(), total);
    if(!ok) { clearlightprobes(); conoutf("light probe bake aborted"); return; }   // don't keep a half-baked grid

    int solid = 0; loopv(probesolid) if(probesolid[i]) solid++;
    conoutf("generated %d light probes (%dx%dx%d, step %d, origin %d %d %d, %d in solid)",
        probes.length(), probedim.x, probedim.y, probedim.z, probestep, probeorigin.x, probeorigin.y, probeorigin.z, solid);
}
ICOMMAND(genlightprobes, "", (), genlightprobes(true));

static ambientcube sampleprobes(const vec &pos)
{
    ambientcube ac;
    if(!haslightprobes()) { loopi(6) ac.faces[i] = vec(ambientcolor.tocolor()); return ac; }
    // continuous grid coords (cell index i centered at origin + (i+0.5)*step)
    vec g = vec(pos).sub(vec(probeorigin)).div(probestep).sub(vec(0.5f, 0.5f, 0.5f));
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
    f->putlil<int>(probeorigin.x); f->putlil<int>(probeorigin.y); f->putlil<int>(probeorigin.z);
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
    probeorigin.x = f->getlil<int>(); probeorigin.y = f->getlil<int>(); probeorigin.z = f->getlil<int>();
    probes.growbuf(n);
    uchar rgbe[4];
    loopi(n) { ambientcube &ac = probes.add(); loopj(6) { f->read(rgbe, 4); ac.faces[j] = decodergbe(rgbe); } }
    placeprobes();   // recompute positions/solid (not stored): the world geometry is loaded by now
}

// debug: draw a coloured cross at each non-buried probe (colour = its omnidirectional ambient), so you can see
// where the probes are and how bright each one bakes. Toggle with `showlightprobes 1`.
void renderlightprobes()
{
    if(!showlightprobes || !haslightprobes()) return;
    notextureshader->set();
    gle::defvertex();
    float r = probestep*0.4f;
    loopv(probes)
    {
        if(i < probesolid.length() && probesolid[i]) continue;       // skip cells with no air
        vec c(0, 0, 0);
        loopj(6) c.add(probes[i].faces[j]);
        float lum = (c.x + c.y + c.z)*(1.0f/18);   // mean of 6 faces, mean of channels
        // heatmap (blue=dark .. green .. red=bright) at full saturation, so probes are visible on any background
        // and you can read brightness: blue probes are dim, red/yellow probes are bright (over-bright = blown models)
        vec col = lum < 0.25f ? vec(0, lum*4, 1)
                : lum < 0.5f  ? vec(0, 1, 1-(lum-0.25f)*4)
                : lum < 0.75f ? vec((lum-0.5f)*4, 1, 0)
                :               vec(1, max(1-(lum-0.75f)*4, 0.0f), 0);
        vec p = i < probepos.length() ? probepos[i] : vec(0, 0, 0);   // actual (relocated) position
        gle::color(bvec(uchar(col.x*255), uchar(col.y*255), uchar(col.z*255)));
        gle::begin(GL_LINES);
        gle::attrib(vec(p).sub(vec(r, 0, 0))); gle::attrib(vec(p).add(vec(r, 0, 0)));
        gle::attrib(vec(p).sub(vec(0, r, 0))); gle::attrib(vec(p).add(vec(0, r, 0)));
        gle::attrib(vec(p).sub(vec(0, 0, r))); gle::attrib(vec(p).add(vec(0, 0, r)));
        xtraverts += gle::end();
    }
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
