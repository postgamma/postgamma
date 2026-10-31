# Blog cover artwork

These covers were created with the built-in `imagegen` tool. Each article has
its own composition, medium, and palette. The final assets are 1200 by 675 pixel
WebP images, used in both website color themes.

They are editorial illustrations, not exact architecture diagrams or measured
results. In particular, the flame graph in the third cover is an illustration,
not an actual profiling artifact from the performance investigation.

## Assets and visual directions

- [introducing-postgamma.webp](../../docs/assets/images/blog/introducing-postgamma.webp): a hand-drawn print illustration with a cobalt elephant workshop, coral, turquoise, and golden yellow.
- [threading-postgresql-without-editing-upstream.webp](../../docs/assets/images/blog/threading-postgresql-without-editing-upstream.webp): an exploded engineering drawing with glacier blue, navy, cyan, and vermilion.
- [a-repeated-check-in-postgamma.webp](../../docs/assets/images/blog/a-repeated-check-in-postgamma.webp): a cinematic debugging scene with deep plum, ink blue, amber, coral, and cyan.

## Final generation prompts

### 1. introducing-postgamma

```text
Use case: illustration-story.
Asset type: a finished landscape 16:9 cover for "Introducing PostGamma", a project that runs PostgreSQL using threads and can embed the database in C or Python applications.
Create a rich, confident editorial illustration with hand-drawn ink contours, crisp flat screen-printed color, subtle paper grain, and expressive geometric shapes. It should look like a carefully commissioned technology magazine illustration, not a 3D render or a generic icon.
Composition: a large sculptural cobalt-blue elephant, seen from the side, occupies the center-left as a visual metaphor for PostgreSQL. Its body is a beautiful cutaway mechanical workshop containing neatly arranged data drawers, a small processor, gears and several working connections. Three colorful cables leave this shared engine and connect to separate application windows arranged at different scales on the right, one tall, one wide, one compact. Those windows show simple code-like strokes and tiny row/column patterns, never readable code. Warm coral panels, curved turquoise cables, golden-yellow connectors, cream pages, indigo outlines. Add a few well-placed geometric details and fine hatch texture so the picture rewards looking, but keep the elephant and fan-out to applications immediately clear at thumbnail size.
Background and palette: warm pale peach paper, cobalt blue main subject, vivid coral-orange, golden yellow, turquoise and deep ink accents. Harmonious complementary warm/cool palette with deliberate color hierarchy. Broad diagonal movement and asymmetry, full use of the wide frame, balanced negative space. No text, no letters, no logo, no watermark, no floating glass balls, no green stone cylinders, no beige studio still life. Professional and lively, not childish. Opaque image.
```

### 2. threading-postgresql-without-editing-upstream

```text
Use case: scientific-educational.
Asset type: a wide 16:9 conceptual cover illustration for "Threading PostgreSQL Without Editing Upstream".
Primary message: original upstream source remains completely intact, while a separate generated copy receives transformations and yields a threaded build. This is a conceptual editorial illustration, not an exact software architecture diagram.
Visual style: an elaborate exploded technical drawing in the tradition of carefully drafted engineering plates, with architectural ink linework, translucent tracing-paper overlays, fine cyan registration lines, small measured construction details, and selective bold flat color. A flat, near-orthographic overhead composition with layers lifted slightly apart, NOT photorealistic objects in a studio.
Composition: an intact navy source blueprint occupies the left third on an icy blue drafting surface. It contains an orderly tree of small blocks and code-like horizontal marks. Its border and contents are whole and undisturbed. A separate matching blueprint copy appears across the center, with two translucent tracing layers lifted above it, coral transformation marks and subtle curved connecting lines. At the right, the generated drawing opens into three precisely aligned but visually different turquoise, coral, and golden-orange execution tracks, each composed of fine tiny geometric nodes. Make the separation between untouched source and generated copy obvious through comfortable spacing and distinct paper edges. A single elegant left-to-right dashed connector may link the source to the copy, without implying the original is moved or altered. Add an orange corner tab and cyan alignment guides as meaningful details, not unrelated clutter.
Palette: pale glacier blue background, dark midnight navy linework and source plate, clean cyan and turquoise overlays, warm vermilion and small ochre highlights, white paper. Strong cool/warm contrast, delicate detail and rich layering. Clear large shapes readable at 360px wide. No text, no letters, no numbers, no labels, no cut or damage to the source plate, no magnifying glass, no elephant, no green ribbon sculpture, no beige studio background. Opaque finished artwork.
```

### 3. a-repeated-check-in-postgamma

```text
Use case: illustration-story.
Asset type: a finished wide 16:9 editorial illustration for a database developer's story about finding a redundant check in a frequently executed path.
Art direction: cinematic painted digital collage, a close-up of a developer's performance investigation desk at night. Tactile ink, subtle film grain and layered translucent panels, with painterly light and sophisticated contrast. This must be a rich, atmospheric scene, not a minimalist physical sculpture and not a generic product dashboard screenshot.
Composition: the main subject is a large angled profiler panel occupying the left and center, showing an ABSTRACT symbolic flame graph made of stacked rectangular blocks in amber, burnt orange, coral and soft gold against deep plum and charcoal blue. The repeated small coral segment appears at several points in the stacks. One segment is connected by a fine cyan line to a large offset circular zoom inset on the right, which expands a repeated conditional-branch shape into a clear little branching diagram. Around the main panel are a partially visible compact keyboard in the foreground, overlapping cream scratch notes with tiny code-like strokes, and a narrow teal waveform strip. Use strong foreground, middle-ground and background depth, asymmetric crop, thoughtful overlapping shapes and concentrated warm monitor light. The repeated segment and zoom inset are the visual story; support objects are quieter.
Palette: dark ink blue and rich plum background, amber/orange/coral graph blocks, mint-cyan highlights, pale cream notes. Harmonious varied colors with a warm luminous focal point and cool surrounding shadows. The cover must remain legible at 360px wide. No text, no readable code, no numbers, no axes, no benchmark values, no claims of measured improvement, no checkmarks or speedometers, no physical magnifying glass, no green ribbon paths, no beige studio background. This is an unmistakably illustrative scene, not an authentic profiling result. Opaque image.
```
