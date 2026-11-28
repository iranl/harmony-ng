#!/bin/sh
# /cache/kernel6/udhcpc.sh: lease script for BusyBox udhcpc (address, default route, DNS).
case "$1" in
    deconfig)
        ifconfig "$interface" 0.0.0.0
        ;;
    bound|renew)
        ifconfig "$interface" "$ip" netmask "${subnet:-255.255.255.0}" up
        if [ -n "$router" ]; then
            while route del default dev "$interface" 2>/dev/null; do :; done
            for r in $router; do
                route add default gw "$r" dev "$interface"
            done
        fi
        # /etc/resolv.conf is on flash (the stock writable layer): rewrite it only on a change
        if [ -n "$dns" ]; then
            : > /tmp/resolv.conf
            for d in $dns; do
                echo "nameserver $d" >> /tmp/resolv.conf
            done
            cmp -s /tmp/resolv.conf /etc/resolv.conf || cp /tmp/resolv.conf /etc/resolv.conf
        fi
        echo "udhcpc: $interface $ip"
        ;;
esac
exit 0
