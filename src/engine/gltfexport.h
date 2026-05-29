// gltfexport.h -- the struct gltf data model and small helpers extracted from gltfexport.cpp so
// rendermodel.cpp can emit mapmodel geometry directly (the model headers it owns -- animmodel.h,
// vertmodel.h, skelmodel.h -- declare file-scope VARs and static class members, so they can only
// be included from a single TU; the gltf exporter cannot include them itself).

#ifndef GLTFEXPORT_H
#define GLTFEXPORT_H

// Texture URIs in a glTF resolve relative to the .gltf's location. Sauer writes the .gltf to
// homedir, but the diffuse/normal/glow textures live in <root>/packages/. We resolve each logical
// name to its actual disk path via Sauer's findfile() and then absolutise via realpath(), so the
// importer can find the file no matter where the .gltf was written.
#ifdef _WIN32
#include <stdlib.h>
#define gltf_realpath(p, abs) _fullpath(abs, p, sizeof(abs))
#else
#include <stdlib.h>
#include <limits.h>
#define gltf_realpath(p, abs) realpath(p, abs)
#endif

static inline const char *gltf_resolvediskuri(const char *logical)
{
    static string out;
    const char *disk = findfile(logical, "r");
    if(!disk || !*disk) { copystring(out, logical); return out; }
    char absbuf[4096];
    if(gltf_realpath(disk, absbuf)) { copystring(out, absbuf); return out; }
    copystring(out, disk);
    return out;
}


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
        int sauerLmid;                          // SAUER_lightmap marker (-1 = none)

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
            sauerLmid = -1;
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

            // extensions block: specular + emissive_strength + SAUER_lightmap marker
            bool hasSpec = extensions.KHR_materials_specular.specularFactor != 1.0 ||
                           extensions.KHR_materials_specular.specularTexture.index >= 0;
            bool hasEmissiveStrength = emissiveTexture.index >= 0 && emissiveStrength != 1.0f;
            bool hasSauerLm = sauerLmid >= 0;
            if(hasSpec || hasEmissiveStrength || hasSauerLm) {
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
                if(hasSauerLm) {
                    if(c++) f->printf(",");
                    f->printf("\"SAUER_lightmap\":{\"lmid\":%d}", sauerLmid);
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
                "\"extensionsUsed\": [\"KHR_materials_specular\",\"KHR_mesh_quantization\",\"KHR_lights_punctual\",\"KHR_materials_emissive_strength\",\"SAUER_lightmap\"],"
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
            if(m.sauerLmid                                     != mat.sauerLmid)                                     continue;
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

// Implemented in rendermodel.cpp where the model headers are already included. Walks the
// mapmodel referenced by ent.attr2, adds one glTF node (TRS from the entity), and per-part
// meshes/primitives + accessors/bufferViews (deduping by meshgroup name). Skinning, animation
// and bump tangents are intentionally omitted -- static frame-0 only.
extern void gltf_emit_mapmodel(gltf &g, stream *b, int &binsize, const extentity &ent, int rootnode);

#endif
