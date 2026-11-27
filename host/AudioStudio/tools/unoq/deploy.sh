#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Copies audio_stream.py to ~/audiostudio-unoq/ on the UNO Q, where the
# stream command of targets/unoq.json runs it:
#
#   flock -n -F -E 75 /home/arduino python3 -B ~/audiostudio-unoq/audio_stream.py
#
# Usage: tools/unoq/deploy.sh [user@host]   (default arduino@unoq.local)
#
# ssh and scp run with BatchMode=yes: key authentication must be set up.
set -eu

target=${1:-arduino@unoq.local}
here=$(cd "$(dirname "$0")" && pwd)

ssh -o BatchMode=yes "$target" 'mkdir -p ~/audiostudio-unoq'
scp -o BatchMode=yes -q "$here/audio_stream.py" "$target:audiostudio-unoq/audio_stream.py"
echo "Copied audio_stream.py to $target:~/audiostudio-unoq/"
