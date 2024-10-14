#!/bin/sh

gcc -O3 -Wall -Werror -std=gnu11 arecordmidi-live.c -o arecordmidi-live -lasound

