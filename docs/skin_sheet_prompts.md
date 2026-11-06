# ChatGPT skin textures: four tiles made one after another, joined into one big seamless texture

ChatGPT's images are small (about 1024 px square), so one tile stretched to a 2048 or 4096 map goes soft and repeats. Instead, make four
tiles IN ONE CHAT, each continuing the last, so together they are one continuous image of twice the width and height:

```
  [ 1 ][ 2 ]      1: the first tile
  [ 3 ][ 4 ]      2: continues to the RIGHT of 1     3: continues BELOW 1     4: continues to the right of 3 AND below 2
```

`gameenginetools\skin_tex\tile_mosaic.py` joins them. Inner seams are healed (the colour step shared out, the detail band rebuilt and
joined along a minimum-error path); the four outer edges are healed the same way as if they met on the wrap, so the result tiles in both
directions. Pores stay sharp: nothing is cross-faded over a wide strip.

Base colour (chosen, see `character_texture_prompts.md`): sRGB 207, 150, 120 (#CF9678).
Nothing here is a normal map on purpose: ChatGPT's normals are unreliable. Ask for a height or colour tile and let the tools derive normals
(`pore_tiles.py`, `skin_tile.py normal`).

## Rules for all four prompts

Square 1:1, highest resolution, flat even light, no shadows, no highlights, no gloss, no baked occlusion, no vignette, no gradient across the
tile, no hair, no text, no border, no watermark, no makeup. Fill the frame edge to edge. Keep the same scale, colour and contrast in every tile.

## Step by step (paste in order, in ONE chat, attaching the earlier images where it says)

### Tile 1  ->  `tiles\<name>_1.png`

> Generate an image: SUBJECT. Square 1:1, highest resolution you can output, flat even light, no shadows, no highlights, no gloss, no vignette,
> no gradient across the image, no focal point, detail running edge to edge. No hair, no text, no border, no watermark.

### Tile 2 (to the right)  ->  `tiles\<name>_2.png`

> Generate a new square 1:1 image of the SAME skin, same scale, same colour, same contrast and same style as the attached image. It is the tile
> directly to the RIGHT of the attached one, as if both were one wide photograph cut in two: its LEFT edge must continue the attached image's
> RIGHT edge with no visible seam (pores, lines and colour carry straight across, nothing cut off or repeated). The rest is new, different
> detail, not a copy or a mirror. Same rules: flat even light, no shadows, highlights, vignette, text, border or watermark.

### Tile 3 (below)  ->  `tiles\<name>_3.png`

> Same again: a new square 1:1 image of the SAME skin, same scale, colour, contrast and style as the FIRST image. It is the tile directly BELOW
> it: its TOP edge must continue the first image's BOTTOM edge with no visible seam. The rest is new detail, not a copy or a mirror. Same rules.

### Tile 4 (the corner, borders two)  ->  `tiles\<name>_4.png`

> A new square 1:1 image of the SAME skin, same scale, colour, contrast and style. Attached are tile 2 (above it) and tile 3 (to its left).
> It is the fourth tile of a 2x2 block: its TOP edge must continue tile 2's BOTTOM edge, and its LEFT edge must continue tile 3's RIGHT edge,
> both with no visible seam. The rest is new detail, not a copy or a mirror. Same rules.

If a tile comes back with a border, a different scale or visible lighting, ask for it again; four good tiles beat a fixed bad one.

## The SUBJECT for each texture

Use these in the Tile 1 prompt (the other three just say "the SAME skin"). One four-tile set per texture, save as `<name>_1..4.png`.

- **`pores_face` (height)** : a macro, top-down, orthographic texture of real adult FACE skin (cheek and forehead) as a grayscale HEIGHT map:
  mid-grey surface, pores as small dark round pits about 2 mm apart, a faint network of shallow dark cell lines between them, even density,
  no large wrinkles. White is high, black is deep. Pure grayscale. Feeds `pore_B_face_src.png`.
- **`pores_open` (height)** : the same, but larger OPEN pores about 3 to 4 mm apart, round deeper pits with a soft slightly raised rim, a faint
  orange-peel texture between them, as on a nose or inner cheek. Feeds `pore_A_open_src.png`.
- **`thin_skin` (height)** : very THIN skin as on an eyelid: very fine, shallow, tight crepe-like micro-wrinkles in short wavy lines, almost no
  visible pores, smooth soft look. Grayscale height map. Feeds `pore_E_thin_src.png`.
- **`skin_colour` (albedo)** : a macro, top-down texture of real adult FACE skin colour as a game albedo tile: soft, large, slow blotches of
  blood-colour variation (slightly redder and paler patches), a few very faint capillaries, a very light speckle of tiny darker and lighter dots,
  fine pores barely visible. Average colour sRGB 207, 150, 120 (#CF9678), local variation within about +-6%, never shifting the average. No
  freckles, moles, scars or wrinkles. Feeds the skin colour-variation tile (low contrast).

## Join them

```
python gameenginetools\skin_tex\tile_mosaic.py pores_face_1.png pores_face_2.png pores_face_3.png pores_face_4.png pores_face_big.png --mono --trim 6 --size 2048 --preview prev.png
```

Order is top-left, top-right, bottom-left, bottom-right. Use `--mono` for the grayscale height sets, leave it off for colour. `--trim 6` cuts
ChatGPT's soft edge off every tile first (raise it if a border shows). `--match` pulls the four to one colour if they drifted. `--band` and
`--fall` set the detail join (default 20 px) and how far the colour step is shared out (default 160 px). The script prints a seam score: about 1
means the seams are invisible. `prev.png` shows it tiled 2x2: look at the cross in the middle.

Then `pore_tiles.py` and `pore_atlas.py` as before. Tell me when the tiles are saved and I'll run all of it and check the base head in the engine.

## Fallback: one image with a 2x2 sheet

If ChatGPT will not continue an image cleanly, ask it for ONE image laid out as a 2x2 sheet of four variations of the same skin (same colour,
different random detail) and use `sheet_quilt.py <sheet.png> <out.png> --size 2048 --upscale 2`. It cuts four variations into one tile with
minimum-error seams. It reduces repetition but each seam is a real mismatch, so the sequential method above looks better.
