# Cajeta caramel-cube logo — render proposals

Three photoreal-ish glossy caramel cube proposals, modeled & rendered in Blender
5.1.2 to match your Vecteezy reference (`perfect-caramel-candy-cube-…webp`).

## Files
- `assets/cajeta-cube.blend` — one file, **three scenes** (switch via the Scene dropdown
  in Blender's top bar): `P1_Studio`, `P2_Softbox`, `P3_Dramatic`. Each is
  pre-configured to render straight to its PNG below.
- `assets/cajeta-cube-1.png` — **P1_Studio** (1024², RGBA, transparent bg)
- `assets/cajeta-cube-2.png` — **P2_Softbox**
- `assets/cajeta-cube-3.png` — **P3_Dramatic**

### Kitchen hero scenes (the "cajeta kitchen" — 1280×720, in-scene, procedural)
- `assets/cajeta-kitchen-1.png` — **K1_Table**: cube on a warm wood table, dim cozy kitchen.
- `assets/cajeta-kitchen-2.png` — **K2_Hearth**: deep-amber cube on a dark worn-iron *comal*,
  warm rim light + light pool — **dark-mode hero** (the standout).
- `assets/cajeta-kitchen-3.png` — **K3_Terracotta**: cube on the wood table against a warm
  **terracotta wall**, morning daylight — ties terracotta + wood + caramel together.

(All EEVEE, procedural wood/clay/iron — Poly Haven download is off, so no external
assets. Scenes `K1_Table` / `K2_Hearth` / `K3_Terracotta` live in `assets/cajeta-cube.blend`.)

## The three proposals
All share: a rounded-bevel cube, *very slightly* deformed/displaced for a
hand-poured feel, edge-forward 3/4 view, a glowing warm-amber caramel material
(Principled BSDF: low roughness + clearcoat for wet gloss, subsurface for the
internal glow), transparent background, AgX "Punchy" tone-mapping.

| Scene | Lighting | Tone | Feel |
|-------|----------|------|------|
| **P1_Studio** | soft 3-point (warm key + cool fill + warm rim), warm low world | rich amber, big creamy top highlight | warm, classic, appetizing |
| **P2_Softbox** | big soft softbox + wrap fill, brighter warm world (visible side sheen) | brighter golden candy | clean, fresh, glossiest sides |
| **P3_Dramatic** | strong back/rim glow + low key, near-black world | deep burnt-amber, stronger internal glow | moody jewel, high contrast |

## My take
- **P2_Softbox** is probably the best *logo*: brightest, most legible, glossiest
  sides, reads well small.
- **P1_Studio** is the warmest/most "edible."
- **P3_Dramatic** is the most striking but darkest (can lose detail at favicon size).
- P1's top highlight is a touch large/milky — easy to shrink (smaller/closer key).

## Why EEVEE (not Cycles)
The first Cycles render on the AMD **HIP GPU crashed Blender** (ROCm + Cycles on
Strix Halo is unstable), so iteration was done on EEVEE Next (fast, stable).
**CONFIRMED: Cycles on _CPU_ is stable AND fast** — P1_Studio rendered at 1200²,
160 samples + OpenImageDenoise in **11.3 s, no crash** (`assets/cajeta-cube-1-cycles.png`).
→ Rule of thumb on this machine: **`scene.cycles.device = 'CPU'`** for final renders;
never HIP GPU. EEVEE still fine for quick look-dev.

## Next steps (when you pick one)
1. Tell me the winner (or a hybrid — e.g. "P2 lighting, P1 warmth").
2. I'll refine it (highlight shape, exact gold), then **export favicon/OG sizes**
   (512², 180², 32², 1200×630) with transparency.
3. Wire it into the site as the logo/favicon and swap the SVG placeholder
   (`plans/site/assets/cajeta-cube.svg`) referenced in `cajeta-site-plan.md` §3.3 / §11.

## To re-render yourself
Open `assets/cajeta-cube.blend`, pick a scene from the Scene dropdown, F12 to render.
(The BlenderMCP socket on port 9876 only stays connected while Blender runs; if
Blender restarts, re-open the N-panel → BlenderMCP → Connect before asking me to
drive it again.)
