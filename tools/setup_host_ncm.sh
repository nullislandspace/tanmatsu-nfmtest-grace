#!/usr/bin/env bash
# Give the PC 192.168.77.1/24 on the badge's USB network interface, for
# when the badge's own DHCP does not do it (a PC with no DHCP client on
# new interfaces). Usually not needed: NetworkManager's automatic "Wired
# connection" asks the badge by DHCP and gets exactly this address, with
# no route and no DNS.
#
#   tools/setup_host_ncm.sh <mac>        NetworkManager profile for that
#                                        badge (once; no sudo afterwards)
#   tools/setup_host_ncm.sh --now [mac]  just set the address on the
#                                        interface that is there now (sudo,
#                                        every time the badge reconnects)
#
# <mac> is the PC-side MAC the badge shows on its screen (the interface
# is enx<mac without colons>).
set -euo pipefail

HOST_IP=192.168.77.1/24

find_if() {
    local mac="${1:-}"
    for d in /sys/class/net/*; do
        if [[ -n "$mac" ]]; then
            [[ "$(cat "$d/address")" == "${mac,,}" ]] && { basename "$d"; return; }
        elif [[ "$(basename "$(readlink -f "$d/device/driver" 2>/dev/null)")" == cdc_ncm ]]; then
            basename "$d"; return
        fi
    done
}

if [[ "${1:-}" == "--now" ]]; then
    ifname="$(find_if "${2:-}")"
    [[ -n "$ifname" ]] || { echo "no badge interface found (is it in network mode?)" >&2; exit 1; }
    sudo ip link set "$ifname" up
    sudo ip addr replace "$HOST_IP" dev "$ifname"
    ip -4 addr show dev "$ifname"
    exit 0
fi

mac="${1:?usage: $0 <mac> | --now [mac]}"
command -v nmcli >/dev/null || { echo "no nmcli here: use $0 --now" >&2; exit 1; }
name="nfmtest-${mac//:/}"
nmcli connection delete "$name" >/dev/null 2>&1 || true
nmcli connection add type ethernet con-name "$name" ifname '*' \
    802-3-ethernet.mac-address "$mac" \
    ipv4.method manual ipv4.addresses "$HOST_IP" ipv4.never-default yes \
    ipv6.method disabled connection.autoconnect yes connection.autoconnect-priority 50
echo "profile $name: $HOST_IP on the interface with MAC $mac, no default route"
