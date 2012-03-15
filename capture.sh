#!/bin/bash

FILENAME=$(date +"idleprobe-%s.cap")
echo $FILENAME
while [ 1 ]
do
	cat /proc/idleprobe >> $FILENAME
	sleep 10
done