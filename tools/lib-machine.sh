#!/bin/bash
# tools/lib-machine.sh — one line that says WHICH MACHINE ran this.
#
# Why it exists: two runs of the same boot can differ by 10x on wall time and the
# log does not say whether that was KVM or TCG, an idle runner or one with four
# jobs on it, 4 vCPU or 32. On 2026-11-25 an arch install sat for 140 minutes and
# a gentoo mount took longer than its 120 s budget; both logs recorded the
# symptom and neither recorded the machine, so "slow guest" and "wedged guest"
# could not be told apart from the log alone.
#
# Usage:  . tools/lib-machine.sh ; machine_host_line
machine_host_line() {
    local cpu ncpu mem kvm load
    cpu=$(awk -F': ' '/^model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null)
    [ -n "$cpu" ] || cpu=$(awk -F': ' '/^Hardware|^Processor/{print $2; exit}' /proc/cpuinfo 2>/dev/null)
    [ -n "$cpu" ] || cpu="unknown CPU"
    ncpu=$(nproc 2>/dev/null || echo '?')
    mem=$(awk '/^MemTotal/{printf "%.1f", $2/1048576}' /proc/meminfo 2>/dev/null)
    kvm=no
    { [ -r /dev/kvm ] && [ -w /dev/kvm ]; } && kvm=yes
    load=$(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null)
    printf 'host: %s x%s, %s GiB RAM, /dev/kvm=%s, load=%s, kernel %s\n' \
        "$cpu" "$ncpu" "${mem:-?}" "$kvm" "${load:-?}" "$(uname -r 2>/dev/null)"
}
