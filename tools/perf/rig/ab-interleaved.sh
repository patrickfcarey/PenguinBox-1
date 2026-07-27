#!/usr/bin/env bash
# Interleaved ABABA A/B for the narrow-fence toggle. Assumes the emulator is
# ALREADY in the probe scene, logging to ~/ring-smoke.log. Arms are delimited
# in the log by the "narrowfence: async early-submit ON/OFF" state lines.
LOG="$HOME/ring-smoke.log"
arm() {  # $1 = on|off, $2 = seconds
    if [ "$1" = on ]; then touch /tmp/narrowfence-on; else rm -f /tmp/narrowfence-on; fi
    sleep "$2"
}
# Force a marker pair so per-segment keys start fresh (excludes boot lines)
arm on 2
# ABABA, 15 s per arm
arm off 15
arm on  15
arm off 15
arm on  15
arm off 15
echo "== per-arm summary (mean mspf | drains | submits | dl-waits | trapped-reads | n) =="
awk '
/narrowfence: async early-submit/ { state=$NF; seg++; key=seg ":" state; next }
/nv2a-prof:/ {
    if (key=="") next
    state=key
    for (i=1;i<=NF;i++) {
        split($i,kv,"=")
        if (kv[1]=="mspf")               m[state]+=kv[2]
        if (kv[1]=="FINISH_SURFACE_DOWN") fsd[state]+=kv[2]
        if (kv[1]=="QUEUE_SUBMIT")        qs[state]+=kv[2]
        if (kv[1]=="SURF_CPU_DL_WAIT")    dw[state]+=kv[2]
        if (kv[1]=="SURF_CPU_READ")       rd[state]+=kv[2]
        if (kv[1]=="SURF_PREDL")          pr[state]+=kv[2]
    }
    n[state]++
}
END {
    for (s in n) if (n[s]>0)
        printf "%-8s mspf=%.1f fsd=%.1f qs=%.1f dlw=%.1f reads=%.0f predl=%.1f n=%d\n",
            s, m[s]/n[s], fsd[s]/n[s], qs[s]/n[s], dw[s]/n[s], rd[s]/n[s], pr[s]/n[s], n[s]
}' "$LOG" | sort -t: -k1 -n
rm -f /tmp/narrowfence-on
