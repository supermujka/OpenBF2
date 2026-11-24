# usage: extract_since.sh <csv file> <line>  -> rows after <line> in that file plus all newer files
cd /home/bf2/hitreg_logs || exit 1
F="$1"; L="$2"
{ sudo -n head -1 "$F"; sudo -n tail -n +$((L+1)) "$F"
  for f in $(sudo -n ls -tr openbf2_hitreg_*.csv); do [ "$f" \> "$F" ] && sudo -n tail -n +2 "$f"; done; } > /tmp/since.csv
chmod 644 /tmp/since.csv; wc -l /tmp/since.csv
