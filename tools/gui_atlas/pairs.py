# Side-by-side pairs: mockup (left) | engine (right), same device pixels: shots/gui_1to1/<dev>_<state>_pair.png
import glob, os
from PIL import Image, ImageDraw

D = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'shots', 'gui_1to1')
for m in sorted(glob.glob(os.path.join(D, '*_mockup.png'))):
    e = m.replace('_mockup.png', '_engine.png')
    if not os.path.exists(e):
        continue
    a = Image.open(m).convert('RGB')
    b = Image.open(e).convert('RGB').resize(a.size)
    out = Image.new('RGB', (a.width * 2 + 20, a.height + 60), (20, 21, 26))
    out.paste(a, (0, 60)), out.paste(b, (a.width + 20, 60))
    d = ImageDraw.Draw(out)
    d.text((10, 20), 'MOCKUP', fill=(230, 230, 230))
    d.text((a.width + 30, 20), 'ENGINE', fill=(230, 230, 230))
    out.thumbnail((2400, 2400))
    out.save(m.replace('_mockup.png', '_pair.png'))
    print(os.path.basename(m).replace('_mockup.png', ''))
