#!/bin/bash
# usage: callers.sh W pattern  -> prints containing function for each bl matching pattern
W=$1; P=$2
awk -v p="$P" '/^[0-9a-f]+ </{f=$2} $0 ~ p && /\tbl\t|\tb\t/{print f}' $W/full.s | sort | uniq -c | c++filt
