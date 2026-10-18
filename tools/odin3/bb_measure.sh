#!/bin/zsh
# bb_measure.sh <outdir> [seconds] : mesure le jeu BachataS4 en cours (paquet PKG, défaut com.bachatas4.android.odin3).
# Images/s et régularité depuis les présentations SurfaceFlinger (fps.py de Cemu), plus GPU, fréquences, températures
# et puissance d'entrée USB (poll.sh de xemu, une ligne par seconde). À lancer sous le verrou ps4. Jamais de capture
# d'écran pendant la mesure : chaque screencap fige SurfaceFlinger.
A=~/Library/Android/sdk/platform-tools/adb
OUT=$1; DUR=${2:-60}; PKG=${PKG:-com.bachatas4.android.odin3}
mkdir -p $OUT
# Older builds list bare names, newer wrap them in RequestedLayerState{…}; the BLAST child receives the frames.
L=$($A shell "dumpsys SurfaceFlinger --list" | tr -d '\r' | grep -v Background | grep -oE "SurfaceView\[$PKG/[^]]*\](\(BLAST\))?(#[0-9]+)?" | sort -r | head -1)
[ -z "$L" ] && { echo "pas de couche SurfaceView pour $PKG"; exit 1; }
echo "layer=$L start=$(date +%T)" > $OUT/context.txt
$A push ~/PhpstormProjects/xemu/tools/odin3/poll.sh /data/local/tmp/bb_poll.sh >/dev/null
$A shell "sh /data/local/tmp/bb_poll.sh $DUR" > $OUT/poll.csv &
POLL=$!
: > $OUT/sf_latency.txt
t0=$SECONDS
while [ $((SECONDS - t0)) -lt $DUR ]; do
  $A shell "dumpsys SurfaceFlinger --latency '$L'" >> $OUT/sf_latency.txt
  sleep 1
done
wait $POLL
python3 -I ~/PhpstormProjects/cemu/tools/odin3/fps.py $OUT/sf_latency.txt | tee $OUT/summary.txt
python3 -I - $OUT/poll.csv <<'EOF' | tee -a $OUT/summary.txt
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
if rows:
    avg = lambda k: sum(float(r[k]) for r in rows) / len(rows)
    usb_w = sum(int(r['usb_ma']) * int(r['usb_mv']) for r in rows) / len(rows) / 1e6
    bat_w = sum(int(r['batt_ma']) * int(r['batt_mv']) for r in rows) / len(rows) / 1e6
    print(f"gpu_busy={avg('gpu_busy'):.0f}% gpu_mhz={avg('gpu_mhz'):.0f} c0_mhz={avg('c0_mhz'):.0f} c6_mhz={avg('c6_mhz'):.0f} "
          f"cpu_tmax={max(int(r['cpu_tmax']) for r in rows) / 1000:.0f}C gpu_tmax={max(int(r['gpu_tmax']) for r in rows) / 1000:.0f}C "
          f"usb_in={usb_w:.1f}W batt={bat_w:+.1f}W")
EOF
