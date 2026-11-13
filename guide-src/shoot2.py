import urllib.request
from playwright.sync_api import sync_playwright
B="http://127.0.0.1:8099"
def mock(p): urllib.request.urlopen(B+p).read()
with sync_playwright() as p:
    br=p.chromium.launch(channel="chrome")
    mock("/mock/set?name=lost"); mock("/mock/adv?on=0"); mock("/mock/rec?mode=1")
    ctx=br.new_context(viewport={"width":400,"height":860},device_scale_factor=2,is_mobile=True,has_touch=True,locale="en-US")
    pg=ctx.new_page(); pg.goto(B+"/"); pg.wait_for_timeout(900); pg.click('nav#tabs button[data-t="osd"]'); pg.wait_for_timeout(700)
    pg.add_style_tag(content="nav#tabs{position:static!important} footer{position:static!important}")
    pg.locator("#osdscreen").first.screenshot(path="shots/osd_lost.png")
    pg.check("#osdsim"); pg.wait_for_timeout(300)
    pg.locator("#osdscreen").first.screenshot(path="shots/osd_sim.png")
    br.close()
