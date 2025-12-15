#!/bin/sh
method="$1"
args="$2"
if [ "$method" = "echo" ]; then
  printf "TEST_ECHO_RESPONSE:%s\n" "$args"
else
  printf "ERROR:unsupported method %s\n" "$method"
  exit 1
fi
