# usage: remote_say.sh "message" [extra console line ...]
D=/home/bf2/server; MSG="$1"; shift
{ printf 'game.sayAll "[Claude] %s"\n' "$MSG"; for l in "$@"; do printf '%s\n' "$l"; done; printf '# %s\n' "$(date +%T.%N)"; } > /tmp/say.txt
sudo -n install -o bf2 -g bf2 -m 644 /tmp/say.txt $D/live_cvars.txt
sleep 3; sudo -n tail -$(( $# + 1 )) $D/live_cvars.log
