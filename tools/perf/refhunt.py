import sys
from capstone import *
from capstone.x86 import *
data=open("default.xbe","rb").read()
SECS=[(".text",0x2000,0x1fa1cc,0x12000),("D3D",0x1fd000,0xede0,0x20c1e0),(".data",0x277000,0x893f8,0x27a500)]
md=Cs(CS_ARCH_X86,CS_MODE_32); md.detail=True
TARGETS=[int(x,16) for x in sys.argv[1:]] or [0x351bdc]
ins=[]
for nm,ro,rs,va in SECS:
    if nm==".data": continue
    code=data[ro:ro+rs]; off=0; n=len(code)
    while off<n:
        got=False
        for i in md.disasm(code[off:off+16],va+off):
            ins.append(i); off+=i.size; got=True; break
        if not got: off+=1
ins.sort(key=lambda i:i.address)
for T in TARGETS:
    print(f"\n===== references to 0x{T:08x} =====")
    for k,i in enumerate(ins):
        hit=False; kind=""
        for op in i.operands:
            if op.type==X86_OP_IMM and (op.imm&0xffffffff)==T: hit=True; kind="imm"
            if op.type==X86_OP_MEM and (op.mem.disp&0xffffffff)==T: hit=True; kind="mem[disp]"
        if hit:
            # is it write to the global (mov [T], reg) or read (mov reg,[T]) ?
            rw="?"
            if i.mnemonic in ("mov","lea") and i.operands and i.operands[0].type==X86_OP_MEM and (i.operands[0].mem.disp&0xffffffff)==T:
                rw="WRITE-global"
            elif kind=="mem[disp]":
                rw="read-global"
            elif kind=="imm":
                rw="addr-imm"
            print(f"  0x{i.address:08x}: {i.mnemonic:6s} {i.op_str:40s} [{rw}]")
