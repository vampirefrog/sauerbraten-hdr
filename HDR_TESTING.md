# Testing the HL2/Source-style HDR lighting port

This build adds a Half-Life 2 / Source-engine style HDR lighting system to Cube 2: Sauerbraten,
following Mitchell's *"Shading in Valve's Source Engine"* (SIGGRAPH 2006). **Every feature is opt-in
via console variables** — a fresh install renders exactly like stock Sauerbraten with everything off.

## 1. Build

```sh
make -C src client -j$(nproc)        # produces src/sauer_client
```
Dependencies (already present here): SDL2, SDL2_image, SDL2_mixer, zlib, OpenGL. Needs a GL 3.x GPU.

## 2. Run

Run from the repo root so it finds `data/` and `packages/`:
```sh
./src/sauerbraten_unix                 # normal launcher (uses ~/.sauerbraten as home)
# or run the binary directly with an explicit home dir:
./src/sauer_client -q/path/to/homedir
```
In-game, open the console with `T` or `/` and type the commands below (no leading `/` needed in the
console input line shown here; in chat use `/cmd`).

> Tip: `hdrinfo` prints the current state of every HDR feature at any time.

---

## 3. Quick start — turn it all on

```
hdr 1            // master HDR pipeline (fp16 + tonemap + auto-exposure)
hdrtonemap 2     // 0=Reinhard, 1=Hejl (closest to stock look), 2=ACES (punchy)
hdrinfo          // confirm state
```
`hdr 1` alone makes the existing scene render through the HDR pipeline. The bake-based features
(HDR lightmaps, RNM, light probes) additionally require a `calclight` with the right var set — see below.

---

## 4. Per-feature tests

### Phase 1 — HDR pipeline (tonemapping + eye adaptation)
```
map aard3c
hdr 1
hdrtonemap 1              // try 0 / 1 / 2 to compare operators
hdrexposure 1.0          // manual exposure (when autoexposure off)
hdrautoexposure 1        // eye adaptation: look at bright sky vs dark corners, watch it adapt
```
Expect: with `hdr 0` the image is identical to stock; with `hdr 1` bright areas no longer hard-clip and
auto-exposure brightens/darkens as you look around. `hdrgamma` (default 220) tunes output gamma.

### Phase 4 — HDR Nishita sky (procedural atmosphere)
```
map aard3c
atmo 1; atmobright 2; sunlight 0xffffff; sunlightscale 1
glare 1
hdr 1
```
Expect: the procedural sky renders through HDR; with `hdr 1` the **sun disk is no longer clamped** and
blooms as a real bright source (turn the view toward the sun). `hdr 0` = the old clamped sky.

### Phase 3/4 — HDR `.hdr` textures and skyboxes
A test HDR skybox is included at `packages/skies/hdrtest/` (a blue gradient with a very bright sun).
```
map aard3c
atmo 0
skybox "skies/hdrtest/sky*.hdr"
hdr 1; glare 1
```
Expect: with `hdr 1` the sky shows a correct blue gradient and a blooming sun; lower `hdrexposure 0.2`
and the dim sky darkens while the bright sun persists (true HDR range). With `hdr 0` the same skybox
looks wrong/pinkish (undecoded RGBE) — that contrast confirms the HDR decode path.
(`.hdr` textures also load for normal surfaces via the same loader.)

### Phase 5 — HDR emissive textures & materials
`fire_keep` has a lava floor and wall torches.
```
map fire_keep
hdr 1; hdrtonemap 1; glare 1
emissivescale 6          // global emissive intensity (HDR); hdremissive 1 must be on (default)
```
Expect: the lava and glow/torch surfaces glow brightly and **bloom**, versus flat orange with `hdr 0`.
`emissivescale 1` ≈ stock brightness; higher = more HDR glow. `hdremissive 0` disables the boost.

### Phase 2 — HDR + 3-basis radiosity-normal-mapped lightmaps (needs a bake)
`aard3c` uses normal-mapped (bump) textures. These vars are **bake-time** — set them, then `calclight`:
```
map aard3c
hdrlightmaps 1           // HDR (no 0-255 clamp) lightmaps on flat surfaces
rnm 1                    // 3-basis radiosity normal mapping on bumped surfaces
calclight                // bake (may take a few seconds)
hdr 1; hdrtonemap 1
```
Expect: surfaces are lit smoothly with no clipping/banding; normal-mapped walls show directional bounce
lighting that varies with the surface detail. Save and reload to confirm it round-trips:
```
savemap mytest_rnm
map mytest_rnm
hdrinfo                  // should report  lightmaps loaded: hdr=1 rnm=1
```
With `hdrlightmaps 0; rnm 0; calclight` you get the stock 8-bit lightmaps back.
(Bake quality: `lmaa 0; lightprecision 64` = fast/coarse for testing; defaults = higher quality/slower.)

### Phase 6 — Ambient-cube light-probe grid for models (needs a bake)
Lights dynamic models (mapmodels, items, players) from a baked grid of ambient cubes, Valve-style.
```
map aard3c
lightprobes 1            // bake (at calclight) AND use the probe grid
lightprobegrid 128       // probe spacing in world units (smaller = denser/larger map file)
calclight                // bakes the grid (also saved into the .ogz)
lightprobeinfo           // prints grid dims + the sampled ambient cube at your position
```
Expect: `lightprobeinfo` shows a 6-face cube whose **+Z (up) face is brightest** and **-Z (down) dimmest**,
tinted by the local lighting. Model/item lighting now comes from the grid (trilinearly interpolated),
varying smoothly as you move through the level. `lightprobes 0` reverts to the stock per-frame light trace.
After reload, set `lightprobes 1` (or put it in the map's `.cfg`) to use the baked grid.

---

### Soft (area-light) shadows — bake-time, works with or without HDR/GI
By default each light casts a single hard shadow ray. Set `softshadows` > 1 to treat lights as areas:
```
map aard3c
sunlight 0xffffff; sunlightscale 2; sunlightyaw 70; sunlightpitch 45
softshadows 24       // shadow-ray samples per light (more = smoother penumbra, slower)
lightsize 12         // point/spot light area radius (world units)
sunsoftness 2.5      // sun angular radius in degrees (IGNORED when atmo 1 — see below)
calclight
```
With `atmo 1`, the sun's soft-shadow size is taken from the visible Nishita sun (`atmosundisksize`, a
diameter) so the penumbra matches the sky's sun automatically; `sunsoftness` only applies when `atmo 0`.
Expect: shadow edges become soft and the penumbra **widens with distance from the caster** (area-light
behaviour), for the sun and every point/spot light. More samples + a little `blurlms 1` smooth the
Monte-Carlo noise. `softshadows 0` = stock hard shadows.

### Global illumination (sky IBL + indirect bounce) — bake-time, requires `hdrlightmaps`
```
map aard3c
skylight 0x6488B0                 // a sky colour (the sky must have a colour to light the scene)
sunlight 0xFFE8C0; sunlightscale 2; sunlightyaw 40; sunlightpitch 45
hdrlightmaps 1; gi 1; giscale 1   // gi adds: HDR sky image-based lighting + one indirect bounce
calclight
hdr 1; hdrtonemap 1
```
```
gibounces 2      // number of indirect light bounces (1..4); higher = richer + much slower bake
girays 64        // cosine-weighted hemisphere samples per lumel (8..1024); higher = smoother, slower
```
Expect: surfaces facing open sky pick up the (HDR) sky/sun colour with soft occlusion; shadowed areas
get warm indirect fill that picks up the colour of nearby surfaces (colour bleed). `giscale` scales the
effect (try 2–4 to see it clearly), `gialbedo` is the fallback reflectance for untextured hits.
**Nishita sky as the light source:** if `atmo 1` is set, the GI bake samples the actual procedural
atmosphere (the full Nishita/Preetham scattering model, evaluated on the CPU) for sky lighting — so
surfaces pick up the real directional sky colour (blue zenith, warm horizon, bright sun) instead of a
flat `skylight` colour. Just enable both:
```
atmo 1; atmobright 3; sunlight 0xffffff; sunlightscale 2; sunlightyaw 45; sunlightpitch 35
hdrlightmaps 1; rnm 1; gi 1; giscale 2; calclight; hdr 1
```
Without `atmo`, sky lighting falls back to the `skylight`/`sunlight` colours.

**Skybox as the light source (`giskybox`):** to light from a loaded skybox image (LDR or `.hdr`) instead
of the procedural atmosphere, set `giskybox 1` with a `skybox` loaded. The GI gather samples the skybox
faces, so a bright sun *painted into the skybox* lights the scene and casts soft shadows sized by how big
that sun is in the image. At bake, the **direct sun is also driven from the skybox** (its direction and
colour come from the brightest texel). You can also do that explicitly any time with the command
**`getskyboxsun`** (sets `sunlightyaw`/`sunlightpitch`/`sunlight` from the skybox's brightest point).
```
skybox "skies/hdrtest/sky*.hdr"
giskybox 1; gi 1; girays 96; softshadows 32
hdrlightmaps 1; calclight; hdr 1
```
Precedence for sky lighting: `giskybox` (if a skybox is loaded) > `atmo` > flat `skylight`.

**Emissive textures as area light sources (`giemissive`):** any texture with a glow map (`TEX_GLOW`,
including HDR `.hdr` glow maps and `glowcolor` >1) emits real radiance into the GI bake — so a glowing
panel, neon strip or lava-coloured wall *lights the surfaces around it* (and bleeds its colour onto them),
exactly like Valve's emissive surfaces. The bounce gather adds `emission × giemissive` whenever a sample
ray hits an emissive surface, so it scales with the glow's brightness and the surface's solid angle.
`giemissive 0` disables it; bump it up to make dim glows act as proper lights.
```
hdrlightmaps 1; gi 1; girays 64; giemissive 8
calclight; hdr 1
```
Expect: a glowing surface casts coloured fill onto adjacent non-glowing surfaces (verified on `dopamine` —
green panels tint the nearby crates/floor green, orange panels warm their walls). Needs `gi` + a glow-*map*
texture; map *light entities* and bare emissive **materials** without a glow texture do not emit through GI.

The emitted colour conforms to the texture: a *coloured* glow map emits its own colour, while a *grayscale*
glow map (just an intensity mask) emits the texture's own diffuse colour, so an orange panel with a white
glow mask casts orange light (not white). Tint further with the `glowcolor` shader param if needed.

`giemissivetexel` chooses how the glow is sampled at a bounce hit:
- `0` (default) — **per-slot average**: the surface is treated as a uniform area emitter with the mean
  emitted radiance. Cheap, low-noise, correct for far-field fill; `vscale`/offset/rotation don't affect it.
- `1` — **per-texel**: samples the actual glow texture at the hit point's UV, so bright strips/patterns
  emit locally and `vscale`, texture offset and rotation all matter. More faithful up close, but noisier
  per ray — raise `girays` (96–256) to keep it clean.
```
gi 1; giemissive 8; giemissivetexel 1; girays 128; hdrlightmaps 1; calclight; hdr 1
```

**Emissive materials (lava / water / glass) as GI area lights:** the three volume materials can each emit
their material colour into the GI bake. There are 4 variants of each, set individually with
`gilava`/`gilava2`/`gilava3`/`gilava4`, `giwater`…`giwater4`, `giglass`…`giglass4` (the first has no `1`).
Each is an emission *scale* (0 = off, the default); emitted radiance = the material's colour
(`lavacolour`/`watercolour`/`glasscolour` of that variant) × the scale. At bake the material's boundary
surfaces are collected as area emitters, and a bounce ray that crosses one before hitting solid geometry
picks up its light (so a lava pool warms the walls/ceiling above it, water casts its blue, etc.).
```
gi 1; gilava 3; hdrlightmaps 1; calclight; hdr 1   // lava pool lights its surroundings orange
```
`calclight` prints `GI: N emissive material surface(s)` when any are active. Needs `gi`; cost scales with
the number of emissive surfaces × `girays` (cheap unless a map has huge liquid areas).

**Notes:** GI is *additive*, so it adds fill light rather than darkening an already-bright map — it reads
best on scenes with strong directional light + shadow or a dark room with one light. `gibounces` is real
recursive radiosity, and `girays` sets samples per lumel, so bake time scales with both; keep
`lightprecision` coarse (64–128) and `girays` modest (32–64) while iterating. Requires `hdrlightmaps`/`rnm`.

**GI only runs on HDR lumels — important gotcha:** indirect light is computed only for surfaces baked as
`LM_HDR`. Flat (non-bumpmapped) textures get that automatically with `hdrlightmaps 1`. **Bumpmapped**
surfaces only get it with `rnm 1` — otherwise they bake as legacy LDR bump lightmaps and GI is silently
skipped on them. So on a map full of bumpmapped textures (most stock maps), enable `rnm 1` or the bounce
won't appear. For sky-lit indirect (light entering through the sky), use `gibounces 2`+ so the sky reaches
surfaces via the second bounce. A verified Cornell-box recipe (red wall left, green right, sky-faced top):
```
ambient 0; skylight 0; hdrlightmaps 1; gi 1; giscale 3; gibounces 2; girays 96; calclight; hdr 1
```
→ floor/ceiling fill with bounced light and the side walls bleed red/green onto them.

Colour bleed picks up both the texture's own colour **and** its `vcolor` tint: the bounce reflectance is
the base texture's average × that surface's `vcolor` (colorscale), so a wall coloured purely with
`vcolor 1 0 0` correctly bleeds red even if the underlying texture is grey.

## 5. Variable reference

| Var | Default | Persist | Meaning |
|-----|---------|---------|---------|
| `hdr` | 0 | yes | master HDR fp16 pipeline + tonemap (with neutral tonemap, `hdr 1` ≈ stock but HDR-capable) |
| `hdrtonemap` | 3 | yes | 0=Reinhard 1=Hejl 2=ACES **3=neutral (default, stock-matching)** |
| `hdrexposure` | 1.0 | yes | manual exposure (auto-exposure off) |
| `hdrautoexposure` | 0 | yes | eye-adaptation auto-exposure (off by default; on = cinematic adaptation). Valve-style luminance **histogram**: exposes toward a target percentile of the *lit* pixels (void excluded), robust to dark areas / small bright lights |
| `hdrkey` / `hdrminexposure` / `hdrmaxexposure` / `hdradaptrate` | 0.10 / … | yes | auto-exposure tuning (key = target grey; min/max clamp; adapt rate/sec) |
| `hdrexposurepct` / `hdrvoidcutoff` | 0.5 / 0.001 | yes | auto-exposure percentile (0.5 = median of lit pixels; higher = expose for highlights) / luminance below which pixels count as void and are ignored |
| `hdrgamma` | 100 | yes | extra output gamma ×100 (100 = none; Sauer is already display-referred) |
| `hdrcontrast` | 1.0 | yes | post-tonemap contrast (1.0 = none; >1 darkens shadows/mids) |
| `hdremissive` / `emissivescale` | 1 / 2.0 | yes | HDR emissive (glow + lava) boost |
| `hdrlightmaps` | 0 | per-map | **bake** HDR lightmaps (run `calclight`) |
| `rnm` | 0 | per-map | **bake** 3-basis radiosity normal mapping |
| `softshadows` / `lightsize` / `sunsoftness` | 0 / 8 / 0.5 | per-map | **bake** area-light soft shadows (samples / point-light radius / sun angular deg); 0 = hard |
| `gi` / `giscale` / `gialbedo` | 0 / 1 / 0.7 | per-map | **bake** HDR sky IBL + multi-bounce GI (needs `hdrlightmaps`) |
| `gibounces` / `girays` | 2 / 64 | per-map | GI bounce count / cosine-weighted samples per lumel (quality vs bake time) |
| `giskybox` | 0 | per-map | sample the loaded skybox image as the GI light source + drive the direct sun from it |
| `giemissive` | 1 | per-map | emissive (glow-map) textures act as GI area light sources; scales their emitted radiance (0 = off) |
| `giemissivetexel` | 0 | per-map | 0 = per-slot average emission (fast); 1 = per-texel glow sampling at hit UV (vscale/offset/rotation matter, wants higher girays) |
| `giemitdirect` / `giemitcell` | 1 / 16 | per-map | 1 = explicit area-light sampling of emissive surfaces (Source/VRAD texlight style — smooth, no noise/grazing-cutoff); 0 = old hemisphere-gather capture. `giemitcell` = emitter subdivision cell size (smaller = more accurate near field, slower) |
| `gilava`..`gilava4` / `giwater`..`giwater4` / `giglass`..`giglass4` | 0 | per-map | per-variant emission scale for lava/water/glass materials as GI area lights (0 = off); radiance = material colour × scale |
| `lightprobes` | 0 | per-map | **bake**+use ambient-cube model-lighting grid |
| `lightprobegrid` | 128 | per-map | probe spacing (world units) |
| `debughdr` | 0 | no | (debug) |

Console helpers: `hdrinfo` (feature state), `lightprobeinfo` (probe grid + sample), `genlightprobes`
(rebake the probe grid without a full `calclight`).

Map-format note: maps saved by this build use **MAPVERSION 34**. It loads all older maps unchanged;
older Sauerbraten builds cannot load v34 maps (expected).

---

## 6. Backward-compatibility check

With a **fresh home dir** (no saved config), everything defaults off and the engine renders identically
to stock — confirm with:
```
map aard3c
hdrinfo        // HDR pipeline: off | lightmaps hdr=0 rnm=0 | emissive off | light probes off
```
Existing maps and media load and render exactly as before until you opt in.

## 7. Known limitations
- The editor's **live** lightmap-update path isn't HDR-aware, so interactively editing geometry on an
  HDR/RNM-baked map can look wrong until you re-run `calclight`. The normal bake/load/play flow is correct.
- RNM uses one combined shader (diffuse + normal + 3 basis lightmaps); env/spec/parallax effects are not
  layered on top of RNM surfaces in this version.
