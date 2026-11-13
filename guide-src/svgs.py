def modes():
    X0,PX=150,44.17
    x=lambda t:X0+t*PX
    rows=[("Armed",30,[(2,8)],"var(--mut)"),("Radio switch",64,[(4,10)],"var(--mut)"),
          ("When armed",126,[(2,8)],"var(--acc)"),("From a switch",160,[(4,10)],"var(--acc)"),
          ("Armed OR switch",194,[(2,10)],"var(--acc)"),("Armed AND switch",228,[(4,8)],"var(--acc)")]
    s='<div class="diagram"><svg viewBox="0 0 700 290" role="img" aria-label="Recording modes over time">'
    s+='<text x="10" y="14" class="m t9 b" style="letter-spacing:.08em">INPUTS</text><text x="10" y="108" class="m t9 b" style="letter-spacing:.08em">RECORDING MODE → CAMERA RECORDS</text>'
    for t,l in ((2,"arm"),(4,"switch on"),(8,"disarm"),(10,"switch off")):
        s+=f'<path d="M{x(t):.1f} 22 V262" stroke="var(--line)" stroke-width="1" stroke-dasharray="3 4"/><text x="{x(t):.1f}" y="278" text-anchor="middle" class="m t9">{l}</text>'
    for name,y,segs,col in rows:
        s+=f'<text x="10" y="{y+12}" class="t10">{name}</text>'
        s+=f'<path d="M{X0} {y+16} H{x(12):.1f}" stroke="var(--line)" stroke-width="1.2"/>'
        for a,b in segs:
            if col=="var(--acc)": s+=f'<rect x="{x(a):.1f}" y="{y}" width="{x(b)-x(a):.1f}" height="16" rx="3" fill="var(--acc)"/>'
            else: s+=f'<path d="M{x(a):.1f} {y+16} V{y+2} H{x(b):.1f} V{y+16}" fill="none" stroke="var(--fg)" stroke-width="2"/>'
    s+='</svg><p class="cap">Example: the aircraft arms at 2, the switch goes on at 4, the aircraft disarms at 8 and the switch goes off at 10. Start and stop delays are left out.</p></div>'
    return s
def zones():
    X=lambda us:40+(us-900)/1200*620
    s='<div class="diagram"><svg viewBox="0 0 700 250" role="img" aria-label="Zones on a radio switch">'
    cols=["var(--zone-1)","var(--zone-2)","var(--zone-3)"]
    edges=[900,1300,1700,2100]
    for i in range(3):
        s+=f'<rect x="{X(edges[i]):.1f}" y="38" width="{X(edges[i+1])-X(edges[i]):.1f}" height="22" fill="{cols[i]}" fill-opacity=".85" rx="{0}"/>'
    s+=f'<rect x="40" y="38" width="620" height="22" rx="6" fill="none" stroke="var(--line)" stroke-width="1.3"/>'
    for us in (1300,1700):
        s+=f'<rect x="{X(us)-8:.1f}" y="32" width="16" height="34" rx="5" fill="var(--fg)" stroke="var(--bg)" stroke-width="1.5"/><text x="{X(us):.1f}" y="24" text-anchor="middle" class="t10 b">{us}</text>'
    for us in (1000,1500,2000):
        s+=f'<path d="M{X(us):.1f} 70 V78" stroke="var(--line)" stroke-width="1.2"/><text x="{X(us):.1f}" y="90" text-anchor="middle" class="m t9">{us} µs</text>'
    s+=f'<path d="M{X(1500):.1f} 28 V70" stroke="var(--warn)" stroke-width="2"/><text x="{X(1500)+6:.1f}" y="21" class="t9" style="fill:var(--warn)">stick</text>'
    cards=[("Zone 1","4K · 60 fps","Lens: Wide","HyperSmooth: High","Protune on"),
           ("Zone 2","2.7K · 120 fps","Lens: Linear","HyperSmooth: Low","Protune on"),
           ("Zone 3","1080 · 240 fps","Lens: Wide","HyperSmooth: Off","")]
    for i,c in enumerate(cards):
        x0=40+i*212; col=cols[i]
        s+=f'<rect x="{x0}" y="112" width="196" height="108" rx="10" fill="var(--bg)" stroke="{col}" stroke-width="2"/>'
        s+=f'<text x="{x0+14}" y="134" class="b t12">{c[0]}</text>'
        for j,t in enumerate(c[1:]):
            if t: s+=f'<text x="{x0+14}" y="{154+j*17}" class="t10{" b" if j==0 else ""}">{t}</text>'
        cx=(X(edges[i])+X(edges[i+1]))/2
        s+=f'<path d="M{cx:.1f} 66 V112" stroke="{col}" stroke-width="1.6" stroke-dasharray="4 3" fill="none"/>'
    s+='<text x="350" y="242" text-anchor="middle" class="m t9">Example settings for a GoPro. The zone applies once the stick has rested in it for the hold time (400 ms).</text>'
    s+='</svg><p class="cap">A three-position switch split into three zones. The bar spans 900 to 2100 µs.</p></div>'
    return s
def led():
    X0,PX=250,73  # 6 s window
    rows=[("Booting","solid"),("Config portal open","double"),("Camera recording","solid"),
          ("No flight controller","fc"),("Camera lost","fast"),("Searching for camera","breathe"),
          ("Camera ready, idle","idle"),("Watchdog recovery (once)","fault"),("Identify (3 s)","ident")]
    s=f'<div class="diagram"><svg viewBox="0 0 700 {len(rows)*28+36}" role="img" aria-label="LED patterns">'
    for k in range(0,7):
        s+=f'<path d="M{X0+k*PX} 8 V{len(rows)*28+10}" stroke="var(--line)" stroke-width="1" stroke-dasharray="2 4"/><text x="{X0+k*PX}" y="{len(rows)*28+26}" text-anchor="middle" class="m t8">{k} s</text>'
    def on(t0,t1,y,op=1): return f'<rect x="{X0+t0*PX:.1f}" y="{y}" width="{(t1-t0)*PX:.1f}" height="12" rx="2" fill="var(--acc)" fill-opacity="{op}"/>'
    for i,(name,kind) in enumerate(rows):
        y=14+i*28
        s+=f'<text x="10" y="{y+10}" class="t10">{name}</text><rect x="{X0}" y="{y}" width="{6*PX}" height="12" rx="2" fill="var(--well)" stroke="var(--line)" stroke-width=".8"/>'
        if kind=="solid": s+=on(0,6,y)
        elif kind=="double":
            for k in range(6): s+=on(k+0.0,k+0.12,y)+on(k+0.24,k+0.36,y)
        elif kind=="fc":
            for k in (0,2,4): s+=on(k,k+0.12,y)+on(k+0.3,k+0.42,y)
        elif kind=="fast":
            t=0
            while t<6: s+=on(t,t+0.15,y); t+=0.3
        elif kind=="breathe":
            import math
            pts=[]
            for n in range(0,261):
                t=n/260*6; ph=(t%2.6)/2.6; v=0.5-0.5*math.cos(2*math.pi*ph)
                pts.append((X0+t*PX,y+12-12*v))
            d="M"+" L".join(f"{a:.1f} {b:.1f}" for a,b in pts)+f" L{X0+6*PX} {y+12} L{X0} {y+12} Z"
            s+=f'<path d="{d}" fill="var(--acc)" fill-opacity=".85"/>'
        elif kind=="idle":
            for k in (0,3): s+=on(k,k+0.12,y)
        elif kind=="fault":
            t=0
            for n in range(5): s+=on(t,t+0.15,y); t+=0.3
        elif kind=="ident":
            t=0
            while t<3: s+=on(t,t+0.2,y); t+=0.4
    s+='</svg><p class="cap">What a single-colour LED does over a six-second window.</p></div>'
    return s
SVG={"modes":modes,"zones":zones,"led":led}
