#!/bin/sh

echo '#include "../../targetver.h"' > ./generated/translate.rc
echo '#include "windows.h"' >> ./generated/translate.rc

if [ -f ./source/zh_CN.po ]; then
    ./po2ymo.py ./source/zh_CN.po ./generated/zh_CN.ymo
    echo 'LANGUAGE LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED' >> ./generated/translate.rc
    echo '1 YMO "zh_CN.ymo"' >> ./generated/translate.rc
fi

if [ -f ./source/zh_TW.po ]; then
    ./po2ymo.py ./source/zh_TW.po ./generated/zh_TW.ymo
    echo 'LANGUAGE LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL' >> ./generated/translate.rc
    echo '1 YMO "zh_TW.ymo"' >> ./generated/translate.rc
fi
