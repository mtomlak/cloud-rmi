#!/bin/sh
method="$1"
args="$2"

case "$method" in
  fail)
    echo "FAIL:$args"
    exit 42
    ;;
  stderr)
    echo "STDERR:$args" >&2
    exit 0
    ;;
  large)
    count="$args"
    if [ -z "$count" ]; then
      count=0
    fi
    head -c "$count" /dev/zero | tr '\0' 'L'
    exit 0
    ;;
  *)
    echo "ERROR:unknown method $method"
    exit 1
    ;;
 esac
