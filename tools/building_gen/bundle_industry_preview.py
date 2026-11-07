"""Inline industry_models_data.js and the slab textures into one self-contained page (industry_models_preview.html).

    python tools/building_gen/bundle_industry_preview.py
"""
import base64, glob, json, os

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
html = open(os.path.join(ROOT, 'industry_models.html'), encoding='utf-8').read()
data = open(os.path.join(ROOT, 'industry_models_data.js'), encoding='utf-8').read()
tex = {}
import io
from PIL import Image
# browsers premultiply alpha when decoding, which ruins the colour of texels whose alpha (here: relief depth) is low: the
# preview reads colour from an opaque copy (<name>_c.png) and depth from the original
for p in glob.glob(os.path.join(ROOT, 'assets', 'industry', 'spom', '*_a.png')):
    b = io.BytesIO()
    Image.open(p).convert('RGB').save(b, 'JPEG', quality=88)
    tex['assets/industry/spom/' + os.path.basename(p)[:-6] + '_c.png'] = 'data:image/jpeg;base64,' + base64.b64encode(b.getvalue()).decode()
for sub in ('tex', 'spom'):
    for p in glob.glob(os.path.join(ROOT, 'assets', 'industry', sub, '*.png')):
        tex['assets/industry/%s/' % sub + os.path.basename(p)] = 'data:image/png;base64,' + base64.b64encode(open(p, 'rb').read()).decode()
glb = {}
for p in glob.glob(os.path.join(ROOT, 'assets', 'industry', 'cattle', 'glb', '*.glb')):
    glb[os.path.basename(p)[:-4]] = 'data:model/gltf-binary;base64,' + base64.b64encode(open(p, 'rb').read()).decode()
data += '\nwindow.IND_GLB=' + json.dumps(glb) + ';'
html = html.replace('<script src="industry_models_data.js"></script>',
                    '<script>' + data + '\nwindow.IND_TEX=' + json.dumps(tex) + ';</script>')
html = html.replace("loader.load('assets/industry/tex/' + k + '.png'", "loader.load(window.IND_TEX['assets/industry/tex/' + k + '.png']")
open(os.path.join(ROOT, 'industry_models_preview.html'), 'w', encoding='utf-8').write(html)
print('industry_models_preview.html', len(html) // 1024, 'KB')
