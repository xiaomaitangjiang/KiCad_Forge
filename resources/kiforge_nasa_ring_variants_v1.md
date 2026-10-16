# KIForge 圆环字标：版本与生成提示词

日期：2026-10-16

生成方式：内置 ImageGen；未使用 CLI/API 回退。

设计方向：NASA Worm 式圆角管线字形，结合用户提供的 STARFIELD 参考图中的开口圆环构图。保留字标穿过圆环、环线与字母分离的关系。

所有交付文件均按生成原尺寸保存为 1254 × 1254 像素 PNG，带背景；这些文件是位图设计稿。配色提示词中的十六进制色值是生成目标，不代表逐像素精确用色。

## 文件

| 版本 | 文件 |
| --- | --- |
| 混合大小写：红字、白环、深色背景 | [kiforge_nasa_ring_v1.png](D:/VScode/KiCad_Forge/resources/kiforge_nasa_ring_v1.png) |
| 全大写：红字、白环、深色背景 | [kiforge_nasa_ring_uppercase_v1.png](D:/VScode/KiCad_Forge/resources/kiforge_nasa_ring_uppercase_v1.png) |
| 全大写：红字、红环、白底 | [kiforge_nasa_ring_uppercase_red_v1.png](D:/VScode/KiCad_Forge/resources/kiforge_nasa_ring_uppercase_red_v1.png) |
| 全大写：红字、黑环、白底 | [kiforge_nasa_ring_uppercase_black_v1.png](D:/VScode/KiCad_Forge/resources/kiforge_nasa_ring_uppercase_black_v1.png) |
| 全大写：黑字、红环、白底 | [kiforge_nasa_black_text_red_ring_v1.png](D:/VScode/KiCad_Forge/resources/kiforge_nasa_black_text_red_ring_v1.png) |

## 检查

已逐张查看字标拼写、大小写、圆环开口、文字与环线的分离关系，并读取五个交付 PNG 验证其尺寸和文件格式。

透明底导出尝试出现边缘及空白区杂点，未通过视觉检查，因此未作为项目交付件保存。上述交付文件均为带底 PNG。

## 最终采用的提示词

### 混合大小写：红字、白环、深色背景

对应文件：kiforge_nasa_ring_v1.png

```text
Use case: logo-brand.
Asset type: finished project logo for KIForge, a circuit design engineering project.
Primary request: create an original KIForge wordmark inspired by the retro-futurist NASA Worm identity, integrated with the clean open circular framing of the user's STARFIELD reference.
Scene/backdrop: perfectly flat very dark midnight navy, approximately #0B1220.
Subject and typography: the ONLY text is exactly "KIForge", spelled K, capital I, capital F, lowercase o, lowercase r, lowercase g, lowercase e. Preserve that exact spelling and casing. Custom red lettering, approximately #FC3D21, with continuous tubular strokes, rounded geometric corners, confident horizontal rhythm and the smooth engineered simplicity of the 1970s NASA Worm. Let lowercase forms remain readable. No off-the-shelf condensed Impact lettering. The I must read as an I, all seven letters must be distinct and legible.
Composition/framing: a single centered logo on a generous square canvas. One large, optically perfect thin off-white circle frames the wordmark. The wordmark runs horizontally across the middle of the circle and extends beyond it on both sides, like STARFIELD. Circle diameter about 60 percent of the canvas width, wordmark width about 81 percent. Upper and lower circular arcs read together as ONE circle; leave clean deliberate open gaps where the word crosses the circle on the left and right. Circle strokes must stop before letters and never touch, cut through or hide any letter. Circle stroke around one fifth of the lettering stroke, balanced clean negative space.
Style/medium: extremely crisp flat vector-style graphic, smooth paths, restrained aerospace engineering identity, polished visual balance, useful as a software brand.
Constraints: only the wordmark and one open circular outline, very clear KIForge spelling, strong silhouette. No NASA text, no STARFIELD text, no slogans, no extra labels, no watermark, no stars, no rockets, no arrows, no circuit-board ornament, no texture, no gradient, no glow, no shadow, no bevel, no mockup, no collage, no variations board. Deliver one finished logo.
```

### 全大写：红字、白环、深色背景

对应文件：kiforge_nasa_ring_uppercase_v1.png

```text
Use case: logo-brand.
Input image: the attached KIForge logo is the design reference and edit target.
Primary request: create the ALL-UPPERCASE companion logo. Replace the text with exactly "KIFORGE", seven CAPITAL letters K I F O R G E. Make O, R, G and E unmistakably UPPERCASE, at the same cap height and on the same baseline as K, I and F. There must be no lowercase forms and no descenders.
Preserve the overall design: red retro-futurist NASA-Worm-inspired custom tubular wordmark, confident rounded geometric monoline strokes, generous square canvas, dark midnight navy background, off-white thin circular framing behind the horizontal wordmark. Retain the smooth engineered look of the reference and the distinctive rounded K, I and F. Draw uppercase O as a clear closed rounded geometric loop, uppercase R with its upper bowl and diagonal leg, uppercase G as a distinct open rounded capital with an inward bar, and uppercase E with three clear horizontal arms. Keep all seven letters immediately readable, with even optical spacing. Preserve the color palette and centered composition. Adjust letter widths naturally to keep the entire word inside the existing lateral margins, without compressing it into a condensed font.
Circle: one large off-white perfect circular outline centered on the wordmark, upper and lower arcs forming ONE circle. Clean open gaps at the wordmark band on left and right; ample separation so no arc touches or crosses any letter. The text extends beyond the circle at both sides.
Finish: clean flat colors, perfectly smooth crisp edges and calm negative space. Exactly one finished logo, not a comparison sheet.
Constraints: text must be exactly "KIFORGE", uppercase only. No NASA text, no slogans, no extra text, no stars, no icons, no arrows, no shadows, no texture, no glow, no gradients, no bevels, no mockups.
```

### 全大写：红字、红环、白底

对应文件：kiforge_nasa_ring_uppercase_red_v1.png

```text
Use case: logo-brand.
Input image: the attached all-uppercase KIFORGE logo is the exact design reference and edit target.
Primary request: make the RED CIRCLE version of this logo. Change ONLY the circular outline from off-white to the same vivid warm red as the lettering, approximately #FC3D21, and change the dark background to a perfectly clean pure white background #FFFFFF so the red ring is easy to assess.
Preserve the exact uppercase text "KIFORGE", seven capitals K I F O R G E. Preserve the existing custom NASA-Worm-inspired rounded tubular letterforms, red lettering, stroke thickness, cap height, baseline, spacing, positions, scale, margins and the square canvas. Preserve the ring's perfect circular geometry, size, thin stroke, position and the TWO clean open gaps across the wordmark band. The ring must remain separate from the letters with clear space and never merge with, touch or cross them.
Design: one bold red wordmark horizontally crossing a thin red open circle, centered on a flat white field. Clean smooth flat graphic, like carefully drawn vector paths. This is a precise color variant of the existing logo, no redesign.
Constraints: exactly one finished logo, red text and red ring only on white background. No black elements, no off-white ring, no extra text, no arrows, no icons, no texture, no gradients, no shadows, no glow, no bevel, no mockup.
```

### 全大写：红字、黑环、白底

对应文件：kiforge_nasa_ring_uppercase_black_v1.png

```text
Use case: precise-object-edit.
Input image: the attached red-circle KIFORGE logo on a white background is the edit target.
Primary request: change ONLY the thin CIRCULAR OUTLINE from red to solid BLACK, approximately #111111. This includes both upper and lower arcs and their end caps. Keep the entire word "KIFORGE" bright red exactly as it is.
Invariants: preserve the exact seven uppercase letters K I F O R G E, the red wordmark color and its custom NASA-Worm-style rounded geometric shapes, size, stroke widths, positions, cap height, baseline and spacing. Preserve the white background, original square canvas and margins. Preserve the circle's thin stroke, circular geometry, size, position and two clean gaps at the wordmark. Keep all edges smooth and crisp, no contact between the ring and letters.
Final design: red uppercase KIFORGE wordmark crossing a thin BLACK open circular outline on a clean WHITE background.
Constraints: one logo, exact layout, only the ring changes color. No red remaining in the circular outline; no black in the wordmark. No added text, no gradients, no textures, no shadows, no extra elements, no mockups.
```

### 全大写：黑字、红环、白底

对应文件：kiforge_nasa_black_text_red_ring_v1.png

```text
Use case: precise-object-edit.
Input image: the attached uppercase KIFORGE logo with red letters and a red ring on a white background is the exact edit target.
Primary request: produce the BLACK LETTERING + RED RING version. Change ONLY the seven letters of "KIFORGE" from red to solid BLACK, approximately #111111. Keep both thin circular arcs vivid RED, approximately #FC3D21. Keep the background WHITE.
Invariants: preserve the exact uppercase word "KIFORGE", seven capitals K I F O R G E, the existing custom NASA-Worm-inspired rounded tubular letterforms, the K's geometry, every letter's size, cap height, baseline, stroke thickness, positions and optical spacing. Preserve the canvas, scale and margins. Preserve the ring's perfect circular geometry, thin stroke, position, size and clean open gaps on both sides of the wordmark band. The ring must remain red and the entire wordmark must be black. Circle and letters remain clearly separated.
Finish: clean, crisp flat 2D graphic with smooth vector-like edges and uniform solid fills, centered on a plain white square canvas.
Constraints: exactly one finished logo: BLACK "KIFORGE" text, RED open circular outline, WHITE background. No black circle, no red letters, no extra words, no labels, no arrows, no icons, no gradients, no texture, no shadows, no glow, no bevels, no mockup.
```

