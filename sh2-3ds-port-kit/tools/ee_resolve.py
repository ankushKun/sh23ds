# Statically resolve every Silent Hill 2 Enhancements address lookup against an sh2pc.exe.
# Usage: python3 ee_resolve.py <SH2EE checkout> <sh2pc.exe> <out.json> [v10|v11|dc]   (version auto-detected if omitted)
import re,os,sys,json,pefile,collections
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from sh2version import detect
root,exe,out=sys.argv[1:4]
ver=sys.argv[4] if len(sys.argv)>4 else detect(exe)['version']
col={'v10':2,'v11':3,'dc':4}[ver]
pe=pefile.PE(exe); img=pe.get_memory_mapped_image(); base=pe.OPTIONAL_HEADER.ImageBase
def rd(va,n): o=va-base; return img[o:o+n] if 0<=o<len(img) else b''
call_re=re.compile(r'(SearchAndGetAddresses|ReadSearchedAddresses|CheckMultiMemoryAddress)\s*\(\s*(?:\([^)]*\)\s*)?(0x[0-9A-Fa-f]+)\s*,\s*(?:\([^)]*\)\s*)?(0x[0-9A-Fa-f]+)\s*,\s*(?:\([^)]*\)\s*)?(0x[0-9A-Fa-f]+)\s*,\s*(?:\([^)]*\)\s*)?(\w+)\s*,\s*([^,]+?)\s*,\s*(?:(-?\s*0x[0-9A-Fa-f]+|-?\s*\d+)\s*,)?')
arr_re=re.compile(r'(?:constexpr\s+)?(?:BYTE|unsigned char)\s+(\w+)\s*\[\s*\d*\s*\]\s*(?:=)?\s*\{([^}]*)\}')
fn_re=re.compile(r'^(?:[A-Za-z_][\w\*\s:<>]*\s+\**)([A-Za-z_]\w*(?:::\w+)?)\s*\([^;]*\)\s*$')
res=[];stats=collections.Counter()
for dp,_,fs in os.walk(root):
  for f in fs:
    if not f.endswith('.cpp'): continue
    p=os.path.join(dp,f); txt=open(p,encoding='utf-8',errors='replace').read()
    arrays={m.group(1):bytes(int(x,0) for x in re.findall(r'0x[0-9A-Fa-f]+|\b\d+\b',m.group(2))) for m in arr_re.finditer(txt)}
    lines=txt.splitlines(); funcs=[]
    for i,l in enumerate(lines):
      m=fn_re.match(l)
      if m and i+1<len(lines) and lines[i+1].strip().startswith('{'): funcs.append((i,m.group(1)))
    for m in call_re.finditer(txt):
      kind=m.group(1); addr=int(m.group(col),16); arr=m.group(5); delta=m.group(7)
      line=txt.count('\n',0,m.start())+1
      fn=[n for i,n in funcs if i<line]; fn=fn[-1] if fn else None
      pat=arrays.get(arr); r=dict(file=os.path.relpath(p,root),line=line,func=fn,kind=kind,version=ver,addr=hex(addr),
                                 addr10=m.group(2),addr11=m.group(3),addrdc=m.group(4))
      if pat and addr:
        r['exact']= rd(addr,len(pat))==pat
        found=addr if r['exact'] else None
        if not found:
          o=img.find(pat,max(0,addr-base-0x800),addr-base-0x800+2600)
          if o>=0: found=o+base
        r['found']=hex(found) if found else None
        if found and delta is not None:
          d=int(delta.replace(' ',''),0); r['delta']=d
          if kind=='ReadSearchedAddresses': r['value']=hex(int.from_bytes(rd(found+d,4),'little'))
          else: r['target']=hex(found+d)
        stats['exact' if r['exact'] else ('near' if found else 'miss')]+=1
      else: stats['nopattern']+=1
      res.append(r)
json.dump(res,open(out,'w'),indent=1)
print(ver,dict(stats),len(res))
