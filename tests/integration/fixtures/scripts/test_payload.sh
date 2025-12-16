#!/bin/sh
method="$1"
encoding="$2"
payload="$(cat)"

case "$method" in
  uppercase)
    printf "%s:%s" "$encoding" "$(printf "%s" "$payload" | tr '[:lower:]' '[:upper:]')"
    ;;
  reject)
    echo "REJECT:$encoding" >&2
    exit 2
    ;;
  repeat)
    printf "%s:%s:%s" "$encoding" "$payload" "$payload"
    ;;
  empty)
    # Explicitly return an empty payload body
    ;;
  *)
    printf "ECHO:%s:%s" "$encoding" "$payload"
    ;;
 esac
