# Identify an sh2pc.exe build. Usage: python3 sh2version.py sh2pc.exe
# Version bytes are the ones Silent Hill 2 Enhancements checks at 0x401005 (dllmain.cpp GetGameVersion).
import sys,hashlib,pefile
SIG={'v10':'e9562500 00e97125 0000e9fc 690000e9 77060000','v11':'e9562500 00e97125 0000e99c 6a0000e9 77060000','dc':'e9662500 00e98125 0000e9ac 6a0000e9 77060000'}
SIG={k:bytes.fromhex(v.replace(' ','')) for k,v in SIG.items()}
GOOD={'c65baee155f3f47b4538e67ff9073d25','1be408eca5e437e9d7f63461e2cb9e69','1e797657100d0b6e6de23cffec94a331','957b9349b0dd8a48e64e6d7218b7522c','b2d4a1f0cff2542f9f856150cd770a16','f3f5360298b6d0f6f2e09cf24a0925d1','c16ed8436fdcfc53a07ab21723994770','0afa171ea93ee1441682f49969e6f02c','f9ccf14b5e5a31d9fe50e5b5efe50ed7','37caa31bdb473d9019be2491bd8dc914'}
def detect(path):
    d=open(path,'rb').read(); pe=pefile.PE(data=d); img=pe.get_memory_mapped_image()
    b=img[0x1005:0x1019]; ver=next((k for k,v in SIG.items() if v==b),'unknown')
    md5=hashlib.md5(d).hexdigest()
    secs=[s.Name.rstrip(b'\0').decode(errors='replace') for s in pe.sections]
    return dict(version=ver,md5=md5,sha1=hashlib.sha1(d).hexdigest(),sha256=hashlib.sha256(d).hexdigest(),size=len(d),
                ee_known_good=md5 in GOOD,securom=any(s.startswith('.cms_') for s in secs),sections=secs)
if __name__=='__main__':
    for k,v in detect(sys.argv[1]).items(): print(f'{k:14} {v}')
