#!/bin/sh
# usbppp.sh - the computer's end of the calculator's PPP connection over USB.
#
#   tools/usbppp.sh <serial device>
#
# <serial device> is the calculator's CDC ACM port (macOS: /dev/cu.usbmodem*,
# Linux: /dev/ttyACM*), or, with casio-emu, the pty its host bridge prints
# ("[USB] host: CDC ACM bridge on /dev/ttys..."). Start this, then run
# "USB network" (classpad_usbnet.exe) on the calculator.
#
# Windows CE's direct connection opens with the text CLIENT and waits for
# CLIENTSERVER before it starts PPP; the chat script answers that. The
# calculator gets 192.168.55.2, this side is 192.168.55.1. No authentication
# (the entry on the calculator has no user name). pppd needs root; macOS
# ships pppd and chat in /usr/sbin, nothing to install (chat is named by its
# full path: pppd runs the connect script without /usr/sbin in the PATH).
# "can't set controlling terminal" in pppd's log is harmless.
#
# The name server handed to the calculator is USBPPP_DNS (default 1.1.1.1).
#
# This gives the two ends a link and nothing more: for the calculator to
# reach anything beyond this computer, turn on forwarding and NAT here
# (macOS, with en0 the interface that has the network):
#     sudo sysctl -w net.inet.ip.forwarding=1
#     echo "nat on en0 from 192.168.55.0/24 to any -> (en0)" | sudo pfctl -ef -
set -e
dev="${1:?usage: usbppp.sh <serial device>}"
dns="${USBPPP_DNS:-1.1.1.1}"
exec sudo pppd "$dev" 115200 local noauth nocrtscts nodetach debug \
    192.168.55.1:192.168.55.2 ms-dns "$dns" \
    connect "/usr/sbin/chat -v -t 60 CLIENT CLIENTSERVER"
