# Texture census for Silent Hill 2 PC data/ (format layout from iOrange/sh2tex).
# Usage: python3 texture_census.py "/path/to/Silent Hill 2/data"
import os,struct,sys,collections,json
root=sys.argv[1]
FMT={0:'DXT1',1:'DXT2',2:'DXT3',3:'DXT4',4:'DXT5',8:'PAL8',24:'RGBX8',32:'RGBA8'}
def parse_container(b,off,end,out):
    if end-off<16: return False
    if struct.unpack_from('<I',b,off)[0]!=0x19990901: return False
    p=off+16
    while end-p>=16:
        if struct.unpack_from('<I',b,p)[0]==0: p+=16; continue
        _,w,h,_,_,nspr,_=struct.unpack_from('<IHHHHHH',b,p); p+=32
        for _ in range(nspr):
            sid,x,y,sw,sh,fmt,comp,imp,ds,ds2,u0,bw,bh,mk=struct.unpack_from('<IHHHHBBHIIIBBH',b,p); p+=32
            if mk!=0x9900: return False
            if ds>0:
                if comp: exp=max(1,(w+3)//4)*max(1,(h+3)//4)*(8 if fmt==0 else 16)
                elif fmt==8: exp=w*h
                else: exp=w*h*4
                out.append((FMT.get(fmt,str(fmt)),w,h,exp)); p+=exp
                if fmt==8: p+=32+1024
    return True
stats=collections.Counter();size=collections.Counter();dims=collections.Counter();per_file={};fails=[]
for dp,_,fs in os.walk(root):
    for fn in fs:
        ext=fn.rsplit('.',1)[-1].lower()
        if ext not in('map','tex','mdl','tbn2'): continue
        path=os.path.join(dp,fn); b=open(path,'rb').read(); out=[]
        try:
            if ext=='map':
                mg,_,n,_=struct.unpack_from('<IIII',b,0)
                if mg!=0x20010510: fails.append(path); continue
                p=16
                for _ in range(n):
                    t,s,_,_=struct.unpack_from('<IIII',b,p); p+=16
                    if t==2: parse_container(b,p,p+s,out)
                    p+=s
            elif ext=='mdl':
                _,_,ntex,toff=struct.unpack_from('<IIII',b,0)
                if ntex: parse_container(b,toff,len(b),out)
            elif not parse_container(b,0,len(b),out): fails.append(path)
        except Exception as e: fails.append(f'{path}: {e}')
        for f,w,h,s in out: stats[f]+=1; size[f]+=s; dims[max(w,h)]+=1
        per_file[os.path.relpath(path,root)]=dict(bytes=len(b),tex_bytes=sum(x[3] for x in out),textures=len(out))
print('counts',dict(stats)); print('MB',{k:round(v/2**20,1) for k,v in size.items()})
print('max-dim histogram',sorted(dims.items())); print('unparsed',len(fails))
json.dump(per_file,open('texture_census.json','w'),indent=1)
