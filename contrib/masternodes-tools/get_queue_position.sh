#!/bin/bash

echo "****************************************************************************"
echo "*           Omagecoincore - get_queue_position Linux VPS script.           *"
echo "*           By @Mr_F#6171 on Official Omegacoin Discord server.            *"
echo "****************************************************************************"
echo && echo && echo

MNLISTCMD="omegacoin-cli masternodelist full 2>/dev/null"

MNADDR=$1

if [ -z $MNADDR ]; then
    echo "usage: $0 <masternode address>"
    exit -1
fi

function _cache_command(){

    # cache life in minutes
    AGE=2

    FILE=$1
    AGE=$2
    CMD=$3

    OLD=0
    CONTENTS=""
    if [ -e $FILE ]; then
        OLD=$(find $FILE -mmin +$AGE -ls | wc -l)
        CONTENTS=$(cat $FILE);
    fi
    if [ -z "$CONTENTS" ] || [ "$OLD" -gt 0 ]; then
        echo "REBUILD"
        CONTENTS=$(eval $CMD)
        echo "$CONTENTS" > $FILE
    fi
    echo "$CONTENTS"
}



MN_LIST=$(_cache_command /tmp/cached_mnlistfull 2 "$MNLISTCMD")
SORTED_MN_LIST=$(echo "$MN_LIST" | grep -w ENABLED | sed -e 's/[}|{]//' -e 's/"//g' -e 's/,//g' | grep -v ^$ | \
awk ' \
{
    if ($7 == 0) {
        TIME = $6
        print $_ " " TIME
    }
    else {
        xxx = ("'$NOW'" - $7)
        if ( xxx >= $6) {
            TIME = $6
        }
        else {
            TIME = xxx
        }
        print $_ " " TIME
    }
}' |  sort -k10 -n)

MN_VISIBLE=$(echo "$SORTED_MN_LIST" | grep $MNADDR | wc -l)
MN_QUEUE_LENGTH=$(echo "$SORTED_MN_LIST" | wc -l)
MN_QUEUE_POSITION=$(echo "$SORTED_MN_LIST" | grep -A9999999 $MNADDR | wc -l)
MN_QUEUE_IN_SELECTION=$(( $MN_QUEUE_POSITION <= $(( $MN_QUEUE_LENGTH / 10 )) ))

echo "masternode $MNADDR"
if [ $MN_VISIBLE -gt 0 ]; then
    echo " -> queue position $MN_QUEUE_POSITION/$MN_QUEUE_LENGTH"
    if [ $MN_QUEUE_IN_SELECTION -gt 0 ]; then
        echo " -> SELECTION PENDING"
    fi
else
    echo "is not in masternode list"
fi
