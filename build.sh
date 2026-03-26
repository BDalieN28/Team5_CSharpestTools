#!/bin/bash
set -e
clang -O2 -std=c11 -Wall -Wextra -o engine src/engine.c
echo "Built ./engine"
