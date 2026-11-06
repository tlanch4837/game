# ChatGPT texture prompts for the character creator

Base skin colour (chosen): sRGB 207, 150, 120 (#CF9678), Wyatt's measured average skin. Use this same average colour for the face, back of the hands, eyelids and ears; local redness and veins must stay subtle and not shift the overall average. Palms and soles are slightly brighter, at sRGB 217, 164, 136 (#D9A488). It is the default tone of the tone ramp, so the skin tiles only need to carry structure and soft variation; the tool re-tints them. Keep them low contrast. Mouth textures and grayscale hair masks have their own colours.

Shared rules (already inside each prompt): square 1:1, highest resolution, seamless in both directions, flat even light, no shadows, no highlights or gloss, no baked occlusion, no text, no border, no watermark, no makeup, no hair unless the texture is hair.

How to use: paste the prompt, save the result as the file named under it in `testproject\WesternGame\Assets\characters\skin_tiles\` (strand tiles in `hair_tiles\`, mouth tiles in `mouth_tiles\`). Then run `python gameenginetools\skin_tex\skin_tile.py seamless <in> <out>` and `normal`.

## 1. Face skin  ->  skin_face_01.png
Generate an image: a seamless, tileable macro texture of real adult male FACE skin (cheek and forehead), flat even light, as a game albedo tile, square 1:1, highest resolution you can output. Fine small pores, slightly finer and denser than arm skin, very subtle oily sheen variation shown only as colour not highlights, soft natural blood-colour variation, a few very faint capillaries. Average colour about #CF9678 (warm light skin), low contrast, mid exposure. No hair, stubble, freckles, moles, scars, wrinkles, blemishes, makeup, shadows, highlights, vignette, text, border or watermark. Tiles seamlessly with no focal point. Fill the whole frame edge to edge.

## 2. Hands, knuckles and joints  ->  skin_hand_01.png
Generate an image: a seamless, tileable macro texture of real adult male skin over the BACK OF THE HAND and knuckles, flat even light, square 1:1, highest resolution. Coarser, more creased skin than face skin: fine crosshatch lines, small diamond-shaped skin ridges, tiny visible pores, slightly thin and translucent look, faint blue-green veins deep under the surface. Average colour about #CF9678, low contrast, mid exposure. No hair, freckles, scars, nails, makeup, shadows, highlights, text, border or watermark. Seamless in both directions, no focal point.

## 3. Palms and soles  ->  skin_palm_01.png
Generate an image: a seamless, tileable macro texture of real human PALM or SOLE skin, flat even light, square 1:1, highest resolution. Thick skin with parallel friction ridges (fingerprint-like ridge lines, wavy and branching, no whorl centre), tiny sweat-pore dots along the ridges, slightly paler and yellower than normal skin, a few very faint deeper crease lines. Average colour about #D9A488, low contrast. No hair, no pores of the ordinary kind, no scars, shadows, highlights, text, border or watermark. Seamless in both directions, no focal point.

## 4. Eyelids and ears (thin skin)  ->  skin_thin_01.png
Generate an image: a seamless, tileable macro texture of real thin human skin as on an EYELID or the rim of the ear, flat even light, square 1:1, highest resolution. Very fine, shallow, tight crepe-like micro-wrinkles, almost no visible pores, a translucent appearance with subtle local blood-colour variation and faint fine capillaries. Match the face and hand tiles' average colour: sRGB 207, 150, 120 (#CF9678); do not shift the overall colour redder. Low contrast. No hair, lashes, freckles, scars, makeup, shadows, highlights, text, border or watermark. Seamless in both directions, no focal point.

## 5. Teeth enamel  ->  mouth_tiles\teeth_enamel_01.png
Generate an image: a seamless, tileable macro texture of real human tooth enamel, flat even light, square 1:1, highest resolution. Slightly warm ivory white (about #E8E0CF), very soft vertical micro-ridges (perikymata), faint cloudy translucency, hair-thin craze lines, no cavities, no stains, no gum, no gap. Very low contrast. No shadows, no highlights, no gloss, no text, border or watermark. Seamless in both directions.

## 6. Gums  ->  mouth_tiles\gum_01.png
Generate an image: a seamless, tileable macro texture of healthy human gum tissue, flat even light, square 1:1, highest resolution. Salmon pink (about #D98B8F), fine orange-peel stippling, soft blood-colour variation, a few very faint capillaries, matte (no wet highlights). Low contrast. No teeth, no blood spots, no shadows, no highlights, text, border or watermark. Seamless in both directions.

## 7. Tongue  ->  mouth_tiles\tongue_01.png
Generate an image: a seamless, tileable macro texture of the upper surface of a healthy human tongue, flat even light, square 1:1, highest resolution. Pink-red (about #C46A6E) with a fine carpet of tiny rounded papillae, a faint paler coating, soft blood-colour variation. Matte, no wet highlights, no teeth, no shadows, text, border or watermark. Seamless in both directions.

## 8. Eyebrow hair strands  ->  hair_tiles\brow_strands_01.png
Generate an image: a seamless, tileable macro mask of very short, stiff eyebrow hairs seen from directly above, square 1:1, highest resolution. Hundreds of individual hairs, each a short, thick, nearly straight line with a blunt root and a pointed tip, all pointing the same direction (straight up in the image) with at most 12 degrees of random angle, about 8 to 20 pixels long and 2 to 3 pixels wide at 1024 px, varied brightness 60% to 100%. Pure white hairs on a pure black background, pure grayscale, no colour, no skin, no blur, no glow, no shadow. Hairs crossing an edge continue on the opposite edge so it tiles seamlessly. No text, border or watermark.

## 9. Stubble and buzz-cut strands  ->  hair_tiles\stubble_strands_01.png
Generate an image: a seamless, tileable macro mask of 1 to 2 days of beard stubble and buzz-cut hair seen from directly above, square 1:1, highest resolution. A dense field of tiny hairs, each a very short stiff dot or short dash (4 to 10 pixels long, 1 to 2 pixels wide at 1024 px), pointing roughly the same way (up in the image) with up to 25 degrees of random angle jitter, irregular spacing but even density, brightness 50% to 100%. Pure white on pure black, pure grayscale, no skin, no blur, no glow, no shadow. Tiles seamlessly in both directions. No text, border or watermark.

## 10. Scalp short hair (replace the first tile if it is too long and wavy)  ->  hair_tiles\short_strands_02.png
Generate an image: a seamless, tileable macro mask of very short scalp hair (a close crop, about 3 mm) seen from directly above, square 1:1, highest resolution. Hundreds of individual straight, stiff hairs, 10 to 30 pixels long and 1 to 2 pixels wide at 1024 px, all pointing straight up with at most 8 degrees of jitter, tapered tips, irregular spacing but even density, brightness 60% to 100%. Pure white on pure black, pure grayscale, no skin, no blur, no glow, no shadow, no curls. Seamless in both directions. No text, border or watermark.
