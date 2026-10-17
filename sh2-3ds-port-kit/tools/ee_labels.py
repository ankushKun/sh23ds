# Build a label JSON (globals + platform functions) for one sh2pc.exe build from ee_resolve.py output.
# Usage: python3 ee_labels.py <SH2EE checkout> <sh2pc.exe> <ee_resolved.json> <labels.json>
import re,json,sys,os,pefile
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from sh2version import detect
root,exe,resolved,out=sys.argv[1:5]
ver=detect(exe)['version']
pe=pefile.PE(exe); data=next(s for s in pe.sections if s.Name.startswith(b'.data\0'))
lo=pe.OPTIONAL_HEADER.ImageBase+data.VirtualAddress; hi=lo+data.Misc_VirtualSize
R=json.load(open(resolved))
src=open(os.path.join(root,'Patches/Common.cpp'),encoding='utf-8',errors='replace').read()
labels=[]
for m in re.finditer(r'\n[\w\s\*]+?\*\s*(Get(\w+?)Pointer)\(\)\s*\n\{(.*?)\n\}',src,re.S):
    fn,name,body=m.group(1),m.group(2),m.group(3)
    calls=[r for r in R if r['func']==fn and r['file'].endswith('Common.cpp') and r.get('value')]
    if len(calls)!=1: continue
    v=int(calls[0]['value'],16); off=0
    a=re.search(r'(\w+)\s*=\s*\([^)]*\)\s*ReadSearchedAddresses',body)
    if a:
        # ((DWORD)X + off) and ((DWORD)X) - off, e.g. RunOptionAddr = ControlType - 0x06
        o=re.search(r'\(\(DWORD\)\s*'+a.group(1)+r'\s*\)?\s*([+-])\s*(0x[0-9A-Fa-f]+|\d+)',body)
        if o: off=int(o.group(2),0)*(1 if o.group(1)=='+' else -1)
    if lo<=v+off<hi: labels.append(dict(addr=hex(v+off),name='g'+name,source='SH2EE '+fn))
# Platform labels found by decompiling init code (see Key addresses). v10 and dc verified; v11 not analysed.
PLATFORM={
 'v10':dict(g=[('0x93216c','g_hWnd'),('0x9321a8','g_pDirectInput8'),('0xa32890','g_pD3D8'),('0xa32894','g_pD3DDevice8'),('0xa32898','g_D3DPresentParams'),('0x1d7e820','g_pDirectSound')],
            f=[('0x4063b0','Win_CreateWindowAndDInput'),('0x4f6310','Gfx_CreateD3D8'),('0x4f6380','Gfx_CreateDevice'),('0x4f5db0','Gfx_EnumAdapterModes'),('0x514f10','Snd_Init_DSound_ADX'),('0x44a370','File_StartLoaderThread'),('0x44a200','File_LoaderThreadProc'),('0x518b60','Snd_StartThread'),('0x43d5b0','Movie_BinkPlay'),('0x44a0b0','File_GetSize'),('0x55fd70','Snd_StartAudioTimer')]),
 'dc':dict(g=[('0x934d6c','g_hWnd'),('0x934da8','g_pDirectInput8'),('0xa35490','g_pD3D8'),('0xa35494','g_pD3DDevice8'),('0xa35498','g_D3DPresentParams'),('0x1d81420','g_pDirectSound')],
            f=[('0x4063c0','Win_CreateWindowAndDInput'),('0x4f5e80','Gfx_CreateD3D8'),('0x4f5ef0','Gfx_CreateDevice'),('0x4f5920','Gfx_EnumAdapterModes'),('0x514b60','Snd_Init_DSound_ADX'),('0x44a510','File_StartLoaderThread'),('0x44a3a0','File_LoaderThreadProc'),('0x5187b0','Snd_StartThread'),('0x43d770','Movie_BinkPlay'),('0x44a250','File_GetSize'),('0x55e050','Snd_StartAudioTimer')])}
p=PLATFORM.get(ver,dict(g=[],f=[]))
labels+=[dict(addr=a,name=n,source='decomp') for a,n in p['g']]
json.dump(dict(version=ver,md5=detect(exe)['md5'],globals=labels,functions=[dict(addr=a,name=n) for a,n in p['f']]),open(out,'w'),indent=1)
print(ver,len(labels),'globals',len(p['f']),'functions')
