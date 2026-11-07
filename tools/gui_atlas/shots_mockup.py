# Screenshots of the Glass Dock+ mockup (gui_mockups, tab 8) at device pixels, for the 1:1 pairs in shots/gui_1to1/.
#   python -m http.server 8820 -d "<gui_mockups>"  then  python tools/gui_atlas/shots_mockup.py
import os, sys
from playwright.sync_api import sync_playwright

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'shots', 'gui_1to1')
URL = 'http://localhost:8820/#8'

# state: list of data-a clicks inside the device screen
STATES = {
    'phone': {
        'touch_closed': [], 'touch_open': ['toggle'], 'touch_roads': ['toggle', 'cat:roads'],
        'touch_place': ['toggle', 'cat:roads', 'item:Two-Lane Road|$40/m|road_s'],
        'pause': ['pause'], 'settings': ['pause', 'settings'],
        'pc_closed': ['pause', 'settings', 'setgui:pc', 'settingsback', 'resume'], 'pc_roads': ['pause', 'settings', 'setgui:pc', 'settingsback', 'resume', 'cat:roads'],
    },
    'pad': {
        'pc_closed': [], 'pc_roads': ['cat:roads'], 'pc_services': ['cat:serv'],
        'pc_place': ['cat:roads', 'item:Two-Lane Road|$40/m|road_s'],
        'pause': ['pause'], 'settings': ['pause', 'settings'],
        'touch_closed': ['pause', 'settings', 'setgui:touch', 'settingsback', 'resume'], 'touch_roads': ['pause', 'settings', 'setgui:touch', 'settingsback', 'resume', 'toggle', 'cat:roads'],
    },
}
os.makedirs(OUT, exist_ok=True)
with sync_playwright() as p:
    b = p.chromium.launch(channel='chrome')
    for dev, scale in (('phone', 3), ('pad', 2)):
        for name, clicks in STATES[dev].items():
            page = b.new_page(viewport={'width': 2200, 'height': 1400}, device_scale_factor=scale)
            page.goto(URL)
            page.wait_for_timeout(800)
            page.add_style_tag(content='.padscale{transform:none!important}.island,.homebar{display:none!important}*{animation:none!important;transition:none!important}')
            page.click('#sndbtn')  # sound off
            for a in clicks:
                page.locator(f'#{dev} [data-a="{a}"]').first.click()
                page.wait_for_timeout(350)
            page.wait_for_timeout(400)
            page.locator(f'#{dev}').screenshot(path=os.path.join(OUT, f'{dev}_{name}_mockup.png'))
            page.close()
            print(dev, name)
    b.close()
