// serverengine.cpp: support code for the STANDALONE dedicated server, which compiles the
// octree/edit engine (octa/world/octaedit) to maintain its own copy of the map. It
// provides two kinds of symbols the server would otherwise pull from rendering files:
//   1. faithful re-implementations of small, render-independent data helpers that happen
//      to live in rendering translation units (e.g. the cube-surface helpers in
//      lightmap.cpp), and
//   2. no-op stubs for rendering/GL entry points that the octree code calls but that have
//      no meaning without a renderer.
// It is only built with -DSTANDALONE.

#ifdef STANDALONE

#include "engine.h"

// --- GL extension function pointers referenced by inline shader-param methods in
// texture.h. Never invoked on the server (no rendering); defined NULL so the rare
// odr-use still links.
#define GLSTUB_FNPTR(name) void (*name)(...) = NULL;
GLSTUB_FNPTR(glUniform1f_)  GLSTUB_FNPTR(glUniform2f_)  GLSTUB_FNPTR(glUniform3f_)  GLSTUB_FNPTR(glUniform4f_)
GLSTUB_FNPTR(glUniform1i_)  GLSTUB_FNPTR(glUniform2i_)  GLSTUB_FNPTR(glUniform3i_)  GLSTUB_FNPTR(glUniform4i_)
GLSTUB_FNPTR(glUniform1fv_) GLSTUB_FNPTR(glUniform2fv_) GLSTUB_FNPTR(glUniform3fv_) GLSTUB_FNPTR(glUniform4fv_)
GLSTUB_FNPTR(glUniform1iv_) GLSTUB_FNPTR(glUniform2iv_) GLSTUB_FNPTR(glUniform3iv_) GLSTUB_FNPTR(glUniform4iv_)
GLSTUB_FNPTR(glUniformMatrix2fv_) GLSTUB_FNPTR(glUniformMatrix3fv_) GLSTUB_FNPTR(glUniformMatrix4fv_)

// --- cube-surface helpers, re-implemented verbatim from lightmap.cpp (pure data ops on
// cube extensions; no rendering). Edited cubes get full-bright surfaces here, matching
// the client, so the server's saved .ogz is consistent.
static const surfaceinfo brightsurfaces[6] =
{
    brightsurface, brightsurface, brightsurface,
    brightsurface, brightsurface, brightsurface
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

// --- rendering / lighting no-op stubs (the octree code calls these; nothing headless).
void renderprogress(float bar, const char *text, GLuint tex, bool background) {}
void initlights() {}
void clearlightcache(int id) {}
void resetlightmaps(bool fullclean) {}

// --- globals referenced by the octree/edit code; inert on the server.
dynent *player = NULL;
physent *camera1 = NULL;
bool inbetweenframes = false, renderedframe = true;
int hidehud = 0;
int mainmenu = 0;
int blendpaintmode = 0;
int usegui2d = 0, menuautoclose = 0;
vec worldpos(0, 0, 0), camdir(0, 0, 0), camright(0, 0, 0), camup(0, 0, 0);
vector<vtxarray *> varoot, valist;

// --- vertex-array / render / pvs / blob / decal / sound / menu / pfx no-ops.
void allchanged(bool) {}
void octarender() {}
void destroyva(vtxarray *, bool) {}
void updatevabb(vtxarray *, bool) {}
void updatevabbs(bool) {}
void resetclipplanes() {}
void guessnormals(const vec *, int, vec *) {}
void reduceslope(ivec &) {}
int isvisiblesphere(float, const vec &) { return 0; }
void cleanreflections() {}
void cleardamagescreen() {}
void cleardecals() {}
void clearmainmenu() {}
void clearmapsounds() {}
void clearparticles() {}
void clearpvs() {}
void clearsleep(bool) {}
void invalidatepostfx() {}
void keyrepeat(bool, int) {}
void resetblobs() {}
void resetblendmap() {}
void enlargeblendmap() {}
void shrinkblendmap(int) {}
void stoppaintblendmap() {}
void trypaintblendmap() {}
void setupmaterials(int, int) {}
int findmaterial(const char *) { return -1; }
const char *getmaterialdesc(int, const char *) { return ""; }
bool printparticles(extentity &, char *, int) { return false; }
bool isconnected(bool, bool) { return false; }
bool multiplayer(bool) { return false; }
void dropenttofloor(entity *) {}
bool entinmap(dynent *, bool) { return true; }
vec menuinfrontofplayer() { return vec(0, 0, 0); }
void g3d_addgui(g3d_callback *, vec &, int) {}
void clearmapcrc() {}

// --- game callbacks invoked by the octree/edit code. On the server, edits arrive over
// the network and are applied with local=false, so the local-only hooks (edittrigger,
// etc.) do nothing; map lifecycle is driven by server::changemap instead.
namespace game
{
    bool allowedittoggle() { return true; }
    void edittoggled(bool) {}
    void forceedit(const char *) {}
    void edittrigger(const selinfo &, int, int, int, int, const VSlot *) {}
    const char *getclientmap() { return ""; }
    void resetgamestate() {}
    float ratespawn(dynent *, const extentity &) { return 1.0f; }
    void newmap(int) {}
    void startmap(const char *) {}
}

// --- texture / VSlot / shader symbols. texture.cpp's VSlot management IS compiled into
// the server; these are the render-only entry points it and the octree/worldio code
// reference but that have no meaning headless. getshaderparamname is the real one (VSlot
// param dedup in comparevslot compares interned name pointers); the rest are no-ops.
Texture *notexture = NULL;
Shader *hudshader = NULL;
Shader *Shader::lastshader = NULL;
int smoothangle(int, int) { return 0; }
Texture *loadthumbnail(Slot &) { return notexture; }
void Shader::allocparams(Slot *) {}
void Shader::bindprograms() {}
bool Shader::isnull(const Shader *s) { return !s; }
Shader *useshaderbyname(const char *) { return NULL; }
void resetslotshader() {}
void setslotshader(Slot &) {}
void linkvslotshader(VSlot &, bool) {}

static hashset<const char *> shaderparamnames(256);
const char *getshaderparamname(const char *name, bool insert)
{
    const char *exists = shaderparamnames.find(name, NULL);
    if(exists || !insert) return exists;
    return shaderparamnames.add(newstring(name));
}

const texrotation texrotations[8] =
{
    { false, false, false }, { false,  true,  true }, {  true,  true, false }, {  true, false,  true },
    {  true, false, false }, { false,  true, false }, { false, false,  true }, {  true,  true,  true },
};

#endif
