import sys
# gaps.py trace: sender gaps > 150 ms; whether the relay saw anything at all meanwhile
rows=[l.split() for l in open(sys.argv[1])]
times=[float(r[0]) for r in rows]
lastdata=None; out=[]
for i,r in enumerate(rows):
    t=float(r[0])
    if r[1]=='ba' and int(r[5])>0:
        if lastdata is not None and t-lastdata>0.15:
            # any frame (either direction) strictly inside the gap?
            inside=sum(1 for x in times if lastdata+0.001<x<t-0.001)
            out.append("gap %.3f-%.3f %.0f ms, %d frames meanwhile, resumes seq %s"%(lastdata,t,(t-lastdata)*1000,inside,r[3]))
        lastdata=t
print(len(out),"gaps"); print("\n".join(out[:30]))
