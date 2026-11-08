import ctypes, json, sys
from PIL import Image, ImageDraw, ImageFont
D='/usr/share/fonts/truetype/dejavu/'
def path(flags):
    mono = flags & 4; b = flags & 1; i = flags & 2
    base = 'DejaVuSansMono' if mono else 'DejaVuSerif'
    suf = ('-BoldOblique' if mono else '-BoldItalic') if (b and i) else ('-Bold' if b else (('-Oblique' if mono else '-Italic') if i else ''))
    if mono and not suf: return D+'DejaVuSansMono.ttf'
    return D+base+suf+'.ttf'
cache={}
def font(px, flags):
    k=(px, flags & 7)
    if k not in cache: cache[k]=ImageFont.truetype(path(flags), px)
    return cache[k]
WF=ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int32), ctypes.c_int)
MF=ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int))
def width(px, fl, s, n):
    t=''.join(chr(s[i]) for i in range(n))
    return int(round(font(px, fl).getlength(t)))
def metrics(px, fl, a, d):
    asc, desc = font(px, fl).getmetrics(); a[0]=asc; d[0]=desc
wf=WF(width); mf=MF(metrics)
lib=ctypes.CDLL('/tmp/mdt/libbridge.so')
lib.bridge_layout.restype=ctypes.c_char_p
lib.bridge_layout.argtypes=[ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, WF, MF, ctypes.POINTER(ctypes.c_int)]
def render(md, out, W=940, px=16, dark=1):
    b=md.encode(); ch=ctypes.c_int()
    j=json.loads(lib.bridge_layout(b, len(b), W, px, dark, wf, mf, ctypes.byref(ch)).decode())
    H=max(ch.value, 100)
    col=lambda c:((c>>16)&255,(c>>8)&255,c&255)
    im=Image.new('RGB',(W,H),col(j['bg'])); d=ImageDraw.Draw(im)
    for it in j['items']:
        k=it['k']; x,y,w,h=it['x'],it['y'],it['w'],it['h']; c=col(it['c'])
        if k==0:
            d.text((x,y+it['asc']), it['t'], font=font(it['px'],it['fl']), fill=c, anchor='ls')
            if it['fl']&8: d.line((x,y+it['asc']+2,x+w,y+it['asc']+2), fill=c)
            if it['fl']&16: d.line((x,y+it['asc']*0.6,x+w,y+it['asc']*0.6), fill=c)
        elif k==1: d.rectangle((x,y,x+w-1,y+h-1), fill=c)
        elif k==2: d.rounded_rectangle((x,y,x+w-1,y+h-1), radius=3, outline=c)
        elif k==3: d.ellipse((x,y,x+w,y+h), fill=c)
        elif k==4: d.ellipse((x,y,x+w,y+h), outline=c)
        elif k==6:
            d.rectangle((x,y,x+w-1,y+h-1), fill=(60,70,80), outline=(120,130,140)); d.line((x,y,x+w,y+h), fill=(120,130,140)); d.line((x,y+h,x+w,y), fill=(120,130,140))
        elif k==5:
            d.rectangle((x,y,x+w-1,y+h-1), outline=c)
            if it['aux']: d.line((x+2,y+h/2,x+w/2-1,y+h-3,x+w-3,y+3), fill=c, width=2)
    im.save(out); return j, ch.value
if __name__=='__main__':
    md=open(sys.argv[1],encoding='utf8').read()
    j,h=render(md, sys.argv[2], int(sys.argv[3]) if len(sys.argv)>3 else 940, dark=int(sys.argv[4]) if len(sys.argv)>4 else 1)
    print(len(j['items']),'items, height',h)
