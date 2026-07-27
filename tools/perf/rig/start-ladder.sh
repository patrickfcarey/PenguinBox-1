#!/usr/bin/env bash
setsid bash "$HOME/ladder.sh" > "$HOME/ladder-mission.txt" 2>&1 < /dev/null &
echo "detached $!"
