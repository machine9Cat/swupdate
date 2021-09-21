#!/bin/sh
# this file version
SWIV="V1.000"
#you creat same used indent in here
#
CFG_FILE="/run/swuid.cfg"
# add swver hwver appver
HW_FILE="/etc/hwrevision"

#del the swuid.cfg file 
if [ -f $CFG_FILE ]; then
    rm -f -f $CFG_FILE
fi

echo "" > $CFG_FILE

#
#参数1是name ，参数2是value ，参数3是path ,参数4 ==1 表示行结尾加‘,’
function gen_an_new_line() {
    if [ $4 == '1' ];then
        NL=$( printf "{ name = \"%s\"; value = \"%s\" }," "$1" "$2" );
    else
        NL=$( printf "{ name = \"%s\"; value = \"%s\" }" "$1" "$2" );
    fi
    # echo $NL
    sed -i "\$a$NL" $3
}

# hw fs sw ver
if [ -f "$HW_FILE" ]; then
    TMP=$(head -n1 $HW_FILE) 
    gen_an_new_line "HW" "$TMP" "$CFG_FILE" '1'
fi

# add nat para
NET="eth0 br0 wlan0 wwan0"

for i in  ${NET};do
    #MAC
    TMP=$(ifconfig "$i" | grep 'HWaddr' | awk -F' ' '{printf $5}')
    if [ ! "$TMP" ];then
        TMP="none"
    fi
    
    gen_an_new_line "$i""_MAC" "$TMP" "$CFG_FILE" '1'
    #IP
    TMP=$(ifconfig "$i" | grep 'inet' | awk -F ' ' '{printf $2}' | awk -F: '{printf $2}')

    if [ ! "$TMP" ];then
        TMP="none"
    fi
    
    gen_an_new_line "$i""_IP" "$TMP" "$CFG_FILE" '1'
done

# sysload 5min 10min 15min
eval $(cat /proc/loadavg | awk -F' ' '{printf("a=%s; b=%s; c=%s;",$1,$2,$3)}')
TMP=$a-$b-$c
gen_an_new_line "load" "$TMP" "$CFG_FILE" '1'
# uptime
TMP=$(/usr/bin/uptime | awk -F, '{printf $1}')
gen_an_new_line "uptime" "$TMP" "$CFG_FILE" '1'

#endif for file
gen_an_new_line "swiv" "-" "$SWIV" '0'
