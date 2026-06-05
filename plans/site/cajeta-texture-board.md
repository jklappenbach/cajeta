# Cajeta texture & material board — "the cajeta kitchen"

Sourcing for the texture/material system (plan §3.1.1). Everything here is **CC0**
or **free-for-commercial** so it ships safely. Two tiers:

- **Tier A — web-shipping:** subtle, tiny, fast (overlays, SVG noise, small tiles).
- **Tier B — 3D / hero photography:** rich PBR + photos for Blender renders and big
  section images (lazy-loaded WebP/AVIF).

## Licensing key
- ✅ **CC0 / public domain** — ship freely, no credit needed: **Poly Haven**,
  **ambientCG**, **TextureCan**, **3DTextures.me**, **Transparent Textures**.
- ✅ **Unsplash / Pexels** — free commercial, no attribution required (credit is kind).
- ⚠️ Avoid for shipping: textures.com (restrictive credit limits), iStock / Dreamstime /
  Shutterstock (paid) — fine only if separately licensed.

---

## Terracotta / clay — *the olla*
- **3D/PBR (CC0):** [TextureCan — Old Clay Pot](https://www.texturecan.com/details/582/)
  (ideal for the **Olla** render itself), [Poly Haven — Patio Tiles](https://polyhaven.com/a/patio_tiles),
  [Clay Roof Tiles 02](https://polyhaven.com/a/clay_roof_tiles_02),
  [3DTextures — Wall Brick Terracotta 003](https://3dtextures.me/2026/05/11/wall-brick-terracotta-003-free-seamless-pbr-texture/),
  [ambientCG search](https://ambientcg.com/list?q=terracotta).
- **Photos:** [Unsplash — clay texture](https://unsplash.com/s/photos/clay-texture)
  (also search "talavera", "mexican pottery", "olla de barro" — verify each photo's license badge).
- **Web use:** `terracotta-400` flat + a faint matte-ceramic grain overlay → the Olla
  registry section background and ceramic-look cards.

## Caramel — *glossy amber* (already built)
- Not a tiled texture — it's the **Blender material/render** (`plans/site/assets/cajeta-cube*.png`,
  `assets/cajeta-cube.blend`) + CSS "molten caramel" gradient for CTAs/highlights.

## Worn cast iron / kitchen — *comal, cookware*
- **3D/PBR (CC0):** [ambientCG — Metal](https://ambientcg.com/list?q=metal) (worn / cast
  iron / rust variants), [Poly Haven — Metal category](https://polyhaven.com/textures/metal).
- **Web use:** `comal-900` near-black + a subtle brushed-metal/noise overlay → dark
  sections, footer, **code blocks** (where code "cooks"), scrolled nav, dark mode.

## Wood — *the family table*
- **3D/PBR (CC0):** [Poly Haven — Worn Planks](https://polyhaven.com/a/worn_planks),
  [Poly Haven — Wood category](https://polyhaven.com/textures/wood),
  [ambientCG — Wood](https://ambientcg.com/list?q=wood).
- **Photos:** [Unsplash — rustic wood](https://unsplash.com/s/photos/rustic-wood),
  [old wood texture](https://unsplash.com/s/photos/old-wood-texture).
- **Web use:** low-opacity wood-grain tile on panels/dividers/frames, or a warm wood
  photo as a section band (the table the cube sits on).

## Kraft / paper — *the canvas + docs*
- **Web (free):** [Transparent Textures](https://transparenttextures.com/) ("natural
  paper", "cardboard", "paper"), [Subtle Patterns](https://www.toptal.com/designers/subtlepatterns/).
- **Web use:** flat cream + faint paper grain on the main canvas and docs pages
  (paper = legible).

## Grain / imperfection — *site-wide hand-made feel*
- **Best (zero asset weight):** generate SVG noise with
  [nnnoise (fffuel)](https://www.fffuel.co/nnnoise/) or inline CSS `feTurbulence` —
  see [ibelick — grainy backgrounds with CSS](https://ibelick.com/blog/create-grainy-backgrounds-with-css).
- **Web use:** 3–6% opacity overlay everywhere; slightly irregular edges; soft warm
  (`comal`-tinted) shadows.

---

## Web performance budget (so the kitchen stays fast)
- Tiles **256–512px, <30 KB**; JPEG q75–80 is often smaller than WebP for noise.
- Prefer **inline SVG noise** for grain (no request). Lazy-load hero photos as WebP/AVIF.
- **Texture is accent, not wallpaper** — body type always on clean cream/paper.

## Next cook — the Blender "kitchen" hero 🔥
Assemble a warm **kitchen scene** in `assets/cajeta-cube.blend`: the caramel **cube** + a
clay **olla** on a **wood table / comal**, lit by a warm indoor **HDRI**
([Poly Haven HDRIs](https://polyhaven.com/hdris) — search "kitchen"/"interior"), using
the CC0 PBR materials above. This becomes:
- the **landing hero** image, and
- the environment for the **rotating-cube-with-page-reflections** idea (plan §11).
Render in **EEVEE** (stable; Cycles-HIP crashes on this GPU).
