# Sauerbraten HDR

An HDR / Half-Life 2–style lighting fork of **Cube 2: Sauerbraten**. It adds a floating-point render
pipeline, HDR + radiosity-normal-mapped lightmaps, baked global illumination, emissive textures and
materials, an ambient-cube light-probe grid for models, and HDR textures/skyboxes — all gated behind
variables so the stock LDR renderer is unchanged by default.

The lighting model follows Mitchell, [*"Shading in Valve's Source Engine"*](https://advances.realtimerendering.com/s2006/Mitchell-ShadingInValvesSourceEngine.pdf)
(SIGGRAPH 2006): an fp16 scene buffer with tonemapping and eye-adaptation, 3-basis radiosity normal
mapping, ambient-cube irradiance volumes, and emissive surfaces as light sources.

## Attribution & license

This is a fork of [Cube 2: Sauerbraten](http://sauerbraten.org/) by Wouter "aardappel" van Oortmerssen,
Lee "eihrul" Salzman, and the Cube/Sauerbraten contributors. The original engine and game are released
under the **zlib license** (see `src/readme_source.txt` and the upstream distribution); this fork inherits
that license. All credit for the base engine, game, and the Nishita/Preetham `atmo` sky goes to the
original authors. The HDR/GI additions described below are the new work in this fork.

The **game media** (`packages/`, ~2 GB of maps and textures) is **not** included here — get it from the
upstream Sauerbraten distribution and drop it next to the binary.

## What's new

- **HDR fp16 render pipeline** — scene rendered to a floating-point buffer, then tonemapped (Reinhard /
  Hejl / ACES / a stock-matching *neutral* operator) with optional auto-exposure.
- **HDR + 3-basis RNM lightmaps** — lightmaps store unbounded radiance (RGBE → fp16); bumpmapped surfaces
  use Valve's 3-direction radiosity normal map basis.
- **Baked global illumination** — cosine-weighted, multi-bounce path-traced GI with colour bleed, sky
  image-based lighting, and emissive surfaces (textures *and* lava/water/glass materials) as area lights.
- **Area-light soft shadows** — penumbrae that widen with distance, from the sun and from point/spot lights.
- **Light-probe grid** — baked ambient cubes (irradiance volume) that light dynamic mapmodels, à la Source.
- **HDR textures & skyboxes** — Radiance `.hdr` images load as fp16 (textures, glow maps, cube faces).
- **HDR-aware Nishita sky** — the existing `atmo` sky outputs true HDR radiance and can drive the bake.

Everything defaults **off** (or is selected by file format), so a stock map renders byte-for-byte as before.

## Quick start

```
hdr 1                 // enable the HDR pipeline (neutral tonemap ≈ stock look, but HDR-capable)
hdrlightmaps 1        // bake HDR lightmaps...
gi 1                  // ...with global illumination
calclight             // (re)bake
```

A worked Cornell-box GI example (colour bleed):

```
ambient 0; skylight 0; hdrlightmaps 1; gi 1; giscale 3; gibounces 2; girays 96; calclight; hdr 1
```

## New concepts

### HDR pipeline & tonemapping
The whole scene renders into an fp16 buffer and a final pass maps it to the 8-bit display. `hdrtonemap`
selects the operator; the default **3 = neutral** is an identity curve below ~0.8 with a soft rolloff above,
so it matches stock Sauerbraten (which is already display-referred) while letting true HDR highlights bloom
instead of clipping. Bloom comes from the existing `glare` system, fed the HDR buffer.

### Auto-exposure (eye adaptation)
`hdrautoexposure 1` enables Valve-style adaptation: each frame the scene luminance is binned into a
**histogram** and the exposure is eased toward the value that puts a target **percentile of the *lit*
pixels** at the middle-grey key. Void/near-black pixels are excluded, so a dark area or a few small bright
lights don't drag the exposure around. Off by default; use fixed `hdrexposure` for stable lighting work.

### HDR & 3-basis RNM lightmaps
With `hdrlightmaps 1`, lightmaps store unbounded HDR radiance (RGBE, decoded to fp16 at upload so bilinear
filtering is correct). With `rnm 1`, bumpmapped surfaces bake **three** lightmaps along the HL2 basis
directions, so normal-mapped detail responds to light direction. *GI only runs on HDR lumels* — flat
surfaces get HDR automatically; bumpmapped surfaces need `rnm 1`.

### Global illumination
`gi 1` adds, per lumel, a cosine-weighted hemisphere gather: rays that escape to the sky pick up sky/atmo
radiance (image-based lighting), rays that hit geometry pick up that surface's direct light × its albedo
(**colour bleed**), recursed up to `gibounces` times. `giscale` scales the indirect contribution; `girays`
trades quality for bake time. The bounce reflectance is the texture's average colour × its `vcolor`, so a
red wall bleeds red.

By default emissive surfaces are sampled as **explicit area lights** (`giemitdirect 1`) — each emissive
surface is collected and integrated with a form factor + visibility ray, exactly like Source's VRAD
"texlight" radiosity. This gives smooth, noise-free lighting with correct grazing falloff. `giemitdirect 0`
falls back to capturing emission via random hemisphere rays (noisier).

### Emissive surfaces as light sources
- **Textures**: any glow-mapped texture emits into the GI bake (`giemissive`). A coloured glow map emits its
  own colour; a grayscale glow *mask* emits the texture's own colour (so an orange panel casts orange).
  `giemissivetexel 1` samples the glow per-texel at the hit UV (so `vscale`/offset/rotation matter) instead
  of a per-slot average.
- **Materials**: lava, water and glass volumes emit their material colour into GI, set per variant with
  `gilava`/`gilava2`/`gilava3`/`gilava4` (and `giwater…`, `giglass…`).

### Light-probe grid
`lightprobes 1` bakes a grid of **ambient cubes** (6 directional radiance samples) through the level
(`lightprobegrid` spacing) and lights dynamic mapmodels by trilinearly interpolating the surrounding probes
— Valve's irradiance-volume approach — instead of per-frame ray casts.

### Sky as a light source
- `atmo 1` makes the procedural Nishita atmosphere drive the GI bake (blue sky fill + warm sun).
- `giskybox 1` instead samples a loaded skybox image (LDR or `.hdr`) as the GI light, and drives the direct
  sun from its brightest texel. `atmo` takes precedence when both are on (it overrides the visible sky).

## Variable reference

All new vars default to off/stock. "per-map" vars are bake-time and saved in the map; re-run `calclight`.

| Var | Default | Meaning |
|-----|---------|---------|
| `hdr` | 0 | master HDR fp16 pipeline + tonemap (`hdr 1` ≈ stock look, HDR-capable) |
| `hdrtonemap` | 3 | 0=Reinhard 1=Hejl 2=ACES **3=neutral** (default, stock-matching) |
| `hdrexposure` | 1.0 | manual exposure (used when auto-exposure is off) |
| `hdrautoexposure` | 0 | eye-adaptation auto-exposure (histogram/percentile of lit pixels) |
| `hdrkey` | 0.10 | auto-exposure target middle-grey |
| `hdrexposurepct` | 0.5 | auto-exposure percentile (0.5 = median of lit pixels; higher = expose for highlights) |
| `hdrvoidcutoff` | 0.001 | luminance below which pixels are treated as void and ignored by auto-exposure |
| `hdrminexposure` / `hdrmaxexposure` | 0.25 / 8 | clamp on adapted exposure |
| `hdradaptrate` | 2.5 | eye-adaptation speed (per second) |
| `hdrgamma` | 100 | extra output gamma ×100 (100 = none) |
| `hdrcontrast` | 1.0 | post-tonemap contrast (>1 darkens shadows/mids) |
| `hdremissive` / `emissivescale` | 1 / 2.0 | HDR emissive (glow + lava) render boost |
| `hdrlightmaps` | 0 | bake HDR lightmaps |
| `rnm` | 0 | bake 3-basis radiosity normal mapping (bumped surfaces) |
| `softshadows` / `lightsize` / `sunsoftness` | 0 / 8 / 0.5 | area-light soft shadows (samples / point radius / sun angular deg) |
| `gi` / `giscale` / `gialbedo` | 0 / 1 / 0.7 | bake sky IBL + multi-bounce GI (needs `hdrlightmaps`) |
| `gibounces` / `girays` | 2 / 64 | GI bounce count / samples per lumel |
| `giemissive` | 1 | emissive glow-map textures as GI area lights (scale; 0 = off) |
| `giemissivetexel` | 0 | 0 = per-slot average emission; 1 = per-texel glow sampling |
| `gilava`..`gilava4` / `giwater`..`giwater4` / `giglass`..`giglass4` | 0 | per-variant emission scale for lava/water/glass materials |
| `giskybox` | 0 | sample the loaded skybox as the GI light + drive the sun from it |
| `lightprobes` / `lightprobegrid` | 0 / 128 | bake + use the ambient-cube model-lighting grid / its spacing |

The HDR-aware `atmo` sky reuses the stock atmo vars; `atmobright` and `atmosundiskbright` are the main knobs
for sky/sun brightness (the sun disk is unclamped under `hdr`, so push these higher than in LDR).

## New commands

| Command | Purpose |
|---------|---------|
| `calclight [quality]` | (existing) bake lightmaps — honours all the bake-time vars above |
| `hdrinfo` | print HDR pipeline state (tonemap, exposure, loaded HDR/RNM lightmaps, emissive) |
| `getskyboxsun` | set `sunlightyaw`/`sunlightpitch`/`sunlight` from the skybox's brightest point |
| `dumplms` | dump the CPU lightmap atlases to PNG (HDR/RNM decoded + tonemapped) |
| `dumplmtexs` | dump the **GPU** lightmap textures to PNG and report their dimensions/type (debug) |
| `lmnearest 0/1` | switch lightmap filtering to nearest-neighbour to inspect the raw lumel grid (debug) |
| `lightprobeinfo` | report the baked light-probe grid (debug) |
| `debuglm` | fill lightmaps with solid debug colours and re-upload (debug) |

## Building

**Linux** (SDL2, SDL2_image, SDL2_mixer, zlib, GL):

```
make -C src -j client
```

**Windows** — open `src/vcpp/sauerbraten.sln` in Visual Studio (retarget to your toolset if it offers), or
let CI build it: the [Windows workflow](.github/workflows/windows.yml) builds Release x64 with MSBuild and
uploads a runnable zip (exe + matching SDL runtime DLLs + `data/`) as a build artifact. Add a `packages/`
folder for maps.

## Backward compatibility

- Every new feature is opt-in via a var (or selected by file format); with them off the engine renders
  existing maps identically to stock Sauerbraten.
- Map version was bumped (34); the new engine loads older maps unchanged, and HDR/RNM data rides new
  lightmap type bits plus an optional light-probe section that old maps simply don't have.
- See `HDR_TESTING.md` for a hands-on testing guide with example recipes for each feature.

## References

- Jason Mitchell, Gary McTaggart, Chris Green — *"Shading in Valve's Source Engine"*, SIGGRAPH 2006
  Advanced Real-Time Rendering course:
  https://advances.realtimerendering.com/s2006/Mitchell-ShadingInValvesSourceEngine.pdf
- Cube 2: Sauerbraten — http://sauerbraten.org/
