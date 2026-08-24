#!/usr/bin/env python3
"""Drive the WBRG1 ESPHome proxy: connect to a device and dump its GATT table."""
import socket, time, sys

HOST=sys.argv[1] if len(sys.argv)>1 else "192.168.0.175"
ADDR=int(sys.argv[2],16) if len(sys.argv)>2 else 0x582abd7d3152  # ESP32 BT MAC
ATYPE=int(sys.argv[3]) if len(sys.argv)>3 else 0

def vi(v):
    o=b""
    while True:
        b=v&0x7f; v>>=7; o+=bytes([b|(0x80 if v else 0)])
        if not v: return o
def fr(t,p=b""): return b"\x00"+vi(len(p))+vi(t)+p
def pu(f,v): return bytes([(f<<3)|0])+vi(v)
def rv(b,i):
    v=0;sh=0
    while True:
        x=b[i];i+=1;v|=(x&0x7f)<<sh
        if not x&0x80:return v,i
        sh+=7
def fields(pl):
    """yield (fnum, wire, val_or_bytes)"""
    i=0; out=[]
    while i<len(pl):
        tag,i=rv(pl,i); f=tag>>3; w=tag&7
        if w==0: v,i=rv(pl,i); out.append((f,0,v))
        elif w==2: l,i=rv(pl,i); out.append((f,2,pl[i:i+l])); i+=l
        elif w==5: out.append((f,5,pl[i:i+4])); i+=4
        elif w==1: out.append((f,1,pl[i:i+8])); i+=8
        else: break
    return out

buf=bytearray()
def recv_frames(s, secs):
    end=time.time()+secs; got=[]
    while time.time()<end:
        try: d=s.recv(4096)
        except socket.timeout: d=b""
        if d: buf.extend(d)
        i=0
        while i<len(buf):
            if buf[i]!=0: return got
            try:
                j=i+1; ln,j=rv(buf,j); ty,j=rv(buf,j)
            except IndexError: break
            if j+ln>len(buf): break
            got.append((ty,bytes(buf[j:j+ln]))); i=j+ln
        del buf[:i]
        if any(t==72 for t,_ in got): break   # GetServicesDone
    return got

def uuid_str(flds):
    # 128-bit: two uint64 (field 1) high,low ; 16-bit: short_uuid
    u64=[v for f,w,v in flds if f in (1,) and w==0]
    if len(u64)>=2:
        hi,lo=u64[0],u64[1]; b=hi.to_bytes(8,'big')+lo.to_bytes(8,'big')
        h=b.hex(); return f"{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:32]}"
    short=[v for f,w,v in flds if f in (4,5) and w==0]
    if short: return f"{short[0]:04x} (16-bit)"
    return "?"

s=socket.create_connection((HOST,6053),timeout=6); s.settimeout(1)
s.sendall(fr(1)); s.sendall(fr(9))          # Hello, DeviceInfo
recv_frames(s,1)
print(f"connecting to {ADDR:012x} (type {ATYPE})...")
s.sendall(fr(68, pu(1,ADDR)+pu(2,4)+pu(4,ATYPE)))   # BluetoothDeviceRequest connect v3
got=recv_frames(s,8)
conn=[pl for t,pl in got if t==69]
if conn:
    fl=fields(conn[-1]); d={f:v for f,w,v in fl}
    print(f"  ConnectionResponse: connected={d.get(2)} mtu={d.get(3)} err={d.get(4)}")
    if not d.get(2):
        print("  NOT connected — is the ESP32 in range/running?"); s.close(); sys.exit(1)
else:
    print("  no ConnectionResponse"); s.close(); sys.exit(1)

print("requesting GATT services...")
s.sendall(fr(70, pu(1,ADDR)))
got=recv_frames(s,12)
svcs=[pl for t,pl in got if t==71]
done=any(t==72 for t,_ in got)
print(f"  got {len(svcs)} service response(s), done={done}\n")
for pl in svcs:
    fl=fields(pl)
    svc_subs=[v for f,w,v in fl if f==2 and w==2]
    for sub in svc_subs:
        sf=fields(sub)
        suuid=uuid_str(sf)
        shandle=next((v for f,w,v in sf if f==2 and w==0),None)
        print(f"Service {suuid} (handle {shandle})")
        for f,w,v in sf:
            if f==3 and w==2:
                cf=fields(v)
                cuuid=uuid_str(cf)
                chandle=next((vv for ff,ww,vv in cf if ff==2 and ww==0),None)
                props=next((vv for ff,ww,vv in cf if ff==3 and ww==0),0)
                pr=[]
                if props&0x02:pr.append("read")
                if props&0x04:pr.append("write-nr")
                if props&0x08:pr.append("write")
                if props&0x10:pr.append("notify")
                if props&0x20:pr.append("indicate")
                print(f"    char {cuuid} handle={chandle} props={props:#x} [{','.join(pr)}]")
s.close()
