#!/bin/bash
if [ ! -r /proc/idleprobe ]
then
	echo "Error: the idleprobe module is not running."
	exit 1
fi
while [ 1 ]
do
	FILENAME=$(date +"idleprobe-%s.cap")
	SIZE=0
	echo Writing $FILENAME:
	while [ $SIZE -lt 1048576 ]
	do
		if [ ! -r /proc/idleprobe ]
		then
			echo "Error: the idleprobe module is not running."
			exit 1
		fi
		cat /proc/idleprobe >> $FILENAME
		sleep 30
		SIZE=$( stat -c %s $FILENAME)
	done
	echo $SIZE bytes wrote to $FILENAME
done