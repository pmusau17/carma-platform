#!/bin/bash

while true
    do
        excluded_topics="$(rosparam get /exclude_regex)"
        if [ ! -z "${excluded_topics}" ]
        then 
            rosbag record -o /opt/carma/logs/ --lz4 -a -x '/rosout(.*)|(.*)/received_messages|(.*)/sent_messages|(.*)/scan|$($excluded_topics)'
            exit 0
        fi
    done