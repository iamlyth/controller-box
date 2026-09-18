#!/bin/bash
LOG=/workspace/controller-box/.campaign-logs/monitor.log
: > $LOG
for i in $(seq 1 300); do
  TS=$(date +"%H:%M:%S")
  MEM=$(free -m | awk '/^Mem:/{print $3"MiB/"$2"MiB("100*$3/$2"% free="$4"MiB")}')
  echo "[] mem=$MEM" >> $LOG
  ps --ppid 4132536 -o pid,rss,etime,args 2>/dev/null >> $LOG
  if ! kill -0 4132536 2>/dev/null; then
    echo "[] CAMP DEAD" >> $LOG
    pgrep -af "factory-campaign|pi2|tau|mirror|bun" >> $LOG
    break
  fi
  sleep 30
done
echo done >> $LOG
