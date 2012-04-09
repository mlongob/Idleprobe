#!/bin/bash

while [ 1 ]
do
	FILENAME=$(date +"idleprobe-%s.cap")
	SIZE=0
	echo Writing $FILENAME:
	while [ $SIZE -lt 1048576 ]
	do
		cat /proc/idleprobe >> $FILENAME
		sleep 30
		SIZE=$( stat -c %s $FILENAME)
	done
	echo $SIZE bytes wrote to $FILENAME
done