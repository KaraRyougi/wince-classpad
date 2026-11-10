# Rasterise hinted TrueType fonts the way Windows 2000/XP draws them without
# ClearType: FreeType mono target, full bytecode hinting, 1 bit per pixel.
import freetype, json, sys
F='/System/Library/Fonts/Supplemental/'
FACES={('tahoma',0):F+'Tahoma.ttf',('tahoma',1):F+'Tahoma Bold.ttf',
       ('trebuchet',0):F+'Trebuchet MS.ttf',('trebuchet',1):F+'Trebuchet MS Bold.ttf'}
SIZES={('tahoma',0):range(6,16),('tahoma',1):range(6,16),('trebuchet',0):[10,13,21,26],('trebuchet',1):[10,13,28]}
CHARS=[chr(c) for c in range(32,127)]+list('®·×…▲▼•')
FLAGS=freetype.FT_LOAD_RENDER|freetype.FT_LOAD_TARGET_MONO|freetype.FT_LOAD_MONOCHROME
out={}
for (fam,b),path in FACES.items():
  face=freetype.Face(path)
  for px in SIZES[(fam,b)]:
    face.set_pixel_sizes(0,px)
    m=face.size; gl={}
    for ch in CHARS:
      if face.get_char_index(ch)==0: continue
      face.load_char(ch,FLAGS)
      s=face.glyph; bm=s.bitmap
      rows=[]
      for r in range(bm.rows):
        bits=0
        for c in range(bm.width):
          byte=bm.buffer[r*bm.pitch+(c>>3)]
          if byte&(0x80>>(c&7)): bits|=1<<(bm.width-1-c)
        rows.append(format(bits,'x'))
      gl[ch]=[s.advance.x>>6,s.bitmap_left,s.bitmap_top,bm.width,','.join(rows)]
    out[f'{fam}|{b}|{px}']={'asc':m.ascender>>6,'g':gl}
sys.stdout.write('const BMF='+json.dumps(out,ensure_ascii=False,separators=(',',':'))+';\n')
