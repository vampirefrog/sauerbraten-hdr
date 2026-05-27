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

#endif
