#!/bin/bash
cmake -S . -B build -DEVMONE_TESTING=ON
cmake --build build -j16

TEST_LIST_FILE="./UnittestsRunList.txt"
FILTER_PARAM=""

while IFS= read -r line || [[ -n "$line" ]]; do
    line_clean=$(echo "$line" | xargs)
    if [ -n "$line_clean" ]; then
        if [ -z "$FILTER_PARAM" ]; then
            FILTER_PARAM="$line_clean"
        else
            FILTER_PARAM="$FILTER_PARAM:$line_clean"
        fi
    fi
done < "$TEST_LIST_FILE"

export LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libasan.so.6
./build/bin/evmone-unittests --gtest_filter="$FILTER_PARAM"