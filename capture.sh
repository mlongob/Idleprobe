#!/bin/bash

while [ 1 ]
do
	FILENAME=$(date +"idleprobe-%s.cap")
	echo $FILENAME
	SIZE=0
	echo $SIZE
	while [ $SIZE -lt 1048576 ]
	do
		cat /proc/idleprobe >> $FILENAME
		sleep 1
		SIZE=$( stat -c %s $FILENAME)
		echo $SIZE
	done
done