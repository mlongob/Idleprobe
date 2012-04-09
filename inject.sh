#!/bin/bash
make
OUT=$?
if [ $OUT -eq 0 ];then
	./commit.sh
	sudo rmmod idleprobe.ko
	sudo insmod idleprobe.ko
fi