import urllib.request, json
from playwright.sync_api import sync_playwright
B="http://127.0.0.1:8099"
def mock(path): urllib.request.urlopen(B+path).read()
NOSTICKY="nav#tabs{position:static!important} footer{position:static!important} .modal{position:fixed}"
ANN={}
JS="""(a)=>{const r=(e)=>{const b=e.getBoundingClientRect();return [b.left+scrollX,b.top+scrollY,b.width,b.height]};
 return {c:a.c?r(document.querySelector(a.c)):[scrollX,scrollY,innerWidth,innerHeight],
         m:a.m.map(s=>{const e=document.querySelector(s);return e?r(e):null})}}"""
with sync_playwright() as p:
    br=p.chromium.launch(channel="chrome")
    def page(sc="ready", adv=False, tab=None, rec=1, nosticky=True, w=400, h=860, wait=900):
        mock(f"/mock/set?name={sc}"); mock(f"/mock/adv?on={1 if adv else 0}"); mock(f"/mock/rec?mode={rec}")
        ctx=br.new_context(viewport={"width":w,"height":h},device_scale_factor=2,is_mobile=True,has_touch=True,locale="en-US")
        pg=ctx.new_page(); pg.goto(B+"/"); pg.wait_for_timeout(wait)
        if tab: pg.click(f'nav#tabs button[data-t="{tab}"]'); pg.wait_for_timeout(700)
        if nosticky: pg.add_style_tag(content=NOSTICKY)
        return pg
    def cap(pg,name,container=None,marks=()):
        info=pg.evaluate(JS,{"c":container,"m":[m[1] for m in marks]})
        cx,cy,cw,ch=info["c"]
        ANN[name]={"w":cw,"h":ch,"marks":[{"n":m[0],"box":[b[0]-cx,b[1]-cy,b[2],b[3]]} for m,b in zip(marks,info["m"]) if b]}
        ANN[name]['marks']=[m for m in ANN[name]['marks']]
        if container: pg.locator(container).first.screenshot(path=f"shots/{name}.png")
        else: pg.screenshot(path=f"shots/{name}.png")
    def done(pg): pg.context.close()

    pg=page(nosticky=False); cap(pg,"first_screen",None,[(1,".logo"),(2,"nav#tabs"),(3,"#status .card"),(4,"#fw")]); done(pg)
    pg=page(); cap(pg,"status_now","#status > .card:nth-of-type(1)",[(1,"#stat-primary"),(2,"#flags")]); cap(pg,"status_details","#status > .card:nth-of-type(2)",[(1,"#stat-detail"),(2,"#camrefresh")]); done(pg)
    pg=page("recording"); cap(pg,"status_rec","#status .card"); done(pg)
    pg=page("fcdown"); cap(pg,"status_fcdown","main",[(1,"#flags")]); 
    pg.screenshot(path="shots/offline_banner.png",clip={"x":0,"y":0,"width":400,"height":240}); done(pg)
    pg=page("ready",adv=True); cap(pg,"status_diag","#status .card.adv-only"); done(pg)
    pg=page("repair"); cap(pg,"status_repair","#status .card"); done(pg)
    # Cam control
    pg=page(tab="cam-control",rec=2)
    cap(pg,"cc_record","#cam-control .card",[(1,"[name='record.mode']"),(2,"[name='record.switch.mode']"),(3,"[name='record.switch.channel']"),(4,"[data-aux-for='record.switch.channel']"),(5,"#rb-record"),(6,"[name='record.switch.threshold_us']")])
    cap(pg,"cc_preset","#preset-card",[(1,"[name='preset.enabled']"),(2,"[name='preset.channel']"),(3,"#rb-preset"),(4,"#zone-ids"),(5,"#zone-add"),(6,"#gp-zt"),(7,"#gp-live"),(8,"#gp-rows"),(9,"#gp-capture"),("a_zoneend","#zone-add"),("a_gp","#gp-builder"),("a_prot","#gp-rows h4:nth-of-type(2)")])
    done(pg)
    pg=page(tab="cam-control",rec=1); cap(pg,"cc_record_armed","#cam-control .card"); done(pg)
    pg=page(tab="cam-control",adv=True,rec=3)
    cap(pg,"cc_adv_rec","#cam-control .card","[]" and [(1,"details.adv.adv-only")])
    cap(pg,"cc_hilight","#hilight-card"); done(pg)
    # OSD
    pg=page("recording",tab="osd")
    cap(pg,"osd_preview","#osdprev",[(1,"#osdscreen"),(2,"#osdsim")])
    cap(pg,"osd_editor","#osdedit",[(1,"#palette"),(2,".oline"),(3,".oline .rawtog"),(4,".chip"),("a_l2","[data-i='1']")])
    cap(pg,"osd_settings","#osd",[]); done(pg)
    pg=page("ready",tab="osd"); pg.fill("#osdsim","") if False else None; cap(pg,"osd_lost","#osdprev"); done(pg)
    # Binding
    pg=page(tab="camera"); cap(pg,"binding","#camera",[(1,"#camlist"),(2,"#pairrow"),(3,"#camtest"),(4,".forget")]); done(pg)
    pg=page("nocam",tab="camera"); cap(pg,"binding_empty","#camera"); 
    pg.click('button[data-pair="gopro"]'); pg.wait_for_timeout(2500)
    cap(pg,"pair_modal","#pair .modal-card",[(1,"#pair .steps"),(2,"#pair .qrbox"),(3,"#pair-list"),(4,"#pair-go")]); done(pg)
    # Network / System / Clock
    pg=page(tab="network"); cap(pg,"network","#network",[(1,"[name='wifi.ssid']"),(2,"[name='wifi.password']"),(3,"[name='wifi.portal_trigger']"),(4,"#mspscan")]); done(pg)
    pg=page(adv=True,tab="network"); cap(pg,"network_adv","#network",[(1,"[name='msp.baud']"),(2,"[name='msp.swap_pins']"),(3,"[name='radio.ble_power']"),("a_radio","[name='radio.wifi_power']"),("a_mspend","[name='msp.swap_pins']")]); done(pg)
    pg=page(tab="system"); cap(pg,"system","#system",[(1,"#advmode"),(2,"#identify"),(3,"#factory"),(4,"#otafile")]); done(pg)
    pg=page(adv=True,tab="clock"); cap(pg,"clock","#clock",[(1,"[name='clock.enabled']"),(2,"[name='clock.min_sats']"),(3,"#syncnow")]); done(pg)
    pg=page("nag",nosticky=False); pg.wait_for_timeout(900); cap(pg,"pw_modal","#setup .modal-card"); done(pg)
    # footer bar
    pg=page(tab="osd",nosticky=False); pg.fill("[name='osd.refresh_hz']","6"); pg.wait_for_timeout(400); pg.screenshot(path="shots/savebar_dirty.png",clip={"x":0,"y":770,"width":400,"height":90}); done(pg)
    br.close()
json.dump(ANN,open("shots/annot.json","w"))
print("ok",len(ANN))
