// gltfexport.cpp -- /writegltf <name> : dump the current world (cube octree, mapmodels,
//                   sun + point lights) as a glTF 2.0 file + .bin payload.
//
// Adapted from the exporter found in sauerbraten-code (anonymous-svn fork): same vertex layout,
// vtxarray/elementset structure, and KHR_materials_specular / KHR_lights_punctual mapping. The
// integration here adds HDR lightmap output for this fork:
//
//   - Every unique 'lmid' touched by an exported VA primitive turns into one image: an LDR PNG
//     of the atlas page (tonemapped from RGBE when the page is LM_HDR) plus a Radiance .hdr
//     carrying the unmodified linear HDR data. The PNG is referenced as the material's
//     emissiveTexture, with KHR_materials_emissive_strength multiplying it back up to the page's
//     peak luminance so HDR-aware viewers see the real radiance and LDR viewers see a sensible
//     baked-light approximation.
//   - For HDR/RNM lightmaps (LM_RNM0 + LM_HDR), the three basis pages at lmid, lmid+1, lmid+2 are
//     averaged into a single "flat surface" diffuse representation before tonemapping -- summing
//     basis projections weighted by 1/3 each, since each basis vector b_k has dot(b_k, +Z) = 1/sqrt(3).
//
// One-off command, runs on the main thread; calls readva() which copies VBO contents back to CPU.

#include "engine.h"
// lightmap.h is already included via engine.h
#include "gltfexport.h"

// engine internals we lean on
extern vector<vtxarray *> valist;
extern bool readva(vtxarray *va, ushort *&edata, vertex *&vdata);
extern void savepng(const char *filename, ImageData &image, bool flip);
extern entity sunlightent;
extern bvec sunlightcolor;
extern int sunlightpitch, sunlightyaw;
extern vector<LightMap> lightmaps;
extern vector<LightMapTexture> lightmaptexs;

// -----------------------------------------------------------------------------------------------
// helpers
// -----------------------------------------------------------------------------------------------

// Radiance HDR (`#?RADIANCE`) with uncompressed RGBE scanlines. RGBE is exactly how Sauer keeps
// HDR lightmap data already, so we can just memcpy each row in.
static bool savehdr(const char *filename, int w, int h, const uchar *rgbe)
{
    stream *f = openfile(filename, "wb");
    if(!f) { conoutf(CON_ERROR, "could not write %s", filename); return false; }
    f->printf("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\nGAMMA=1.0\nEXPOSURE=1.0\n\n");
    f->printf("-Y %d +X %d\n", h, w);
    // emit row-by-row, no RLE: every scanline begins with 0x02 0x02 (hi w) (lo w) header to mark
    // "new RLE" -- but we set hi w bit 7 to 0 which signals "no RLE, plain bytes" for old readers.
    // To keep maximum compatibility (Blender, three.js, glTF Sample Viewer, hdrshop) we just dump
    // bytes line by line without per-line header. Both old and new decoders accept that.
    loopi(h) f->write(rgbe + i*w*4, w*4);
    delete f;
    return true;
}

// HL2 RNM bases share dot(b_k, +Z) = 1/sqrt(3); weighted sum of the three basis pages with each
// weight = 1/3 reconstructs the flat-tangent-normal diffuse term -- the right value for a viewer
// without the normal-map normal.
static vec lm_decode_blend(int lmid, int x, int y)
{
    int idx = lmid - LMID_RESERVED;
    if(!lightmaps.inrange(idx)) return vec(0, 0, 0);
    LightMap &lm = lightmaps[idx];
    if(!lm.data) return vec(0, 0, 0);
    int off = (y*LM_PACKW + x)*lm.bpp;
    if((lm.type & LM_TYPE) == LM_RNM0 && (lm.type & LM_HDR))
    {
        vec c = decodergbe(lm.data + off);
        if(lightmaps.inrange(idx+1) && lightmaps[idx+1].data) c.add(decodergbe(lightmaps[idx+1].data + off));
        if(lightmaps.inrange(idx+2) && lightmaps[idx+2].data) c.add(decodergbe(lightmaps[idx+2].data + off));
        return c.div(3);
    }
    if(lm.type & LM_HDR) return decodergbe(lm.data + off);
    // LDR atlas page: bytes already linear-ish 8-bit. Lift to vec [0,1].
    return vec(lm.data[off+0]/255.0f, lm.data[off+1]/255.0f, lm.data[off+2]/255.0f);
}

// Tonemap a single sample with a Reinhard curve, scaled so 'peak' maps to ~1.0 in the PNG. Strength
// multiplier (returned via 'peak') restores brightness when the viewer applies KHR_emissive_strength.
static inline void tonemap_sample(vec &c, float peak)
{
    if(peak <= 0) { c = vec(0, 0, 0); return; }
    c.div(peak);
    c.x = c.x / (1.0f + c.x);
    c.y = c.y / (1.0f + c.y);
    c.z = c.z / (1.0f + c.z);
    // undo the squash at peak so the peak ends at ~1.0 instead of ~0.5.
    c.mul(2.0f);
    c.x = min(c.x, 1.0f); c.y = min(c.y, 1.0f); c.z = min(c.z, 1.0f);
}

// Build a PNG+.hdr pair for one lmid: returns the peak luminance (used as emissive strength) and
// the relative URI of the PNG, which becomes the image's URI in the gltf.
struct lmexport
{
    int lmid;
    string pnguri, hdruri;
    float peak;
};

// Return the trailing filename component of a name (after the last slash). The exporter writes
// every sidecar (.bin, _lm*.png, _lm*.hdr) right next to the .gltf, so embedded URIs need to be
// basenames or they'll be interpreted by glTF loaders as relative to the .gltf's directory and
// double-apply any user-supplied subpath like "subdir/foo".
static inline const char *basenameuri(const char *name)
{
    const char *s = strrchr(name, '/');
    return s ? s + 1 : name;
}

// resolvediskuri / gltf_resolvediskuri are now in gltfexport.h (rendermodel.cpp uses the same helper).
#define resolvediskuri gltf_resolvediskuri

// RNM path: one LightMap (LM_RNM0|LM_HDR) -> one averaged-basis PNG at LM_PACKW x LM_PACKH.
// Each basis vector b_k has dot(b_k, +Z) = 1/sqrt(3), so summing the three basis pages with weight
// 1/3 each reconstructs the flat-tangent-normal diffuse term (the right value for a renderer that
// doesn't have the per-pixel normal-map normal to evaluate the bases against).
//
// File naming: `<name>_lm<lmid>.png` / `.hdr`. lmid is unique per surface so different RNM atlases
// never collide.
static bool export_lmpage_rnm(const char *name, int lmid, lmexport &out)
{
    int idx = lmid - LMID_RESERVED;
    if(!lightmaps.inrange(idx) || !lightmaps[idx].data) return false;

    // First pass: decode + find peak luminance, ignore top 1% so a couple of hot pixels don't
    // crush everything else down to near-zero in the PNG.
    vector<float> lum;
    lum.reserve(LM_PACKW * LM_PACKH);
    loopi(LM_PACKH) loopj(LM_PACKW)
    {
        vec c = lm_decode_blend(lmid, j, i);
        lum.add(0.2126f*c.x + 0.7152f*c.y + 0.0722f*c.z);
    }
    vector<float> tmp;
    tmp.put(lum.getbuf(), lum.length());
    quicksort(tmp.getbuf(), tmp.length(), sortless());
    float peak = tmp[int(tmp.length()*0.99f)];
    if(peak < 1.0f) peak = 1.0f;

    ImageData img(LM_PACKW, LM_PACKH, 3);
    loopi(LM_PACKH) loopj(LM_PACKW)
    {
        vec c = lm_decode_blend(lmid, j, i);
        tonemap_sample(c, peak);
        uchar *p = img.data + (i*LM_PACKW + j)*3;
        p[0] = uchar(c.x*255.0f); p[1] = uchar(c.y*255.0f); p[2] = uchar(c.z*255.0f);
    }
    defformatstring(pngname, "%s_lm%d.png", name, lmid);
    savepng(pngname, img, false);
    defformatstring(pnguri, "%s_lm%d.png", basenameuri(name), lmid);
    copystring(out.pnguri, pnguri);

    // Linear-light HDR sidecar: re-encode the averaged basis to RGBE.
    defformatstring(hdrname, "%s_lm%d.hdr", name, lmid);
    uchar *rgbe = new uchar[LM_PACKW*LM_PACKH*4];
    loopi(LM_PACKH) loopj(LM_PACKW)
    {
        vec c = lm_decode_blend(lmid, j, i);
        encodergbe(c, rgbe + (i*LM_PACKW + j)*4);
    }
    savehdr(hdrname, LM_PACKW, LM_PACKH, rgbe);
    delete[] rgbe;
    defformatstring(hdruri, "%s_lm%d.hdr", basenameuri(name), lmid);
    copystring(out.hdruri, hdruri);

    out.lmid = lmid;
    // RNM averaged-basis PNG is tonemapped like the HDR atlas path -- raise to 2.2 so the
    // companion addon (sRGB-decoding lightmap + linear-space radiance) matches Sauer.
    // See the comment on `out.peak` in export_lmtex_atlas() for the full derivation.
    out.peak = powf(peak, 2.2f);
    return true;
}

// Non-RNM path: stitch every LightMap with `lm.tex == lmtexidx` into a CPU buffer at its
// (offsetx, offsety). The result mirrors the GL atlas that genlightmaptexs uploads (multiple
// LM_PACKW x LM_PACKH pages packed into one tex.w x tex.h texture, up to roundlightmaptex /
// hardware max). The vertex's lm.x/lm.y is encoded against tex.w/tex.h, so the importer's
// shader can sample this PNG directly using `lm / SHRT_MAX` -- no per-vertex UV recompute.
//
// File naming: `<name>_atlas<lmtexidx>.png` / `.hdr`. Distinct from the RNM `_lm<lmid>` files
// so a map mixing HDR/RNM and plain lightmaps doesn't collide on disk.
static bool export_lmtex_atlas(const char *name, int lmtexidx, lmexport &out)
{
    if(!lightmaptexs.inrange(lmtexidx)) return false;
    LightMapTexture &tex = lightmaptexs[lmtexidx];
    int W = tex.w, H = tex.h;
    if(W <= 0 || H <= 0) return false;
    bool ishdr = (tex.type & LM_HDR) != 0;

    // Find the bpp of the first contributing LightMap (all members of a single atlas share bpp).
    int bpp = 0;
    loopv(lightmaps) if(lightmaps[i].tex == lmtexidx && lightmaps[i].data) { bpp = lightmaps[i].bpp; break; }
    if(!bpp) return false;

    // Stitch -- zero-init the gaps so any unused atlas slots show black instead of garbage.
    uchar *atlas = new uchar[W * H * bpp]();
    loopv(lightmaps)
    {
        LightMap &lm = lightmaps[i];
        if(lm.tex != lmtexidx || !lm.data) continue;
        for(int y = 0; y < LM_PACKH; y++)
        {
            uchar *dst = atlas + ((lm.offsety + y) * W + lm.offsetx) * bpp;
            const uchar *src = lm.data + y * LM_PACKW * bpp;
            memcpy(dst, src, LM_PACKW * bpp);
        }
    }

    // Decode pixels into a luminance buffer to pick the 99th-percentile peak.
    vector<float> lum;
    lum.reserve(W*H);
    loopi(H*W)
    {
        vec c(0, 0, 0);
        if(ishdr && bpp == 4) c = decodergbe(atlas + i*4);
        else c = vec(atlas[i*bpp+0]/255.0f, atlas[i*bpp+1]/255.0f, atlas[i*bpp+2]/255.0f);
        lum.add(0.2126f*c.x + 0.7152f*c.y + 0.0722f*c.z);
    }
    vector<float> tmp;
    tmp.put(lum.getbuf(), lum.length());
    quicksort(tmp.getbuf(), tmp.length(), sortless());
    float peak = tmp[int(tmp.length()*0.99f)];
    if(peak < 1.0f) peak = 1.0f;

    // LDR PNG (tonemapped if HDR, straight copy if LDR).
    ImageData img(W, H, 3);
    if(ishdr && bpp == 4)
    {
        loopi(H*W)
        {
            vec c = decodergbe(atlas + i*4);
            tonemap_sample(c, peak);
            uchar *p = img.data + i*3;
            p[0] = uchar(c.x*255.0f); p[1] = uchar(c.y*255.0f); p[2] = uchar(c.z*255.0f);
        }
    }
    else
    {
        loopi(H*W)
        {
            uchar *p = img.data + i*3;
            p[0] = atlas[i*bpp+0]; p[1] = atlas[i*bpp+1]; p[2] = atlas[i*bpp+2];
        }
    }
    defformatstring(pngname, "%s_atlas%d.png", name, lmtexidx);
    savepng(pngname, img, false);
    defformatstring(pnguri, "%s_atlas%d.png", basenameuri(name), lmtexidx);
    copystring(out.pnguri, pnguri);

    // Linear-light HDR sidecar. For RGBE atlases we already have the bytes; for LDR atlases we
    // re-encode each pixel so consumers can pick the linear path uniformly.
    defformatstring(hdrname, "%s_atlas%d.hdr", name, lmtexidx);
    if(ishdr && bpp == 4)
    {
        savehdr(hdrname, W, H, atlas);
    }
    else
    {
        uchar *rgbe = new uchar[W*H*4];
        loopi(H*W)
        {
            vec c(atlas[i*bpp+0]/255.0f, atlas[i*bpp+1]/255.0f, atlas[i*bpp+2]/255.0f);
            encodergbe(c, rgbe + i*4);
        }
        savehdr(hdrname, W, H, rgbe);
        delete[] rgbe;
    }
    defformatstring(hdruri, "%s_atlas%d.hdr", basenameuri(name), lmtexidx);
    copystring(out.hdruri, hdruri);

    out.lmid = lmtexidx;   // reused as 'atlas id' here -- not a true lmid
    // KHR_materials_emissive_strength carries the multiplier the importer applies in *linear*
    // space. Sauer's display intensity for a pixel is `2 * diff/255 * lm/255`, but Sauer treats
    // that linear value as already-display-referred (no sRGB encode on output), whereas Blender's
    // default View Transform sRGB-encodes its linear render. To make Blender's display match
    // Sauer's display, we need `linear_radiance = (2 * diff/255 * lm/255)^2.2`. The companion
    // Blender addon flips the lightmap PNG colourspace to sRGB (decode = `pow(byte/255, 2.2)`)
    // so its sample is already in the gamma-2.2 space, and we provide the outer `2^2.2` factor
    // as the emissive strength.
    //   - LDR atlas: strength = pow(2, 2.2) ~= 4.594
    //   - HDR atlas: strength = pow(peak, 2.2) (tonemap PNG byte ~ 2c/peak, gamma'd to c^2.2,
    //     scaled back by peak^2.2 recovers c^2.2)
    // The .hdr sidecar (linear RGBE) is symmetric: sample is linear `c` (Non-Color), and the
    // addon overrides strength to pow(2, 2.2) so the 2x overbright is preserved.
    out.peak = ishdr ? powf(peak, 2.2f) : powf(2.0f, 2.2f);
    delete[] atlas;
    return true;
}
// -----------------------------------------------------------------------------------------------
// writegltf <name>
// -----------------------------------------------------------------------------------------------

void writegltf(char *name)
{
    if(!name || !*name) name = (char *)game::getclientmap();
    if(!name || !*name) { conoutf(CON_ERROR, "writegltf: no map loaded"); return; }

    gltf g;
    g.samplers.add(gltf::sampler());                  // sampler 0 = LINEAR / REPEAT

    defformatstring(binname, "%s.bin", name);
    path(binname);

    {
        gltf::scene s;
        copystring(s.name, name);
        s.nodes.add(0);
        g.scenes.add(s);
    }

    gltf::mesh octreemesh;
    copystring(octreemesh.name, "Map mesh");

    conoutf("writegltf: opening %s", binname);
    stream *b = openfile(binname, "wb");
    if(!b) { conoutf(CON_ERROR, "could not open %s", binname); return; }

    // ------- lmid -> emissive texture mapping ---------------------------------------------------
    // RNM lightmaps export per-LightMap (one averaged-basis PNG sized LM_PACKW x LM_PACKH); the
    // dedup key is the lmid itself. Non-RNM lightmaps export per parent atlas LightMapTexture
    // (one stitched PNG sized tex.w x tex.h, mirroring the GL upload), and many lmids that share
    // a parent atlas point at the same PNG/texture -- so we dedup on lm.tex too. Both flows fill
    // the lmcache so getlmstrength() can look up KHR_materials_emissive_strength per surface.
    struct lmrec     { int lmid;     int textureIndex; float strength; };
    struct lmtexrec  { int lmtexidx; int textureIndex; float strength; };
    vector<lmrec>    lmcache;
    vector<lmtexrec> lmtexcache;

    auto getlmtexture = [&](int lmid) -> int {
        if(lmid < LMID_RESERVED) return -1;
        loopv(lmcache) if(lmcache[i].lmid == lmid) return lmcache[i].textureIndex;
        int idx = lmid - LMID_RESERVED;
        if(!lightmaps.inrange(idx) || !lightmaps[idx].data) return -1;
        LightMap &lm = lightmaps[idx];
        bool isrnm = (lm.type & LM_TYPE) == LM_RNM0;

        if(isrnm)
        {
            lmexport e;
            if(!export_lmpage_rnm(name, lmid, e)) return -1;
            gltf::texture lmtex;
            defformatstring(lmtexname, "lm%d", lmid);
            copystring(lmtex.name, lmtexname);
            lmtex.sampler = 0;
            lmtex.source = g.getOrAddImage(e.pnguri);
            int ti = g.getOrAddTexture(lmtex);
            lmrec rec = { lmid, ti, e.peak };
            lmcache.add(rec);
            conoutf("writegltf: lmid %d -> %s (rnm, peak %.2f)", lmid, e.pnguri, e.peak);
            return ti;
        }

        // Non-RNM: dedup at the parent atlas. Multiple lmids that share lm.tex collapse to one
        // PNG and one glTF texture.
        if(!lightmaptexs.inrange(lm.tex)) return -1;
        loopv(lmtexcache) if(lmtexcache[i].lmtexidx == lm.tex)
        {
            lmrec rec = { lmid, lmtexcache[i].textureIndex, lmtexcache[i].strength };
            lmcache.add(rec);
            return rec.textureIndex;
        }
        lmexport e;
        if(!export_lmtex_atlas(name, lm.tex, e)) return -1;
        gltf::texture lmtex;
        defformatstring(lmtexname, "atlas%d", lm.tex);
        copystring(lmtex.name, lmtexname);
        lmtex.sampler = 0;
        lmtex.source = g.getOrAddImage(e.pnguri);
        int ti = g.getOrAddTexture(lmtex);
        lmtexrec rt = { lm.tex, ti, e.peak };
        lmtexcache.add(rt);
        lmrec rec = { lmid, ti, e.peak };
        lmcache.add(rec);
        conoutf("writegltf: atlas %d (lmid %d, %dx%d) -> %s (peak %.2f)",
            lm.tex, lmid, lightmaptexs[lm.tex].w, lightmaptexs[lm.tex].h, e.pnguri, e.peak);
        return ti;
    };

    auto getlmstrength = [&](int lmid) -> float {
        loopv(lmcache) if(lmcache[i].lmid == lmid) return lmcache[i].strength;
        return 1.0f;
    };

    // ------- world VA primitives (the octree mesh) ----------------------------------------------
    int binsize = 0;
    loopv(valist)
    {
        vtxarray &va = *valist[i];
        ushort *edata = NULL;
        vertex *vdata = NULL;
        if(!readva(&va, edata, vdata)) continue;

        int vsize = va.verts * sizeof(vertex);
        vec vmin(FLT_MAX, FLT_MAX, FLT_MAX), vmax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        // svec2 lm: Sauer's world shader does `texcoord1 = vtexcoord1 * (1/32767)` (see
        // data/glsl.cfg `lmcoordscale`), so lm.x/lm.y already span [0, SHRT_MAX] for [0,1] UVs.
        // Sauer's encoder puts lm.y=0 at atlas memory row 0 (savepng writes that as PNG row 0 =
        // top of image), and glTF spec says UV (0,0) is the upper-left corner -- same axis
        // direction, no transform needed. componentType=SHORT, normalized=true on the accessor
        // then divides by 32767 in the importer, recovering [0,1].
        loopj(va.verts) {
            // Sauer leaves the normal at all-zeros on placeholder surfs (sky/material brushes); glTF
            // rejects zero-length NORMAL with KHR_mesh_quantization, so substitute a sentinel +Y.
            // readva() returns vertices in Sauer's GPU layout, where norm.flip() has already
            // remapped unsigned bytes to signed (0 = 0.0, 127 = +1.0, -128 = -1.0), so the bytes
            // are directly compatible with the glTF BYTE NORMALIZED format -- no further remap.
            if(!vdata[j].norm.x && !vdata[j].norm.y && !vdata[j].norm.z)
                vdata[j].norm = bvec4(0, 127, 0, 0);    // signed-byte +Y unit vector

            if(vdata[j].pos.x < vmin.x) vmin.x = vdata[j].pos.x;
            if(vdata[j].pos.y < vmin.y) vmin.y = vdata[j].pos.y;
            if(vdata[j].pos.z < vmin.z) vmin.z = vdata[j].pos.z;
            if(vdata[j].pos.x > vmax.x) vmax.x = vdata[j].pos.x;
            if(vdata[j].pos.y > vmax.y) vmax.y = vdata[j].pos.y;
            if(vdata[j].pos.z > vmax.z) vmax.z = vdata[j].pos.z;
        }
        int r = b->write(vdata, vsize);
        if(r != vsize) conoutf(CON_WARN, "writegltf: short write for vertices (%d/%d)", r, vsize);

        // Build accessor + bufferView pair for each vertex attribute, interleaved through the
        // single packed vertex stride.
        int posAcc = g.accessors.length();
        gltf::accessor a;
        a.bufferView = g.bufferViews.length(); a.componentType = gltf::accessor::FLOAT; a.type = gltf::accessor::VEC3;
        a.count = va.verts; a.min = vmin; a.max = vmax; a.minmax = true; a.normalized = false; g.accessors.add(a);
        gltf::bufferView bv;
        bv.buffer = 0; bv.byteLength = vsize - (int)offsetof(vertex, pos); bv.byteOffset = binsize + (int)offsetof(vertex, pos);
        bv.target = gltf::bufferView::ARRAY_BUFFER; bv.byteStride = sizeof(vertex); g.bufferViews.add(bv);

        int normAcc = g.accessors.length();
        a.bufferView = g.bufferViews.length(); a.componentType = gltf::accessor::BYTE; a.type = gltf::accessor::VEC3;
        a.count = va.verts; a.minmax = false; a.normalized = true; g.accessors.add(a);
        bv.byteLength = vsize - (int)offsetof(vertex, norm); bv.byteOffset = binsize + (int)offsetof(vertex, norm); g.bufferViews.add(bv);

        int tcAcc = g.accessors.length();
        a.bufferView = g.bufferViews.length(); a.componentType = gltf::accessor::FLOAT; a.type = gltf::accessor::VEC2;
        a.count = va.verts; a.normalized = false; g.accessors.add(a);
        bv.byteLength = vsize - (int)offsetof(vertex, tc); bv.byteOffset = binsize + (int)offsetof(vertex, tc); g.bufferViews.add(bv);

        int lmAcc = g.accessors.length();
        a.bufferView = g.bufferViews.length(); a.componentType = gltf::accessor::SHORT; a.type = gltf::accessor::VEC2;
        a.count = va.verts; a.normalized = true; g.accessors.add(a);
        bv.byteLength = vsize - (int)offsetof(vertex, lm); bv.byteOffset = binsize + (int)offsetof(vertex, lm); g.bufferViews.add(bv);

        // TANGENT is intentionally not exported: Sauer leaves unused surface tangents at zero
        // (LMID_AMBIENT / LMID_BRIGHT placeholders) which glTF 2.0 rejects as non-unit. Viewers
        // synthesize tangents from normals + UVs when needed for normal mapping, so dropping the
        // attribute is the cleanest path. The stride still skips over the bytes in the bin file.
        binsize += vsize;

        int binpos = binsize;
        loopj(va.texs)
        {
            elementset &es = va.eslist[j];

            gltf::material mat;
            VSlot &v = lookupvslot(es.texture);
            if(v.slot && v.slot->shader && v.slot->shader->name)
                copystring(mat.name, v.slot->shader->name);

            loopvj(v.slot->sts) {
                Slot::Tex &st = v.slot->sts[j];
                if(st.type != TEX_DIFFUSE && st.type != TEX_NORMAL && st.type != TEX_GLOW &&
                   st.type != TEX_SPEC && st.type != TEX_DEPTH) continue;
                defformatstring(imgname, "packages/%s", st.name);
                gltf::texture tex;
                tex.sampler = 0;
                tex.source = g.getOrAddImage(resolvediskuri(imgname));
                copystring(tex.name, st.name);
                int t = g.getOrAddTexture(tex);
                switch(st.type) {
                    case TEX_DIFFUSE:
                        mat.pbrMetallicRoughness.baseColorTexture.index = t;
                        mat.pbrMetallicRoughness.baseColorTexture.texCoord = 0;
                        mat.pbrMetallicRoughness.baseColorFactor = vec4(v.colorscale.r, v.colorscale.g, v.colorscale.b, 1);
                        mat.pbrMetallicRoughness.metallicFactor = 0.0;
                        mat.pbrMetallicRoughness.roughnessFactor = 0.85;
                        break;
                    case TEX_NORMAL:
                        mat.normalTexture.index = t;
                        mat.normalTexture.scale = 1.0;
                        mat.normalTexture.texCoord = 0;
                        break;
                    case TEX_SPEC:
                        mat.extensions.KHR_materials_specular.specularTexture.index = t;
                        mat.extensions.KHR_materials_specular.specularTexture.texCoord = 0;
                        mat.extensions.KHR_materials_specular.specularFactor = 0.8;
                        break;
                    case TEX_DEPTH:
                        mat.heightMap.index = t;
                        break;
                    case TEX_GLOW:
                        // glow gets eaten by lightmap below; we still record the texture for re-use later.
                        break;
                }
            }

            // HDR LIGHTMAP -> emissive (texcoord_1, KHR_materials_emissive_strength).
            int lmTexIndex = getlmtexture(es.lmid);
            if(lmTexIndex >= 0) {
                mat.emissiveTexture.index = lmTexIndex;
                mat.emissiveTexture.texCoord = 1;
                mat.emissiveFactor = vec(1, 1, 1);
                mat.emissiveStrength = getlmstrength(es.lmid);
                mat.sauerLmid = es.lmid;
                copystring(mat.name, tempformatstring("%s_lm%d",
                    v.slot && v.slot->shader && v.slot->shader->name ? v.slot->shader->name : "mat",
                    es.lmid));
            }

            gltf::primitive p;
            p.attributes.position    = posAcc;
            p.attributes.normal      = normAcc;
            p.attributes.texcoord_0  = tcAcc;
            p.attributes.texcoord_1  = lmAcc;
            // tangent intentionally omitted; see comment above where the bin buffer is laid out
            p.indices                = g.accessors.length();
            p.material               = g.getOrAddMaterial(mat);
            octreemesh.primitives.add(p);

            int ebytelength = es.length[1] * sizeof(ushort);
            a.bufferView = g.bufferViews.length(); a.componentType = gltf::accessor::UNSIGNED_SHORT; a.type = gltf::accessor::SCALAR;
            a.count = es.length[1]; a.minmax = false; a.normalized = false; g.accessors.add(a);
            bv.byteLength = ebytelength; bv.byteOffset = binpos; bv.target = gltf::bufferView::ELEMENT_ARRAY_BUFFER; bv.byteStride = 0; g.bufferViews.add(bv);
            bv.target = gltf::bufferView::ARRAY_BUFFER; bv.byteStride = sizeof(vertex);   // restore for the next va slot
            binpos += ebytelength;
        }

        loopj(3 * va.tris) edata[j] -= va.voffset;

        int esize = va.tris * 3 * sizeof(ushort);
        r = b->write(edata, esize);
        if(r != esize) conoutf(CON_WARN, "writegltf: short write for elements (%d/%d)", r, esize);
        int padded = (esize + 3) & ~3;
        if(padded > esize) { char pad[4] = {0,0,0,0}; b->write(pad, padded - esize); esize = padded; }
        binsize += esize;

        delete[] edata;
        delete[] vdata;
    }

    {
        gltf::node n;
        n.mesh = g.meshes.length();
        n.light = -1;
        copystring(n.name, "Octree");
        n.rotation = vec4(0.5f, -0.5f, 0.5f, 0.5f);
        n.scale = vec(-0.0625f, -0.0625f, -0.0625f);
        n.translation = vec(0, 0, 0);
        g.nodes.add(n);
    }
    g.meshes.add(octreemesh);

    // ------- sun light --------------------------------------------------------------------------
    if(sunlightcolor.r || sunlightcolor.g || sunlightcolor.b) {
        float pitch = (sunlightpitch - 90) * PI / 360.0f;
        float yaw   = sunlightyaw * PI / 360.0f;
        float cy = cosf(yaw), sy = sinf(yaw);
        float cp = cosf(pitch), sp = sinf(pitch);
        g.nodes[0].children.add(g.nodes.length());
        gltf::node sn;
        sn.mesh = -1; sn.light = g.lights.length();
        copystring(sn.name, "Sunlight");
        sn.rotation = vec4(sp*cy, sp*sy, cp*sy, cp*cy);
        sn.scale = vec(1, 1, 1);
        sn.translation = vec(sunlightent.o.x, sunlightent.o.y, sunlightent.o.z);
        g.nodes.add(sn);
        gltf::light L;
        L.intensity = 2 * 683.0f;
        L.color = vec(sunlightcolor.r/255.0f, sunlightcolor.g/255.0f, sunlightcolor.b/255.0f);
        L.type = gltf::light::DIRECTIONAL;
        copystring(L.name, "Sunlight");
        L.range = 0; L.radius = 0;
        g.lights.add(L);
    }

    // ------- point lights -----------------------------------------------------------------------
    preloadusedmapmodels();
    const vector<extentity*>& ents = entities::getents();
    loopv(ents)
    {
        const extentity &ent = *ents[i];
        if(ent.type == ET_LIGHT) {
            if(ent.attr1 <= 0) continue;
            g.nodes[0].children.add(g.nodes.length());
            gltf::node ln;
            ln.mesh = -1; ln.light = g.lights.length();
            copystring(ln.name, "Light");
            ln.rotation = vec4(0, 0, 0, 1);
            ln.scale = vec(1, 1, 1);
            ln.translation = vec(ent.o.x, ent.o.y, ent.o.z);
            g.nodes.add(ln);
            gltf::light L;
            L.intensity = ent.attr1 * 2.0f / (4 * PI) * 683.0f;
            L.color = vec(ent.attr2/255.0f, ent.attr3/255.0f, ent.attr4/255.0f);
            L.type = gltf::light::POINT;
            copystring(L.name, "Light");
            L.range = (float)ent.attr1; L.radius = 0;
            g.lights.add(L);
        }
        else if(ent.type == ET_MAPMODEL)
        {
            // Defer to rendermodel.cpp -- it owns the animmodel/vertmodel/skelmodel headers
            // (which declare file-scope VARs and static class members and can therefore only
            // live in one TU). The helper appends the model's part nodes/meshes/primitives
            // and updates binsize for any vertex/index bytes it writes.
            gltf_emit_mapmodel(g, b, binsize, ent, /*rootnode=*/ 0);
            continue;
        }
    }

    {
        gltf::buffer gb;
        gb.byteLength = binsize;
        defformatstring(binuri, "%s.bin", basenameuri(name));
        copystring(gb.uri, binuri);
        g.buffers.add(gb);
    }
    delete b;
    conoutf("writegltf: wrote %s (%d bytes)", binname, binsize);

    defformatstring(fname, "%s.gltf", name);
    path(fname);
    conoutf("writegltf: writing %s", fname);
    g.write(fname);
}

COMMAND(writegltf, "s");
