# Map functions between two builds of sh2pc.exe by masked-bytes hashing (addresses and branch targets masked).
# Usage: match_versions.py a.exe a_functions.tsv b.exe b_functions.tsv out.tsv
import sys,csv,hashlib,collections,pefile
from capstone import Cs,CS_ARCH_X86,CS_MODE_32
from capstone.x86 import X86_OP_IMM,X86_OP_MEM
md=Cs(CS_ARCH_X86,CS_MODE_32); md.detail=True
def load(exe,tsv):
    pe=pefile.PE(exe); img=pe.get_memory_mapped_image(); base=pe.OPTIONAL_HEADER.ImageBase
    F=[r for r in csv.DictReader(open(tsv),delimiter='\t') if r['thunk']=='false' and not r['entry'].startswith('EXTERNAL')]
    return img,base,F
def sig(img,base,entry,size):
    o=entry-base; code=img[o:o+size]; out=[]
    for ins in md.disasm(code,entry):
        b=bytearray(ins.bytes)
        if ins.group(1) or ins.group(2) or ins.mnemonic.startswith('j') or ins.mnemonic=='call':  # branches
            out.append(ins.mnemonic); continue
        for op in ins.operands:
            v=op.imm if op.type==X86_OP_IMM else (op.mem.disp if op.type==X86_OP_MEM else None)
            if v is not None and 0x400000<=(v&0xffffffff)<0x2700000:
                vb=(v&0xffffffff).to_bytes(4,'little'); i=bytes(b).find(vb)
                if i>=0: b[i:i+4]=b'\0\0\0\0'
        out.append(bytes(b).hex())
    return hashlib.sha1('|'.join(out).encode()).hexdigest(), len(out)
def index(img,base,F):
    d=collections.defaultdict(list)
    for r in F:
        e=int(r['entry'],16); s=int(r['size'])
        if s<8: continue
        h,n=sig(img,base,e,s); d[h].append((e,s,r['name']))
    return d
A=index(*load(sys.argv[1],sys.argv[2])); B=index(*load(sys.argv[3],sys.argv[4]))
pairs=[]
for h,la in A.items():
    lb=B.get(h)
    if lb and len(la)==1 and len(lb)==1: pairs.append((la[0][0],lb[0][0],la[0][1]))
with open(sys.argv[5],'w') as f:
    f.write('a_entry\tb_entry\tsize\n')
    for a,b,s in sorted(pairs): f.write(f'{a:#x}\t{b:#x}\t{s}\n')
na=sum(len(v) for v in A.values()); print('A funcs',na,'B funcs',sum(len(v) for v in B.values()),'unique matches',len(pairs), f'{len(pairs)/na:.1%}')
