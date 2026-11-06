
## 9. Status (2026-11-05)

Built (phases A and B, plus parts of C/D): the PHOTO tab in the Character Creator (`src/character/photo_fit.h`, `creator.cpp`).
- 30 built-in pins (eye corners and lid centres, brows, nose, mouth, chin, jaw corners, face edge at the ear, nose bridge, hairline) plus your own custom pins: **+ Custom pin**, then click the head on the right in side-by-side view; mark it on photos like any other pin.
- Several photos per person, any subset of pins; camera solved per photo (outlier pins are down-weighted, bad pins show their pixel miss in the list).
- Overlay (wireframe on the photo) and Side by side (photo | head from that camera, with a head rotation slider); eyeballs and hair cards are hidden in this view.
- Hover a pin in the list: close-up of the head with a dot and an arrow on that spot.
- Session (photo paths, marks, custom pins) is saved to `%LOCALAPPDATA%\tbd_engine\photo_fit_session.json`; photos are never copied into the project.
Not built yet: shape solve (morph weights), auto-suggest from the tracker, skin colour from photos, "project photo" texture mode.

Shape solve (built 2026-11-05): "Fit face shape to the pins" in the PHOTO tab moves the face sliders (HEAD tab) so the head's pins land on the marks of every photo. Pin motion per slider is read once from the head's own deform; the solve is Gauss-Newton with Huber weights and a ridge toward the neutral face, cameras re-solved each round. "Back to the neutral face" resets the sliders; Undo restores a fit. Synthetic test: 28.2 px -> 0.0 px over two photos. Still open: auto-suggest pins, skin colour from photos, project-photo texture mode.

Skin colour from photos (built 2026-11-05, `src/character/photo_skin.h`): "Read colours" and "Project photo" in the PHOTO tab. Each photo with a solved camera is projected onto the head atlas (depth test, facing weight, skin-colour test), the light is fitted as 2nd-order spherical harmonics and divided out (highlights and shadows leave the fit), photos are blended, and the result becomes a GAIN over the head's own albedo (stored in SkinPaint, applied in linear light before the tint), so base pores stay. Read = wide blur (18 mm), Project = narrow blur (3 mm, keeps blotches). Strength, Warmth and Brightness sliders because one photo cannot reveal its own colour cast; five zone swatches (forehead, cheeks, nose, chin) and an uneven-light warning. Limits: chroma and brightness only (moles' darkness is not copied, the base albedo's luminance detail stays); the gain is not saved in character presets yet (it is rebuilt from the session); not yet checked on a real portrait with neutral light.
