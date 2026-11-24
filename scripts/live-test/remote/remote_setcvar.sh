D=/home/bf2/server
F=$(sudo -n ls -t /home/bf2/hitreg_logs/ | head -1)
L=$(sudo -n wc -l < /home/bf2/hitreg_logs/$F)
sudo -n -u bf2 sh -c "printf '%s\n# %s\n' \"$1\" \"\$(date +%T)\" > $D/live_cvars.txt"
sleep 4; sudo -n tail -1 $D/live_cvars.log
echo "$(date +%T)|$1|$F|$L" | sudo -n -u bf2 tee -a $D/live_changes.log
