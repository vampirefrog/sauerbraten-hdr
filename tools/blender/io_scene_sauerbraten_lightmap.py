bl_info = {
    "name": "Sauerbraten Lightmap glTF Hook",
    "author": "Sauerbraten HDR",
    "version": (1, 0, 0),
    "blender": (4, 0, 0),
    "location": "File > Import > glTF 2.0  (panel: Sauerbraten Lightmap)",
    "description": (
        "Re-routes Sauerbraten lightmaps (SAUER_lightmap extension) out of the "
        "Principled BSDF emissive slot and into a Mix Shader so Cycles bounce "
        "lighting stays usable. Optionally hot-swaps the tonemapped PNG for the "
        "linear-light Radiance .hdr sidecar."
    ),
    "category": "Import-Export",
    "support": "COMMUNITY",
}

import os
import bpy
from bpy.props import BoolProperty, EnumProperty, PointerProperty
from bpy.types import PropertyGroup


EXTENSION_NAME = "SAUER_lightmap"
PROPS_ATTR = "sauer_gltf_import_props"


# ---------------------------------------------------------------------------
# Settings live on WindowManager rather than on the operator class — Blender's
# operator-property machinery doesn't accept extra properties bolted on from
# another addon, so we use a side-channel PropertyGroup that the file-browser
# panel and the import hook both read.
# ---------------------------------------------------------------------------

class SauerGltfImportProps(PropertyGroup):
    enabled: BoolProperty(
        name="Import Sauer lightmaps",
        description=(
            "Rewire materials tagged with SAUER_lightmap so the lightmap multiplies the "
            "diffuse into a Mix-Shader emission branch instead of being plugged straight "
            "into the Principled BSDF's emissive slot."
        ),
        default=True,
    )
    source: EnumProperty(
        name="Lightmap source",
        description="Which lightmap file to point the material at",
        items=(
            ("auto", "Auto (HDR if available)",
             "Use the linear-light .hdr sidecar when present; fall back to the tonemapped PNG"),
            ("hdr", "HDR only (.hdr)",
             "Force the linear-light Radiance .hdr sidecar; warns to console if missing"),
            ("ldr", "LDR only (.png)",
             "Keep the tonemapped PNG (default Blender import). Best for viewport speed"),
        ),
        default="auto",
    )


def _props():
    wm = bpy.context.window_manager
    if wm is None:
        return None
    return getattr(wm, PROPS_ATTR, None)


def _import_enabled():
    p = _props()
    return True if p is None else bool(p.enabled)


def _hdr_source():
    p = _props()
    return "auto" if p is None else p.source


# ---------------------------------------------------------------------------
# Material rewire
# ---------------------------------------------------------------------------

def _find_node(nt, type_):
    return next((n for n in nt.nodes if n.type == type_), None)


def _active_output(nt):
    n = next((n for n in nt.nodes if n.type == "OUTPUT_MATERIAL" and n.is_active_output), None)
    return n or _find_node(nt, "OUTPUT_MATERIAL")


def _trace_image_node(socket):
    if not socket or not socket.is_linked:
        return None
    seen = set()
    queue = [link.from_node for link in socket.links]
    while queue:
        n = queue.pop(0)
        if n in seen:
            continue
        seen.add(n)
        if n.type == "TEX_IMAGE":
            return n
        for inp in n.inputs:
            for link in inp.links:
                queue.append(link.from_node)
    return None


def _emission_socket(bsdf):
    # Principled BSDF: 4.x = "Emission Color", pre-4.0 = "Emission"
    return bsdf.inputs.get("Emission Color") or bsdf.inputs.get("Emission")


def _strength_socket(bsdf):
    return bsdf.inputs.get("Emission Strength")


def _swap_to_hdr_sidecar(image, gltf_filepath):
    """Replace `image` with the matching <basename>_lm<N>.hdr sitting next to the .gltf.

    Returns True if a swap happened. We keep the original Blender image datablock
    and just repoint .filepath; this preserves all links into nodes.
    """
    if image is None:
        return False
    src = image.filepath_raw or image.filepath or image.name
    png_name = os.path.basename(src)
    if not png_name.lower().endswith(".png"):
        return False
    hdr_name = png_name[:-4] + ".hdr"
    # Search next to the .gltf file first (where /writegltf put the sidecars),
    # then next to the PNG itself.
    candidates = []
    if gltf_filepath:
        candidates.append(os.path.join(os.path.dirname(gltf_filepath), hdr_name))
    abs_png = bpy.path.abspath(src)
    candidates.append(os.path.join(os.path.dirname(abs_png), hdr_name))
    for hdr_path in candidates:
        if os.path.isfile(hdr_path):
            try:
                image.filepath = bpy.path.relpath(hdr_path) if bpy.data.filepath else hdr_path
            except Exception:
                image.filepath = hdr_path
            image.filepath_raw = image.filepath
            image.source = "FILE"
            image.reload()
            return True
    return False


def _rewire_material(mat, ext_data, gltf_filepath):
    """Convert an emissive-lightmap material into Mix(BSDF, Emission(Base*LM)).

    Layout produced:
                    +----------------+
        BaseColor --+ Multiply (RGB) +--+
        Lightmap --+|                |   |
                    +----------------+   |
                                         |   +-----------+
                                         +--+ Emission  +--+
                                            | strength: |  |  +-----------+
                                            |  peak     |  +--+ Mix       +-- Material Output
                                            +-----------+     | Shader    |
                                                              |  fac=1.0  |
                                  Principled BSDF -----------+|           |
                                                              +-----------+
    """
    if not mat or not mat.use_nodes:
        return
    nt = mat.node_tree
    out = _active_output(nt)
    bsdf = _find_node(nt, "BSDF_PRINCIPLED")
    if not out or not bsdf:
        return
    em_sock = _emission_socket(bsdf)
    if not em_sock or not em_sock.is_linked:
        return
    lm_node = _trace_image_node(em_sock)
    if not lm_node:
        return
    base_sock = bsdf.inputs.get("Base Color")
    base_node = _trace_image_node(base_sock) if (base_sock and base_sock.is_linked) else None

    # --- Lift the lightmap off the BSDF emissive slot ---
    for link in list(em_sock.links):
        nt.links.remove(link)
    ss = _strength_socket(bsdf)
    peak = 1.0
    if ss is not None:
        peak = float(ss.default_value) if ss.default_value else 1.0
        ss.default_value = 0.0
    # Use the SAUER_lightmap extension as an authoritative fallback if Blender
    # didn't pick up KHR_materials_emissive_strength for some reason.
    if isinstance(ext_data, dict) and "peakLuminance" in ext_data:
        peak = float(ext_data["peakLuminance"]) or peak

    # --- HDR / LDR source selection ---
    # Sauer's world shader writes `2 * diffuse * lm` straight to the framebuffer with no sRGB
    # encode step; the monitor then applies an sRGB EOTF and the result looks gamma-2.2-darker
    # than Blender's "Standard" view transform (which sRGB-encodes its linear render). To match
    # Sauer's display intensity we need `linear_radiance = pow(2 * diff/255 * lm/255, 2.2)`.
    # We get there by:
    #   * sampling the LDR PNG lightmap as **sRGB** so Blender's decode applies `pow(byte/255, 2.2)`
    #     (= the per-channel gamma2.2 we need on lm);
    #   * applying the outer `2^2.2` factor via emissive strength (~ 4.594) which the exporter
    #     already wrote as `pow(2.0, 2.2)` for LDR atlases, `pow(peak, 2.2)` for HDR atlases /
    #     RNM pages.
    # When we hot-swap to the linear-light .hdr sidecar the gamma path doesn't apply -- the
    # colourspace flips back to Non-Color and strength resets to `2^2.2` (overbright in linear).
    src = _hdr_source()
    if lm_node.image is not None:
        lm_node.image.colorspace_settings.name = "sRGB"
        lm_node.label = "Sauer Lightmap"
        if src == "hdr" or src == "auto":
            swapped = _swap_to_hdr_sidecar(lm_node.image, gltf_filepath)
            if swapped:
                # .hdr sidecar holds true linear radiance; don't re-decode as sRGB.
                lm_node.image.colorspace_settings.name = "Non-Color"
                peak = 2.0 ** 2.2   # the diffuse-side 2x overbright in linear radiance space
            elif src == "hdr":
                print(f"[SAUER_lightmap] {mat.name}: requested HDR but no .hdr sidecar found; staying on PNG.")

    # --- Build the lightmap branch ---
    mult = nt.nodes.new("ShaderNodeMixRGB")
    mult.blend_type = "MULTIPLY"
    mult.inputs["Fac"].default_value = 1.0
    mult.label = "Base x Lightmap"
    mult.location = (lm_node.location.x + 240, lm_node.location.y - 180)
    if base_node is not None:
        nt.links.new(base_node.outputs["Color"], mult.inputs["Color1"])
    else:
        # No base colour texture; multiply against the BSDF base-colour default.
        mult.inputs["Color1"].default_value = tuple(bsdf.inputs["Base Color"].default_value)
    nt.links.new(lm_node.outputs["Color"], mult.inputs["Color2"])

    emit = nt.nodes.new("ShaderNodeEmission")
    emit.label = "Sauer Lightmap Emission"
    emit.location = (mult.location.x + 240, mult.location.y)
    emit.inputs["Strength"].default_value = peak
    nt.links.new(mult.outputs["Color"], emit.inputs["Color"])

    mix = nt.nodes.new("ShaderNodeMixShader")
    mix.label = "Sauer Lightmap Mix"
    mix.location = (out.location.x - 240, out.location.y)
    mix.inputs[0].default_value = 1.0  # 1.0 = pure baked lighting; user can dial down

    surf = out.inputs["Surface"]
    for link in list(surf.links):
        nt.links.remove(link)
    nt.links.new(bsdf.outputs["BSDF"], mix.inputs[1])
    nt.links.new(emit.outputs["Emission"], mix.inputs[2])
    nt.links.new(mix.outputs["Shader"], surf)


# ---------------------------------------------------------------------------
# glTF importer extension hook
# ---------------------------------------------------------------------------

class glTF2ImportUserExtension:
    def __init__(self):
        # Some Blender API surfaces look for this attribute.
        try:
            from io_scene_gltf2.io.com.gltf2_io_extensions import Extension
            self.Extension = Extension
        except Exception:
            self.Extension = None

    def gather_import_material_after_hook(self, gltf_material, vertex_color, blender_mat, gltf):
        if not _import_enabled():
            return
        exts = getattr(gltf_material, "extensions", None) or {}
        if EXTENSION_NAME not in exts:
            return
        ext_data = exts.get(EXTENSION_NAME) or {}
        gltf_filepath = getattr(gltf, "filename", "") or ""
        if not gltf_filepath:
            try:
                gltf_filepath = gltf.import_settings.get("filepath", "") or ""
            except Exception:
                pass
        _rewire_material(blender_mat, ext_data, gltf_filepath)


# ---------------------------------------------------------------------------
# UI draw callback
#
# Blender's gltf2 importer scans every enabled addon for a module-level
# `draw_import(context, layout)` (or `draw`) and folds it into the file
# browser's "User Extensions" section. That's the *only* place this UI shows
# up in the interactive Import > glTF dialog — a self-registered Panel won't.
# ---------------------------------------------------------------------------

def draw_import(context, layout):
    props = getattr(context.window_manager, PROPS_ATTR, None)
    box = layout.box()
    box.label(text="Sauerbraten Lightmap", icon="LIGHT")
    if props is None:
        box.label(text="(props not registered)")
        return
    box.prop(props, "enabled")
    sub = box.column()
    sub.enabled = props.enabled
    sub.prop(props, "source")


classes = (SauerGltfImportProps,)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    setattr(bpy.types.WindowManager, PROPS_ATTR,
            PointerProperty(type=SauerGltfImportProps))


def unregister():
    try:
        delattr(bpy.types.WindowManager, PROPS_ATTR)
    except Exception:
        pass
    for c in reversed(classes):
        bpy.utils.unregister_class(c)


if __name__ == "__main__":
    register()
