#!/bin/bash

while [ 1 ]
do
	FILENAME=$(date +"idleprobe-%s.cap")
	SIZE=0
	while [ $SIZE -lt 1048576 ]
	do
		cat /proc/idleprobe >> $FILENAME
		sleep 1
		SIZE=$( stat -c %s $FILENAME)
	done
	echo $SIZE bytes wrote to $FILENAME
done