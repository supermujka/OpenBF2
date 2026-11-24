# Build /tmp/ab_VANILLA.csv and /tmp/ab_OPTIMIZED.csv from ab_phases.log marks.
H=/home/bf2/hitreg_logs; PH=/home/bf2/server/ab_phases.log
mapfile -t M < <(sudo -n cat $PH)
for i in 0 1; do
  IFS='|' read -r lab t f s <<< "${M[$i]}"; IFS='|' read -r lab2 t2 f2 e <<< "${M[$((i+1))]}"
  { sudo -n head -1 $H/$f
    if [ "$f" = "$f2" ]; then sudo -n sed -n "$((s+1)),${e}p" $H/$f
    else sudo -n tail -n +$((s+1)) $H/$f; sudo -n sed -n "2,${e}p" $H/$f2; fi; } > /tmp/ab_$lab.csv
  chmod 644 /tmp/ab_$lab.csv; echo "$lab $t-$t2 rows $(wc -l < /tmp/ab_$lab.csv)"
done
