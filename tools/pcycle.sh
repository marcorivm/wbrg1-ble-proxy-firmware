#!/bin/sh
# Power-cycle the WBRG1 gateway via its smart plug (Plug R1) using the HA REST API.
#
# Needs HA_URL + HA_TOKEN in the environment. Either export them, or point
# HA_ENV at a file that defines them (never commit that file):
#   HA_ENV=~/.ha.env sh pcycle.sh
# SW is the smart plug the gateway is plugged into -- override per setup.
set -e
[ -n "$HA_ENV" ] && [ -f "$HA_ENV" ] && . "$HA_ENV"
: "${HA_URL:?set HA_URL (or HA_ENV pointing at a file that defines it)}"
: "${HA_TOKEN:?set HA_TOKEN (or HA_ENV pointing at a file that defines it)}"
U="${HA_URL%/}"
SW="${WBRG1_PLUG:-switch.luces_porton_socket_1}"
h(){ curl -s --max-time 15 -H "Authorization: Bearer $HA_TOKEN" -H "Content-Type: application/json" "$@"; }
echo "power OFF $SW"; h -X POST "$U/api/services/switch/turn_off" -d "{\"entity_id\":\"$SW\"}" >/dev/null
sleep 5
echo "power ON $SW";  h -X POST "$U/api/services/switch/turn_on"  -d "{\"entity_id\":\"$SW\"}" >/dev/null
echo "waiting for gateway to boot..."
