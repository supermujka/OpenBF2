# Overnight 31v31 loadbot soak with watchdog. Runs in the foreground of one wsl.exe so the
# WSL VM stays up; restarts the server/bots if they die and logs status every 5 minutes.
HOURS=${HOURS:-8}
NAVMESH=${NAVMESH:?set NAVMESH to the Karkand navmesh dir (Infantry.qtr + AerialHeighMap.ahm)}
SRV=~/bf2server; LOG=$SRV/soak_status.log; CR=$SRV/soak_crashes.log
END=$(( $(date +%s) + HOURS*3600 ))
SETTINGS=(
 "vars.set phy-latencyComp-improved 1" "vars.set phy-latencyComp-queueDelay 1"
 "vars.set phy-latencyComp-pingFactor 1.0" "vars.set phy-latencyComp-maxClientInterpMs 133"
 "vars.set phy-latencyComp-queueDrainMaxTarget 3" "vars.set net-serverSendRate 30"
 "vars.set net-fast-statemasks 0" "vars.set net-maxPacketBytes 0" "vars.set net-maxGhostsPerPacket 0"
 "vars.set net-scopeBudgetPerFrame 0" "vars.set net-scopeDistance 0" "vars.set net-scopeHysteresis 0"
 "vars.set net-resendPriorityBoost 0" "vars.set sv-maxCatchUpTicks 0" "vars.set net-mt-enable 0"
 "vars.set net-mt-verify 0" "vars.set net-batch-send 0" "vars.set sv-perf-profile 1"
 "sv.soldierFriendlyFire 0" "sv.vehicleFriendlyFire 0" "sv.soldierSplashFriendlyFire 0"
 "sv.vehicleSplashFriendlyFire 0" "sv.tkPunishEnabled 0")
apply_settings(){ printf '%s\n' "${SETTINGS[@]}" "# applied $(date +%T)" > $SRV/live_cvars.txt; }
start_server(){
  cd $SRV; : > live_cvars.txt
  [ -f screen_bf2.log ] && mv -f screen_bf2.log "screen_bf2_$(date +%H%M%S).log"
  screen -dmS bf2 -L -Logfile $SRV/screen_bf2.log ./start.sh +modPath mods/bf2 +config mods/bf2/settings/karkand_inf64.con +mapList mods/bf2/settings/karkand_maplist.con +maxPlayers 64 +noStatusMonitor 1
  for i in $(seq 1 120); do ss -ulpn | grep -q ':16567 ' && grep -q 'SV-CPU' screen_bf2.log && break; sleep 1; done
  sleep 3; apply_settings
}
start_bots(){
  screen -S bots -X quit 2>/dev/null; sleep 2
  [ -f ~/bots.log ] && mv -f ~/bots.log ~/bots_$(date +%H%M%S).log
  screen -dmS bots -L -Logfile ~/bots.log ${LOADBOT:-~/bf2_loadbot_aim} --host 127.0.0.1 --port 16567 --bots 62 \
    --ping-min-ms 0 --ping-max-ms 30 --high-ping-pct 10 --high-ping-min-ms 30 --high-ping-max-ms 70 \
    --jitter-ms 4 --loss-pct 0.5 --duration-sec 0 --world-size 1024 \
    --navmesh "$NAVMESH" --seed 4242 --report-sec 30
}
echo "=== soak start $(date '+%F %T'), ${HOURS}h ===" >> $LOG
pgrep -f 'amd-64/bf2' >/dev/null || start_server
apply_settings; sleep 3
start_bots
restarts=0; last=0
while [ $(date +%s) -lt $END ]; do
  if ! pgrep -f 'amd-64/bf2' >/dev/null; then
    { echo "=== SERVER DOWN $(date '+%F %T') ==="; tail -40 $SRV/screen_bf2.log; ls -t /var/crash 2>/dev/null | head -3; } >> $CR
    restarts=$((restarts+1)); screen -S bots -X quit 2>/dev/null; start_server; start_bots
  elif ! pgrep -f bf2_loadbot >/dev/null; then
    echo "=== BOTS DOWN $(date '+%F %T') ===" >> $CR; tail -20 ~/bots.log >> $CR; start_bots
  fi
  now=$(date +%s)
  if [ $((now-last)) -ge 300 ]; then
    last=$now
    P=$(pgrep -f 'amd-64/bf2' | head -1)
    rss=$(ps -o rss= -p "$P" 2>/dev/null | awk '{printf "%.0f",$1/1024}')
    perf=$(grep 'SV-PERF\] ticks' $SRV/screen_bf2.log | tail -1 | grep -oE 'avg_conns=[0-9.]+|overruns_33ms=[0-9]+|dropped_steps=[0-9]+')
    us=$(grep 'SV-PERF\] us' $SRV/screen_bf2.log | tail -1 | grep -oE 'total_us=[0-9/]+')
    cpu=$(grep 'SV-CPU' $SRV/screen_bf2.log | tail -1 | grep -oE 'proc_cpu_pct=[0-9.]+|main_thread_cpu_pct=[0-9.]+')
    bots=$(grep 'Bots   :' ~/bots.log | tail -1 | grep -oE 'conn=[0-9]+|alive=[0-9]+|disc=[0-9]+ \(tot [0-9]+\)')
    csvs=$(ls $SRV/openbf2_hitreg_*.csv | wc -l); csvmb=$(du -cm $SRV/openbf2_hitreg_*.csv | tail -1 | cut -f1)
    echo "$(date +%T) restarts=$restarts rss_mb=$rss $(echo $perf $us $cpu | tr '\n' ' ') bots[$(echo $bots | tr '\n' ' ')] csv=$csvs/${csvmb}MB disk_free=$(df -h ~ | tail -1 | awk '{print $4}')" >> $LOG
  fi
  sleep 60
done
echo "=== soak end $(date '+%F %T') restarts=$restarts ===" >> $LOG
tail -5 $LOG
