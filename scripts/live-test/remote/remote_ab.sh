# Map restart, 2 min vanilla, 2 min all optimizations; marks to /home/bf2/server/ab_phases.log
D=/home/bf2/server; H=/home/bf2/hitreg_logs; PH=$D/ab_phases.log
say(){ { printf 'game.sayAll "[Claude] %s"\n' "$1"; shift; for l in "$@"; do printf '%s\n' "$l"; done; printf '# %s\n' "$(date +%T.%N)"; } > /tmp/ab.txt; sudo -n install -o bf2 -g bf2 -m 644 /tmp/ab.txt $D/live_cvars.txt; sleep 3; }
mark(){ F=$(sudo -n ls -t $H | head -1); echo "$1|$(date +%T)|$F|$(sudo -n wc -l < $H/$F)" | sudo -n -u bf2 tee -a $PH; }
sudo -n -u bf2 sh -c ": > $PH"
before=$(sudo -n ls -t $H | head -1)
say "Restart mape. Prve 2 minute VANILLA (bez optimizacija)." 'vars.set phy-latencyComp-improved 0' 'vars.set net-serverSendRate 0' 'admin.restartMap'
for i in $(seq 1 60); do now=$(sudo -n ls -t $H | head -1); [ "$now" != "$before" ] && break; sleep 2; done
sleep 5; mark VANILLA
sleep 120
say "Sada 2 minute SA SVIM OPTIMIZACIJAMA (hitreg-v1, 30 Hz, interp 450)." 'vars.set phy-latencyComp-improved 1' 'vars.set net-serverSendRate 30' 'vars.set phy-latencyComp-maxClientInterpMs 450' 'vars.set phy-latencyComp-queueDrainMaxTarget 4' 'vars.set phy-latencyComp-queueDelay 1'
mark OPTIMIZED
sleep 120
mark END
say "Test gotov. Ostaju optimizacije ukljucene."
sudo -n cat $PH
