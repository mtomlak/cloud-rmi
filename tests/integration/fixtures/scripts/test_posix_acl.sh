#!/bin/bash
method="$1"
shift

usage() {
  echo "Usage: $(basename "$0") setPosixUserAccessMode [uid] [mode] [path] <flags>"
  echo "       $(basename "$0") setPosixGroupAccessMode [gid] [mode] [path] <flags>"
  exit 1
}

case "$method" in
  setPosixUserAccessMode)
    if [ "$#" -lt 3 ]; then
      usage
    fi
    user="$1"
    mode="$2"
    path="$3"
    flags="${4:-}"
    echo "USER:${user}:${mode}:${path}:${flags}"
    ;;
  setPosixGroupAccessMode)
    if [ "$#" -lt 3 ]; then
      usage
    fi
    group="$1"
    mode="$2"
    path="$3"
    flags="${4:-}"
    echo "GROUP:${group}:${mode}:${path}:${flags}"
    ;;
  *)
    usage
    ;;
esac
