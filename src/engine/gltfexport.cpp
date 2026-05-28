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

// engine internals we lean on
extern vector<vtxarray *> valist;
extern bool readva(vtxarray *va, ushort *&edata, vertex *&vdata);
extern void savepng(const char *filename, ImageData &image, bool flip);
extern entity sunlightent;
extern bvec sunlightcolor;
extern int sunlightpitch, sunlightyaw;
extern vector<LightMap> lightmaps;

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

static bool export_lmpage(const char *name, int lmid, lmexport &out)
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
    // crude 99th-percentile via full sort -- atlas pages are 512x512=262k floats, sub-100ms
    vector<float> tmp;
    tmp.put(lum.getbuf(), lum.length());
    quicksort(tmp.getbuf(), tmp.length(), sortless());
    float peak = tmp[int(tmp.length()*0.99f)];
    if(peak < 1.0f) peak = 1.0f;       // never DARKEN an already-LDR atlas page

    // Second pass: write the LDR PNG.
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

    // Third pass: write the linear-HDR Radiance .hdr from baked RGBE bytes (or re-encode from
    // decoded vec for LDR/non-HDR atlas pages so the .hdr is always linear-light).
    LightMap &lm = lightmaps[idx];
    defformatstring(hdrname, "%s_lm%d.hdr", name, lmid);
    if((lm.type & LM_HDR) && (lm.type & LM_TYPE) != LM_RNM0)
    {
        savehdr(hdrname, LM_PACKW, LM_PACKH, lm.data);
    }
    else
    {
        uchar *rgbe = new uchar[LM_PACKW*LM_PACKH*4];
        loopi(LM_PACKH) loopj(LM_PACKW)
        {
            vec c = lm_decode_blend(lmid, j, i);
            encodergbe(c, rgbe + (i*LM_PACKW + j)*4);
        }
        savehdr(hdrname, LM_PACKW, LM_PACKH, rgbe);
        delete[] rgbe;
    }
    defformatstring(hdruri, "%s_lm%d.hdr", basenameuri(name), lmid);
    copystring(out.hdruri, hdruri);

    out.lmid = lmid;
    out.peak = peak;
    return true;
}

// -----------------------------------------------------------------------------------------------
// glTF JSON model -- straight port of the upstream layout, with the field-name bug in material's
// emissive block fixed (was writing "metallicFactor" for emissiveFactor) and KHR_materials_emissive_strength
// added so HDR LM data round-trips through the exporter.
// -----------------------------------------------------------------------------------------------

struct gltf {
    struct light {
        float intensity;
        vec color;
        enum { POINT, DIRECTIONAL } type;
        string name;
        float range, radius;

        void serialize(stream *f) {
            f->printf(
                "{"
                    "\"intensity\":%f,"
                    "\"color\":[%f,%f,%f],"
                    "\"type\":\"%s\","
                    "\"name\":\"%s\"",
                intensity,
                color.r, color.g, color.b,
                type == DIRECTIONAL ? "directional" : "point",
                name
            );
            if(type == POINT)
                f->printf(",\"extras\":{\"radius\":%f},\"range\":%f", radius, range);
            f->printf("}");
        }
    };

    struct scene {
        string name;
        vector<int> nodes;

        void serialize(stream *f) {
            f->printf("{\"name\":\"%s\"", name);
            if(nodes.length() > 0) {
                f->printf(",\"nodes\":[");
                loopv(nodes) { if(i > 0) f->printf(","); f->printf("%d", nodes[i]); }
                f->printf("]");
            }
            f->printf("}");
        }
    };

    struct node {
        int mesh, light;
        string name;
        vec4 rotation;
        vec scale;
        vec translation;
        vector<int> children;

        void serialize(stream *f) {
            f->printf("{\"name\":\"%s\"", name);
            if(light >= 0)
                f->printf(",\"extensions\": {\"KHR_lights_punctual\": {\"light\": %d}}", light);
            if(mesh >= 0)
                f->printf(",\"mesh\":%d", mesh);
            if(translation.x || translation.y || translation.z)
                f->printf(",\"translation\": [%f,%f,%f]", translation.x, translation.y, translation.z);
            if(scale.x || scale.y || scale.z)
                f->printf(",\"scale\": [%f,%f,%f]", scale.x, scale.y, scale.z);
            if(rotation.x || rotation.y || rotation.z || rotation.w)
                f->printf(",\"rotation\": [%f,%f,%f,%f]", rotation.x, rotation.y, rotation.z, rotation.w);
            if(children.length() > 0) {
                f->printf(",\"children\":[");
                loopvj(children) { if(j > 0) f->printf(","); f->printf("%d", children[j]); }
                f->printf("]");
            }
            f->printf("}");
        }
    };

    struct primitive {
        struct {
            int position, normal, tangent, texcoord_0, texcoord_1;
        } attributes;
        int indices, material;

        primitive() : indices(-1), material(-1) {
            attributes.position = attributes.normal = attributes.tangent = -1;
            attributes.texcoord_0 = attributes.texcoord_1 = -1;
        }

        void serialize(stream *f) {
            f->printf("{\"indices\":%d,\"material\":%d", indices, material);
            if(attributes.position >= 0 || attributes.normal >= 0 || attributes.tangent >= 0 ||
               attributes.texcoord_0 >= 0 || attributes.texcoord_1 >= 0)
            {
                f->printf(",\"attributes\":{");
                int k = 0;
                if(attributes.position   >= 0) { if(k++) f->printf(","); f->printf("\"POSITION\":%d",   attributes.position);   }
                if(attributes.normal     >= 0) { if(k++) f->printf(","); f->printf("\"NORMAL\":%d",     attributes.normal);     }
                if(attributes.tangent    >= 0) { if(k++) f->printf(","); f->printf("\"TANGENT\":%d",    attributes.tangent);    }
                if(attributes.texcoord_0 >= 0) { if(k++) f->printf(","); f->printf("\"TEXCOORD_0\":%d", attributes.texcoord_0); }
                if(attributes.texcoord_1 >= 0) { if(k++) f->printf(","); f->printf("\"TEXCOORD_1\":%d", attributes.texcoord_1); }
                f->printf("}");
            }
            f->printf("}");
        }
    };

    struct mesh {
        string name;
        vector<gltf::primitive> primitives;

        void serialize(stream *f) {
            f->printf("{\"name\":\"%s\",\"primitives\":[", name);
            loopv(primitives) { if(i > 0) f->printf(","); primitives[i].serialize(f); }
            f->printf("]}");
        }
    };

    struct accessor {
        int bufferView;
        enum { BYTE=5120, UNSIGNED_BYTE=5121, SHORT=5122, UNSIGNED_SHORT=5123, UNSIGNED_INT=5125, FLOAT=5126 } componentType;
        enum { SCALAR, VEC2, VEC3, VEC4, MAT2, MAT3, MAT4 } type;
        int count;
        vec min, max;
        bool minmax;
        bool normalized;

        const char *getTypeString() {
            static const char *names[] = { "SCALAR","VEC2","VEC3","VEC4","MAT2","MAT3","MAT4" };
            return type < int(sizeof(names)/sizeof(names[0])) ? names[type] : "UNKNOWN";
        }

        void serialize(stream *f) {
            f->printf("{\"bufferView\":%d,\"componentType\":%d,\"count\":%d,\"type\":\"%s\"",
                bufferView, componentType, count, getTypeString());
            if(componentType != UNSIGNED_INT && componentType != FLOAT && normalized)
                f->printf(",\"normalized\":true");
            if(type == VEC3 && componentType == FLOAT && minmax) {
                f->printf(",\"min\":[%f,%f,%f]", min.x, min.y, min.z);
                f->printf(",\"max\":[%f,%f,%f]", max.x, max.y, max.z);
            }
            f->printf("}");
        }
    };

    struct bufferView {
        int buffer;
        int byteLength;
        int byteOffset;
        enum { ARRAY_BUFFER = 34962, ELEMENT_ARRAY_BUFFER = 34963 } target;
        int byteStride;

        void serialize(stream *f) {
            f->printf("{\"buffer\":%d,\"byteLength\":%d,\"byteOffset\":%d,\"target\":%d",
                buffer, byteLength, byteOffset, target);
            if(byteStride > 0) f->printf(",\"byteStride\":%d", byteStride);
            f->printf("}");
        }
    };

    struct material {
        bool doubleSided;
        string name;
        enum { OPAQUE, MASK, BLEND } alphaMode;
        struct {
            struct { int index; int texCoord; } baseColorTexture;
            vec4 baseColorFactor;
            float metallicFactor;
            float roughnessFactor;
        } pbrMetallicRoughness;

        struct { int index; float scale; int texCoord; } normalTexture;
        struct { int index; int texCoord; } emissiveTexture;
        vec emissiveFactor;
        float emissiveStrength;                 // KHR_materials_emissive_strength

        struct {
            struct {
                struct { int index; int texCoord; } specularTexture;
                float specularFactor;
            } KHR_materials_specular;
        } extensions;

        struct { int index; } heightMap;

        material() {
            doubleSided = false;
            name[0] = 0;
            alphaMode = OPAQUE;
            pbrMetallicRoughness.baseColorTexture.index = -1;
            pbrMetallicRoughness.baseColorTexture.texCoord = 0;
            pbrMetallicRoughness.baseColorFactor = vec4(1, 1, 1, 1);
            pbrMetallicRoughness.metallicFactor = 1.0;
            pbrMetallicRoughness.roughnessFactor = 1.0;
            normalTexture.index = -1;
            normalTexture.scale = -1;
            normalTexture.texCoord = -1;
            emissiveTexture.index = -1;
            emissiveTexture.texCoord = -1;
            emissiveFactor = vec(0, 0, 0);
            emissiveStrength = 1.0f;
            extensions.KHR_materials_specular.specularTexture.index = -1;
            extensions.KHR_materials_specular.specularTexture.texCoord = -1;
            extensions.KHR_materials_specular.specularFactor = 1.0;
            heightMap.index = -1;
        }

        const char *getAlphaModeString() {
            static const char *names[] = { "OPAQUE", "MASK", "BLEND" };
            return alphaMode < int(sizeof(names)/sizeof(names[0])) ? names[alphaMode] : "UNKNOWN";
        }

        void serialize(stream *f) {
            f->printf("{\"name\":\"%s\",\"doubleSided\":%s,\"alphaMode\":\"%s\"",
                name, doubleSided ? "true" : "false", getAlphaModeString());

            if(pbrMetallicRoughness.baseColorFactor.r != 1 ||
               pbrMetallicRoughness.baseColorFactor.g != 1 ||
               pbrMetallicRoughness.baseColorFactor.b != 1 ||
               pbrMetallicRoughness.baseColorFactor.a != 1 ||
               pbrMetallicRoughness.metallicFactor != 1 ||
               pbrMetallicRoughness.roughnessFactor != 1 ||
               pbrMetallicRoughness.baseColorTexture.index >= 0)
            {
                f->printf(",\"pbrMetallicRoughness\":{");
                int c = 0;
                if(pbrMetallicRoughness.baseColorTexture.index >= 0) {
                    if(c++) f->printf(",");
                    f->printf("\"baseColorTexture\":{\"index\":%d,\"texCoord\":%d}",
                        pbrMetallicRoughness.baseColorTexture.index,
                        pbrMetallicRoughness.baseColorTexture.texCoord);
                }
                if(pbrMetallicRoughness.baseColorFactor.r != 1 ||
                   pbrMetallicRoughness.baseColorFactor.g != 1 ||
                   pbrMetallicRoughness.baseColorFactor.b != 1 ||
                   pbrMetallicRoughness.baseColorFactor.a != 1)
                {
                    if(c++) f->printf(",");
                    f->printf("\"baseColorFactor\":[%f,%f,%f,%f]",
                        pbrMetallicRoughness.baseColorFactor.r, pbrMetallicRoughness.baseColorFactor.g,
                        pbrMetallicRoughness.baseColorFactor.b, pbrMetallicRoughness.baseColorFactor.a);
                }
                if(pbrMetallicRoughness.metallicFactor != 1) {
                    if(c++) f->printf(",");
                    f->printf("\"metallicFactor\":%f", pbrMetallicRoughness.metallicFactor);
                }
                if(pbrMetallicRoughness.roughnessFactor != 1) {
                    if(c++) f->printf(",");
                    f->printf("\"roughnessFactor\":%f", pbrMetallicRoughness.roughnessFactor);
                }
                f->printf("}");
            }

            if(normalTexture.index >= 0) {
                f->printf(",\"normalTexture\":{\"index\":%d", normalTexture.index);
                if(normalTexture.texCoord >= 0) f->printf(",\"texCoord\":%d", normalTexture.texCoord);
                f->printf("}");
            }

            if(emissiveTexture.index >= 0) {
                f->printf(",\"emissiveTexture\":{\"index\":%d", emissiveTexture.index);
                if(emissiveTexture.texCoord >= 0) f->printf(",\"texCoord\":%d", emissiveTexture.texCoord);
                f->printf("}");
            }

            if(emissiveFactor.r != 0 || emissiveFactor.g != 0 || emissiveFactor.b != 0)
                f->printf(",\"emissiveFactor\":[%f,%f,%f]",
                    emissiveFactor.r, emissiveFactor.g, emissiveFactor.b);

            // extensions block: specular + emissive_strength
            bool hasSpec = extensions.KHR_materials_specular.specularFactor != 1.0 ||
                           extensions.KHR_materials_specular.specularTexture.index >= 0;
            bool hasEmissiveStrength = emissiveTexture.index >= 0 && emissiveStrength != 1.0f;
            if(hasSpec || hasEmissiveStrength) {
                f->printf(",\"extensions\":{");
                int c = 0;
                if(hasSpec) {
                    if(c++) f->printf(",");
                    f->printf("\"KHR_materials_specular\":{");
                    int c2 = 0;
                    if(extensions.KHR_materials_specular.specularFactor != 1.0) {
                        if(c2++) f->printf(",");
                        f->printf("\"specularFactor\":%f", extensions.KHR_materials_specular.specularFactor);
                    }
                    if(extensions.KHR_materials_specular.specularTexture.index >= 0) {
                        if(c2++) f->printf(",");
                        f->printf("\"specularTexture\":{\"index\":%d", extensions.KHR_materials_specular.specularTexture.index);
                        if(extensions.KHR_materials_specular.specularTexture.texCoord >= 0)
                            f->printf(",\"texCoord\":%d", extensions.KHR_materials_specular.specularTexture.texCoord);
                        f->printf("}");
                    }
                    f->printf("}");
                }
                if(hasEmissiveStrength) {
                    if(c++) f->printf(",");
                    f->printf("\"KHR_materials_emissive_strength\":{\"emissiveStrength\":%f}", emissiveStrength);
                }
                f->printf("}");
            }
            if(heightMap.index >= 0)
                f->printf(",\"heightMap\":{\"index\":%d}", heightMap.index);
            f->printf("}");
        }
    };

    struct image {
        string uri;
        void serialize(stream *f) { f->printf("{\"uri\":\"%s\"}", uri); }
    };

    struct sampler {
        enum {
            NEAREST=9728, LINEAR=9729,
            NEAREST_MIPMAP_NEAREST=9984, LINEAR_MIPMAP_NEAREST=9985,
            NEAREST_MIPMAP_LINEAR=9986, LINEAR_MIPMAP_LINEAR=9987,
        } minFilter, magFilter;
        enum { CLAMP_TO_EDGE=33071, MIRRORED_REPEAT=33648, REPEAT=10497 } wrapS, wrapT;
        string name;

        sampler() : minFilter(LINEAR), magFilter(LINEAR), wrapS(REPEAT), wrapT(REPEAT) { name[0] = 0; }

        void serialize(stream *f) {
            f->printf("{");
            int c = 0;
            if(name[0])              { if(c++) f->printf(","); f->printf("\"name\":\"%s\"", name); }
            if(magFilter != LINEAR)  { if(c++) f->printf(","); f->printf("\"magFilter\":%d", magFilter); }
            if(minFilter != LINEAR)  { if(c++) f->printf(","); f->printf("\"minFilter\":%d", minFilter); }
            if(wrapS != REPEAT)      { if(c++) f->printf(","); f->printf("\"wrapS\":%d", wrapS); }
            if(wrapT != REPEAT)      { if(c++) f->printf(","); f->printf("\"wrapT\":%d", wrapT); }
            f->printf("}");
        }
    };

    struct texture {
        int sampler, source;
        string name;
        void serialize(stream *f) { f->printf("{\"sampler\":%d,\"source\":%d,\"name\":\"%s\"}", sampler, source, name); }
    };

    struct buffer {
        int byteLength;
        string uri;
        void serialize(stream *f) { f->printf("{\"uri\":\"%s\",\"byteLength\":%d}", uri, byteLength); }
    };

    vector<gltf::scene>      scenes;
    vector<gltf::light>      lights;
    vector<gltf::node>       nodes;
    vector<gltf::mesh>       meshes;
    vector<gltf::accessor>   accessors;
    vector<gltf::bufferView> bufferViews;
    vector<gltf::material>   materials;
    vector<gltf::image>      images;
    vector<gltf::texture>    textures;
    vector<gltf::buffer>     buffers;
    vector<gltf::sampler>    samplers;

    int write(const char *fname) {
        stream *f = openfile(fname, "w");
        if(!f) { conoutf(CON_ERROR, "Could not open %s", fname); return 1; }

        f->printf(
            "{"
                "\"asset\": {\"generator\": \"Cube 2: Sauerbraten (HDR fork)\",\"version\": \"2.0\"},"
                "\"extensionsUsed\": [\"KHR_materials_specular\",\"KHR_mesh_quantization\",\"KHR_lights_punctual\",\"KHR_materials_emissive_strength\"],"
                "\"extensionsRequired\": [\"KHR_mesh_quantization\"],"
                "\"scene\": 0"
        );

        #define EMIT_ARRAY(key, arr) \
            do { if((arr).length() > 0) { \
                f->printf(",\"" key "\":["); \
                loopv(arr) { if(i > 0) f->printf(","); (arr)[i].serialize(f); } \
                f->printf("]"); \
            } } while(0)

        EMIT_ARRAY("scenes", scenes);
        if(lights.length() > 0) {
            f->printf(",\"extensions\":{\"KHR_lights_punctual\":{\"lights\":[");
            loopv(lights) { if(i > 0) f->printf(","); lights[i].serialize(f); }
            f->printf("]}}");
        }
        EMIT_ARRAY("nodes",       nodes);
        EMIT_ARRAY("meshes",      meshes);
        EMIT_ARRAY("accessors",   accessors);
        EMIT_ARRAY("bufferViews", bufferViews);
        EMIT_ARRAY("materials",   materials);
        EMIT_ARRAY("images",      images);
        EMIT_ARRAY("textures",    textures);
        EMIT_ARRAY("samplers",    samplers);
        EMIT_ARRAY("buffers",     buffers);

        #undef EMIT_ARRAY

        f->printf("}");
        delete f;
        return 0;
    }

    int getOrAddTexture(gltf::texture &tex) {
        loopv(textures) {
            if(strcmp(textures[i].name, tex.name)) continue;
            if(textures[i].sampler != tex.sampler) continue;
            if(textures[i].source != tex.source) continue;
            return i;
        }
        textures.add(tex);
        return textures.length() - 1;
    }

    int getOrAddImage(const char *uri) {
        loopv(images) if(!strcmp(images[i].uri, uri)) return i;
        image img;
        copystring(img.uri, uri);
        images.add(img);
        return images.length() - 1;
    }

    int getOrAddMaterial(gltf::material &mat) {
        loopv(materials) {
            material &m = materials[i];
            if(m.doubleSided != mat.doubleSided) continue;
            if(strcmp(m.name, mat.name)) continue;
            if(m.pbrMetallicRoughness.baseColorTexture.index   != mat.pbrMetallicRoughness.baseColorTexture.index)   continue;
            if(m.pbrMetallicRoughness.baseColorTexture.texCoord!= mat.pbrMetallicRoughness.baseColorTexture.texCoord)continue;
            if(m.pbrMetallicRoughness.baseColorFactor.r        != mat.pbrMetallicRoughness.baseColorFactor.r)        continue;
            if(m.pbrMetallicRoughness.baseColorFactor.g        != mat.pbrMetallicRoughness.baseColorFactor.g)        continue;
            if(m.pbrMetallicRoughness.baseColorFactor.b        != mat.pbrMetallicRoughness.baseColorFactor.b)        continue;
            if(m.pbrMetallicRoughness.baseColorFactor.a        != mat.pbrMetallicRoughness.baseColorFactor.a)        continue;
            if(m.pbrMetallicRoughness.metallicFactor           != mat.pbrMetallicRoughness.metallicFactor)           continue;
            if(m.pbrMetallicRoughness.roughnessFactor          != mat.pbrMetallicRoughness.roughnessFactor)          continue;
            if(m.normalTexture.index                           != mat.normalTexture.index)                           continue;
            if(m.normalTexture.scale                           != mat.normalTexture.scale)                           continue;
            if(m.normalTexture.texCoord                        != mat.normalTexture.texCoord)                        continue;
            if(m.emissiveTexture.index                         != mat.emissiveTexture.index)                         continue;
            if(m.emissiveTexture.texCoord                      != mat.emissiveTexture.texCoord)                      continue;
            if(m.emissiveFactor                                != mat.emissiveFactor)                                continue;
            if(m.emissiveStrength                              != mat.emissiveStrength)                              continue;
            if(m.extensions.KHR_materials_specular.specularTexture.index    != mat.extensions.KHR_materials_specular.specularTexture.index)    continue;
            if(m.extensions.KHR_materials_specular.specularTexture.texCoord != mat.extensions.KHR_materials_specular.specularTexture.texCoord) continue;
            if(m.extensions.KHR_materials_specular.specularFactor           != mat.extensions.KHR_materials_specular.specularFactor)           continue;
            if(m.heightMap.index                                            != mat.heightMap.index)                                            continue;
            return i;
        }
        materials.add(mat);
        return materials.length() - 1;
    }
};

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

    // ------- lmid -> texture index cache, fills on demand from VA primitives --------------------
    struct lmrec { int lmid; int textureIndex; float strength; };
    vector<lmrec> lmcache;
    auto getlmtexture = [&](int lmid) -> int {
        if(lmid < LMID_RESERVED) return -1;
        loopv(lmcache) if(lmcache[i].lmid == lmid) return lmcache[i].textureIndex;
        lmexport e;
        if(!export_lmpage(name, lmid, e)) return -1;
        gltf::texture lmtex;
        defformatstring(lmtexname, "lm%d", lmid);
        copystring(lmtex.name, lmtexname);
        lmtex.sampler = 0;
        lmtex.source = g.getOrAddImage(e.pnguri);
        int ti = g.getOrAddTexture(lmtex);
        lmrec rec = { lmid, ti, e.peak };
        lmcache.add(rec);
        conoutf("writegltf: lmid %d -> %s (peak %.2f)", lmid, e.pnguri, e.peak);
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
        loopj(va.verts) {
            // svec2 lm is normalized [0,1] over the atlas in a half-open Y-down range. Match
            // upstream: rescale x ~ [0,2] then flip y. (texcoord_1 sampler is REPEAT.)
            vdata[j].lm.x *= 2;
            vdata[j].lm.y = (SHRT_MAX - vdata[j].lm.y) * 2;

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
                tex.source = g.getOrAddImage(imgname);
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
        // (Mapmodel export omitted in this first pass: the upstream version walked vertmesh/skelmesh
        //  internals directly. Re-adding that requires more skinned-mesh wiring; world+lights+LMs is
        //  the useful piece for visualizing baked HDR lighting.)
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
