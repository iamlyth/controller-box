#!/bin/bash
# Samples the campaign process tree + memory every 30s into monitor.log
LOG=.campaign-logs/monitor.log
: > $LOG
for i in $(seq 1 240); do
  TS=$(date +"%H:%M:%S")
  # total mem
  MEM=$(free -m | awk '/^Mem:/{print $3"MiB/"$2"MiB("100*$3/$2"%)"}')
  echo "[$TS] mem=$MEM" >> $LOG
  # campaign & children
  ps --ppid 4132536 -o pid,rss,etime,args 2>/dev/null | grep -vE "bash -c|nix-shell" | head -15 >> $LOG
  if ! kill -0 4132536 2>/dev/null; then
    echo "[$TS] CAMPAIGN 4132536 NO LONGER RUNNING" >> $LOG
    # capture remaining children
    pgrep -af "factory-campaign|pi2|tau|mirror" >> $LOG
    break
  fi
  sleep 30
done
echo "monitor done" >> $LOG
