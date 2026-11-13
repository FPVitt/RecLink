import sys, os
from playwright.sync_api import sync_playwright
import pypdfium2 as pdfium
from PIL import Image
D=os.getcwd()
with sync_playwright() as p:
    br=p.chromium.launch(channel="chrome")
    for name in sys.argv[1:]:
        pg=br.new_page(); pg.goto(f"file://{D}/{name}.html"); pg.wait_for_timeout(600)
        pg.pdf(path=f"{name}.pdf",prefer_css_page_size=True,print_background=True); pg.close()
        pdf=pdfium.PdfDocument(f"{name}.pdf"); n=len(pdf)
        os.makedirs(f"pv/{name}",exist_ok=True); ims=[]
        for i in range(n):
            im=pdf[i].render(scale=0.8).to_pil(); im.save(f"pv/{name}/p{i+1:02d}.png"); ims.append(im)
        w,h=ims[0].size; cols=min(n,6); tw,th=int(w*0.5),int(h*0.5); rows=(n+cols-1)//cols
        s=Image.new("RGB",(cols*(tw+6),rows*(th+6)),(80,80,80))
        for j,im in enumerate(ims): s.paste(im.resize((tw,th)),((j%cols)*(tw+6),(j//cols)*(th+6)))
        s.save(f"pv/{name}_contact.png"); print(name,n,"pages")
    br.close()
