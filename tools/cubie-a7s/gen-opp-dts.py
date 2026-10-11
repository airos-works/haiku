#!/usr/bin/env python3
"""Emit the A733 CPU/DSU OPP tables of Radxa's BSP device tree (the 26 MHz
crystal columns, opp-microvolt-26m-vfXXXX) as air/OS device tree nodes with
opp-microvolt-vfXXXX properties, the body of
data/dts/cubie-a7s/sun60i-a733-opp-26m.dtsi. Needs fdtget (device-tree-compiler).
Usage: gen-opp-dts.py <bsp.dtb>"""
import subprocess, sys
dtb = sys.argv[1]
bins = ["vf0000", "vf0100", "vf0200", "vf0300", "vf0400", "vf0500", "vf0600",
        "vf0401", "vf0501"]
def get(path, prop):
    r = subprocess.run(["fdtget", "-t", "u", dtb, path, prop],
        capture_output=True, text=True)
    return r.stdout.split() if r.returncode == 0 else None
def nodes(path):
    return subprocess.run(["fdtget", "-l", dtb, path], capture_output=True,
        text=True).stdout.split()
for table, label in (("cluster0-opp-table", "cpul_opp"),
        ("cluster1-opp-table", "cpub_opp"), ("dsu-opp-table", "dsu_opp")):
    rows = []
    for n in nodes("/" + table):
        hz = int(get("/%s/%s" % (table, n), "opp-hz")[1])
        volts = []
        for b in bins:
            v = get("/%s/%s" % (table, n), "opp-microvolt-26m-" + b)
            if v and int(v[0]):
                volts.append((b, int(v[0])))
        if volts:
            rows.append((hz, volts))
    rows.sort()
    print("\t%s: opp-table-%s {" % (label, label.split("_")[0]))
    print('\t\tcompatible = "allwinner,sun60i-a733-operating-points";')
    print("\t\tnvmem-cells = <&cpu_speed_grade>;")
    print('\t\tnvmem-cell-names = "speed";')
    print("\t\topp-shared;")
    for hz, volts in rows:
        print()
        print("\t\topp-%d {" % hz)
        print("\t\t\topp-hz = /bits/ 64 <%d>;" % hz)
        for b, v in volts:
            print("\t\t\topp-microvolt-%s = <%d>;" % (b, v))
        print("\t\t\tclock-latency-ns = <244144>;")
        print("\t\t};")
    print("\t};")
    print()
