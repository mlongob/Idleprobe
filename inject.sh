#!/bin/bash
make
OUT=$?
if [ $OUT -eq 0 ];then
	./commit.sh
	sudo rmmod idleprobe.ko >& /dev/null
	sudo insmod idleprobe.ko
fi
