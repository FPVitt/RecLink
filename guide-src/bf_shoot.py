import json, re, sys
from playwright.sync_api import sync_playwright

FAKE = r"""
(() => {
const enc=s=>[...s].map(c=>c.charCodeAt(0));
const u16=v=>[v&255,(v>>8)&255], u32=v=>[v&255,(v>>8)&255,(v>>16)&255,(v>>>24)&255];
const str=s=>[s.length,...enc(s)];
const R={}; window.__R=R; window.__log=[];
R[1]=()=>[0,1,47];
R[2]=()=>enc('BTFL');
R[3]=()=>[4,6,0];
R[4]=()=>[...enc('SPBE'),...u16(0),2,1,...str('SPEEDYBEEF405V4'),...str('SPEEDYBEEF405V4'),...str('SPBE'),...new Array(32).fill(0),4,2,...u16(8000),...u32(0),0,0];
R[5]=()=>[...enc('2026-06-01'.padEnd(11,' ').slice(0,11)),...enc('12:00:00'),...enc('abcdef1')];
R[160]=()=>new Array(12).fill(1);
R[108]=()=>[0,0,0,0,0,0];
R[110]=()=>[0,0,0,0,0,0,0];
const ports=[[20,1,5],[0,64,5],[1,0,5],[2,1,5],[3,0,5],[5,0,5]];
R[4105]=()=>[ports.length,...ports.flatMap(([id,m,b])=>[id,...u32(m),b,0,0,5])];
R[36]=()=>[...u32((1<<3)|(1<<18)|(1<<22))];
R[105]=()=>{const ch=window.__CH||[1500,1500,1000,1500,1000,2000,1000,1000,1000,1000,1500,1500,1500,1500,1500,1500,1500,1500];return ch.flatMap(u16);};
R[64]=()=>[0,1,3,2,4,5,6,7,8,9,10,11,12,13,14,15,16,17];
const N=87, pos=new Array(N).fill(0);
const vis=(x,y)=>0x0800|(y<<5)|(x&31);
pos[1]=vis(2,18); pos[0]=vis(2,17); pos[5]=vis(26,18);
pos[81]=vis(2,1); pos[82]=vis(2,2); pos[83]=vis(2,3); pos[84]=vis(2,4);
window.__POS=pos;
R[189]=()=>[53,20];
R[84]=()=>[65,3, 1,70,...u16(2200),0,N,...u16(100),
 ...window.__POS.flatMap(u16),
 31,...new Array(31).fill(0).map((_,i)=>i<12?1:0),
 3,...u16(0x0F00),...u16(0),...u16(0),
 ...u16(0),
 22,...[0xFF,0xFF,0x3F,0],
 1,1,0,24,11,
 ...u16(70),...u16(0xFF9C)];
let buf=[], ctrl=null;
const send=(arr)=>{ctrl&&ctrl.enqueue(new Uint8Array(arr));};
const crc8=(a)=>{let c=0;for(const b of a){c^=b;for(let i=0;i<8;i++)c=(c&0x80)?((c<<1)^0xD5)&255:(c<<1)&255;}return c;};
const reply=(v2,cmd,payload)=>{
 if(v2||payload.length>254||cmd>255){const h=[0,...u16(cmd),...u16(payload.length)];send([36,88,62,...h,...payload,crc8([...h,...payload])]);}
 else{let c=payload.length^cmd;for(const b of payload)c^=b;send([36,77,62,payload.length,cmd,...payload,c&255]);}
};
const toArr=(ch)=>ch instanceof ArrayBuffer?Array.from(new Uint8Array(ch)):Array.from(new Uint8Array(ch.buffer,ch.byteOffset,ch.byteLength));
const incoming=(v2,cmd,pl)=>{window.__log.push(cmd);const f=R[cmd];reply(v2,cmd,f?f(pl):[]);};
const handle=(chunk)=>{buf.push(...toArr(chunk));
 while(buf.length>=6){const i=buf.indexOf(36); if(i<0){buf=[];return;} if(i>0)buf.splice(0,i); if(buf.length<6)return;
  if(buf[1]===77){const sz=buf[3],need=6+sz; if(buf.length<need)return; const cmd=buf[4],pl=buf.slice(5,5+sz);buf.splice(0,need);incoming(false,cmd,pl);}
  else if(buf[1]===88){if(buf.length<9)return;const sz=buf[6]|buf[7]<<8,need=9+sz;if(buf.length<need)return;const cmd=buf[4]|buf[5]<<8,pl=buf.slice(8,8+sz);buf.splice(0,need);incoming(true,cmd,pl);}
  else buf.splice(0,1);}
};
const fake={getInfo:()=>({usbVendorId:0x0483,usbProductId:0x5740}),
 async open(){buf=[];this.readable=new ReadableStream({start(c){ctrl=c}});this.writable=new WritableStream({write(ch){handle(ch)}});},
 async close(){try{ctrl&&ctrl.close()}catch(e){} this.readable=null;this.writable=null;},
 readable:null,writable:null,addEventListener(){},removeEventListener(){},async forget(){},async setSignals(){},async getSignals(){return{}}};
navigator.serial.getPorts=async()=>[fake];
navigator.serial.requestPort=async()=>fake;
window.__fake=fake;
try{localStorage.setItem('userLanguageSelect',JSON.stringify({userLanguageSelect:'en'}));localStorage.setItem('firstRun',JSON.stringify({firstRun:false}));localStorage.setItem('darkTheme',JSON.stringify({darkTheme:2}));}catch(e){}
})();
"""

def connect(pg):
    pg.goto("https://app.betaflight.com/")
    pg.wait_for_timeout(5000)
    for t in ("OK","Close"):
        b=pg.get_by_role("button",name=t)
        if b.count(): b.first.click(); pg.wait_for_timeout(500)
    b=pg.get_by_role("button",name=re.compile("STM"))
    b.first.click()
    pg.wait_for_timeout(6000)

def dismiss(pg):
    for _ in range(3):
        b=pg.get_by_role('button',name='OK')
        if b.count(): b.first.click(); pg.wait_for_timeout(700)

def tab(pg,name):
    dismiss(pg)
    pg.locator("#tabs, nav, aside").first  # noqa
    pg.get_by_text(name,exact=True).first.click()
    pg.wait_for_timeout(3000)
    dismiss(pg)

def main(out="shots"):
    import os
    ann=json.load(open("shots/annot.json"))
    with sync_playwright() as p:
        br = p.chromium.launch(channel="chrome", headless=True)
        ctx = br.new_context(viewport={"width":1280,"height":860}, device_scale_factor=2, locale="en-US", color_scheme="dark")
        pg = ctx.new_page()
        pg.add_init_script(FAKE)
        connect(pg)
        pg.wait_for_timeout(9000); dismiss(pg)
        # ---- Ports
        tab(pg,"Ports"); pg.wait_for_timeout(1500)
        clip=dict(x=270,y=180,width=314,height=262)
        pg.screenshot(path=f"{out}/bf_ports.png",clip=clip)
        def rel(b): return [b[0]-clip["x"],b[1]-clip["y"],b[2],b[3]]
        ann["bf_ports"]={"w":clip["width"],"h":clip["height"],"marks":[
            {"n":1,"box":rel([358,328,40,28])},{"n":2,"box":rel([402,327,100,30])}]}
        # ---- Receiver
        tab(pg,"Receiver"); pg.wait_for_timeout(1500)
        clip=dict(x=268,y=562,width=398,height=166)
        pg.screenshot(path=f"{out}/bf_receiver.png",clip=clip)
        ann["bf_receiver"]={"w":clip["width"],"h":clip["height"],"marks":[{"n":1,"box":[349-clip["x"]+8,592-clip["y"],265,22]}]}
        # ---- OSD
        pg.set_viewport_size({"width":1280,"height":1500})
        tab(pg,"OSD"); pg.wait_for_timeout(2500)
        clip=dict(x=296,y=188,width=470,height=244)
        pg.screenshot(path=f"{out}/bf_osd_preview.png",clip=clip)
        ann["bf_osd_preview"]={"w":clip["width"],"h":clip["height"],"marks":[]}
        pg.get_by_placeholder("Search...").fill("Custom message")
        for _ in range(20):
            pg.wait_for_timeout(1000); dismiss(pg)
            pl=pg.get_by_text("Elements",exact=True).first.bounding_box()
            mm=pg.get_by_text("Custom message 1",exact=True).first.bounding_box()
            if mm and pl and mm["y"]-pl["y"]<150: break
        pg.wait_for_timeout(500)
        pill=pg.get_by_text("Elements",exact=True).first.bounding_box()
        m4=pg.get_by_text("Custom message 4",exact=True).first.bounding_box()
        m1=pg.get_by_text("Custom message 1",exact=True).first.bounding_box()
        print("pill",pill,"m1",m1,"m4",m4)
        x0=pill["x"]-22; y0=pill["y"]-12
        clip=dict(x=x0,y=y0,width=340,height=(m4["y"]+m4["height"]+22)-y0)
        pg.screenshot(path=f"{out}/bf_osd.png",clip=clip)
        ann["bf_osd"]={"w":clip["width"],"h":clip["height"],"marks":[{"n":1,"box":[m1["x"]-30-x0,m1["y"]-4-y0,170,m4["y"]+m4["height"]-m1["y"]+8]}]}
        json.dump(ann,open("shots/annot.json","w"),indent=1)
        print(ann["bf_ports"],ann["bf_receiver"],ann["bf_osd"])
        br.close()

if __name__ == "__main__":
    main()
