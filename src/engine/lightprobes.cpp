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
static ivec probedim(0, 0, 0);   // probe counts per axis
static int probestep = 0;        // world units between probes (probe centers at (i+0.5)*step)

VARR(lightprobes, 0, 0, 1);          // bake (at calclight) and use the ambient-cube grid for model lighting
VARR(lightprobegrid, 8, 128, 1024);  // target probe spacing in world units (bake time)
VAR(showlightprobes, 0, 0, 1);       // debug: print/inspect probe state (see /lightprobeinfo)

bool haslightprobes() { return lightprobes && probes.length() && probestep > 0; }

void clearlightprobes()
{
    probes.shrink(0);
    probedim = ivec(0, 0, 0);
    probestep = 0;
}

// gather an ambient cube at a world position from the static lights, sun and skylight
static void computeprobe(const vec &pos, ambientcube &ac)
{
    vec amb = vec(skylightcolor.tocolor().max(ambientcolor.tocolor()));   // flat ambient floor
    loopi(6) ac.faces[i] = amb;

    const vector<extentity *> &ents = entities::getents();
    const vector<int> &lights = checklightcache(int(pos.x), int(pos.y));
    loopv(lights)
    {
        extentity &e = *ents[lights[i]];
        if(e.type != ET_LIGHT) continue;
        vec ray(pos);
        ray.sub(e.o);
        float mag = ray.magnitude();
        if(e.attr1 && mag >= float(e.attr1)) continue;
        vec tolight(0, 0, 1);
        if(mag >= 1e-4f)
        {
            ray.div(mag);
            if(shadowray(e.o, ray, mag, RAY_SHADOW | RAY_POLY) < mag) continue;
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
        loopj(6) { float d = cubeaxis[j].dot(tolight); if(d > 0) ac.faces[j].add(vec(lightcol).mul(d)); }
    }
    if(sunlight && shadowray(pos, sunlightdir, 1e16f, RAY_SHADOW | RAY_POLY) > 1e15f)
    {
        vec suncol = vec(sunlightcolor.x, sunlightcolor.y, sunlightcolor.z).mul(sunlightscale/255.0f);
        loopj(6) { float d = cubeaxis[j].dot(sunlightdir); if(d > 0) ac.faces[j].add(vec(suncol).mul(d)); }
    }
}

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
    probes.growbuf(total);
    loop(z, probedim.z) loop(y, probedim.y) loop(x, probedim.x)
    {
        vec pos((x+0.5f)*step, (y+0.5f)*step, (z+0.5f)*step);
        computeprobe(pos, probes.add());
    }
    conoutf("generated %d light probes (%dx%dx%d, step %d)", probes.length(), probedim.x, probedim.y, probedim.z, probestep);
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
    loop(dz, 2) loop(dy, 2) loop(dx, 2)
    {
        ivec c(clamp(b.x+dx, 0, probedim.x-1), clamp(b.y+dy, 0, probedim.y-1), clamp(b.z+dz, 0, probedim.z-1));
        float w = (dx ? f.x : 1-f.x) * (dy ? f.y : 1-f.y) * (dz ? f.z : 1-f.z);
        const ambientcube &p = probes[(c.z*probedim.y + c.y)*probedim.x + c.x];
        loopi(6) ac.faces[i].add(vec(p.faces[i]).mul(w));
    }
    return ac;
}

// model-lighting interface: derive an ambient colour + dominant direction from the local probe.
void lightprobesreaching(const vec &target, vec &color, vec &dir)
{
    ambientcube ac = sampleprobes(target);
    color = vec(0, 0, 0);
    dir = vec(0, 0, 0);
    loopi(6)
    {
        color.add(ac.faces[i]);
        dir.add(vec(cubeaxis[i]).mul(ac.faces[i].x + ac.faces[i].y + ac.faces[i].z));
    }
    color.mul(1.0f/6);                       // spherical-average irradiance
    loopk(3) color[k] = min(color[k], 1.5f);
    if(dir.iszero()) dir = vec(0, 0, 1); else dir.normalize();
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
}

ICOMMAND(lightprobeinfo, "", (),
{
    conoutf("light probes: %d cells, dim %d %d %d, step %d, active=%d",
        probes.length(), probedim.x, probedim.y, probedim.z, probestep, haslightprobes() ? 1 : 0);
    if(probes.length())
    {
        ambientcube ac = sampleprobes(camera1->o);
        loopi(6) conoutf("  face %d: %.2f %.2f %.2f", i, ac.faces[i].x, ac.faces[i].y, ac.faces[i].z);
    }
});
