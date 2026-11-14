import sys, collections
# flight.py trace [step]: per time step, the most data in flight, retransmissions and
# sender gaps of one relay.py trace (TRACE=file); connections split at SYNs
rows=[l.split() for l in open(sys.argv[1])]
step=float(sys.argv[2]) if len(sys.argv)>2 else 0.1
base=None; hs=0; ha=0; t0=None; lastsend=None; lastbase=None
b=collections.defaultdict(lambda:[0,0,'',0])
for r in rows:
    t=float(r[0]); d=r[1]; w=r[2]; sq=int(r[3]); ak=int(r[4]); pl=int(r[5]); win=int(r[6])
    flags=int(r[9]) if len(r)>9 else 0
    if d=='ba' and flags & 2: base=None; continue  # SYN: a new connection
    if t0 is None: t0=t
    i=int((t-t0)/step)
    if d=='ba' and pl>0:
        if base is None: base=sq; hs=0; ha=0; b[i][2]+='NEWCONN '
        o=(sq-base)&0xffffffff
        e=o+pl
        if e>hs: hs=e
        else: b[i][1]+=1
        if lastsend and t-lastsend>0.1: b[i][2]+='GAP%.0fms '%((t-lastsend)*1000)
        lastsend=t
    if d=='ab' and base is not None:
        a=(ak-base)&0xffffffff
        if a<1<<31 and a>ha: ha=a
        b[i][0]=max(b[i][0],(hs-ha)//1448); b[i][3]=max(b[i][3],win)
for i in sorted(b):
    print("%.1f fmax=%d R=%d rwin=%d %s"%(i*step,b[i][0],b[i][1],b[i][3],b[i][2]))
