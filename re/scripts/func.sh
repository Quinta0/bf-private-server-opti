#!/bin/bash
W=$1; P=$2
awk -v p="$P" '/^[0-9a-f]+ </{on=(index($0,p)>0)} on' $W/full.s | c++filt
