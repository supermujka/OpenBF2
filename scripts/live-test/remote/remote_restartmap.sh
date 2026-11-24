D=/home/bf2/server
before=$(sudo -n ls -t /home/bf2/hitreg_logs/ | head -1)
sudo -n -u bf2 sh -c "printf 'admin.restartMap\n# %s\n' \"\$(date +%T)\" > $D/live_cvars.txt"
sleep 4; sudo -n tail -1 $D/live_cvars.log
for i in $(seq 1 60); do now=$(sudo -n ls -t /home/bf2/hitreg_logs/ | head -1); [ "$now" != "$before" ] && break; sleep 2; done
echo "new csv: $now"
