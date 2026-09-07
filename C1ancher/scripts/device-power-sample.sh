#!/bin/sh
set -u

usage() {
    echo "usage: $0 [duration_seconds [interval_seconds]]" >&2
    exit 64
}

case "${1:-60}" in
    ''|*[!0-9]*) usage ;;
esac
case "${2:-1}" in
    ''|*[!0-9]*) usage ;;
esac

c1_duration=${1:-60}
c1_interval=${2:-1}
[ "$c1_interval" -gt 0 ] || usage
[ "$#" -le 2 ] || usage

c1_sanitize() {
    printf '%s' "$1" | tr '\011\015\012;|' '_____'
}

c1_read_value() {
    c1_read_path=$1
    if [ ! -r "$c1_read_path" ]; then
        printf '%s' unavailable
        return
    fi
    c1_read_result=$(head -n 1 "$c1_read_path" 2>/dev/null)
    if [ -n "$c1_read_result" ]; then
        c1_sanitize "$c1_read_result"
    else
        printf '%s' unavailable
    fi
}

c1_emit_item() {
    if [ "$c1_emit_first" -eq 1 ]; then
        c1_emit_first=0
    else
        printf ';'
    fi
    printf '%s' "$1"
}

c1_collect_power_supply() {
    c1_emit_first=1
    c1_supply_found=0
    for c1_supply_dir in /sys/class/power_supply/*; do
        [ -d "$c1_supply_dir" ] || continue
        c1_supply_found=1
        c1_supply_name=${c1_supply_dir##*/}
        for c1_supply_attr in type status present online capacity capacity_level health technology voltage_now voltage_avg voltage_min voltage_max voltage_min_design voltage_max_design current_now current_avg current_max charge_now charge_full charge_full_design energy_now energy_full energy_full_design power_now temp; do
            c1_supply_value=$(c1_read_value "$c1_supply_dir/$c1_supply_attr")
            c1_emit_item "$c1_supply_name.$c1_supply_attr=$c1_supply_value"
        done
    done
    [ "$c1_supply_found" -eq 1 ] || printf '%s' unavailable
}

c1_collect_cpu() {
    if [ ! -r /proc/stat ]; then
        printf '%s' unavailable
        return
    fi
    awk '
        /^cpu / {
            printf "user=%s,nice=%s,system=%s,idle=%s,iowait=%s,irq=%s,softirq=%s,steal=%s,guest=%s,guest_nice=%s", $2,$3,$4,$5,$6,$7,$8,$9,$10,$11
            found=1
            exit
        }
        END { if (!found) printf "unavailable" }
    ' /proc/stat 2>/dev/null
}

c1_collect_ctxt() {
    if [ ! -r /proc/stat ]; then
        printf '%s' unavailable
        return
    fi
    c1_ctxt=$(awk '/^ctxt / { print $2; exit }' /proc/stat 2>/dev/null)
    if [ -n "$c1_ctxt" ]; then printf '%s' "$c1_ctxt"; else printf '%s' unavailable; fi
}

c1_collect_processes() {
    c1_emit_first=1
    c1_process_found=0
    for c1_process_comm in /proc/[0-9]*/comm; do
        [ -r "$c1_process_comm" ] || continue
        c1_process_name=$(head -n 1 "$c1_process_comm" 2>/dev/null)
        case "$c1_process_name" in
            C1ancher|app_daemon|adbd|wpa_supplicant|udhcpc|umtprd|sh|c1pkg) ;;
            *) continue ;;
        esac
        c1_process_dir=${c1_process_comm%/comm}
        c1_process_pid=${c1_process_dir##*/}
        [ -r "$c1_process_dir/stat" ] || continue
        c1_process_stat=$(awk -v name="$c1_process_name" '{ printf "name=%s,pid=%s,state=%s,utime=%s,stime=%s,cutime=%s,cstime=%s,nice=%s,threads=%s,starttime=%s,processor=%s", name,$1,$3,$14,$15,$16,$17,$19,$20,$22,$39 }' "$c1_process_dir/stat" 2>/dev/null)
        [ -n "$c1_process_stat" ] || continue
        c1_process_found=1
        c1_emit_item "$c1_process_stat"
    done
    [ "$c1_process_found" -eq 1 ] || printf '%s' unavailable
}

c1_collect_leds() {
    c1_emit_first=1
    c1_led_found=0
    for c1_led_dir in /sys/class/leds/*; do
        [ -d "$c1_led_dir" ] || continue
        c1_led_found=1
        c1_led_name=${c1_led_dir##*/}
        c1_emit_item "name=$c1_led_name,brightness=$(c1_read_value "$c1_led_dir/brightness"),max=$(c1_read_value "$c1_led_dir/max_brightness"),trigger=$(c1_read_value "$c1_led_dir/trigger")"
    done
    [ "$c1_led_found" -eq 1 ] || printf '%s' unavailable
}

c1_collect_cpu_power() {
    c1_emit_first=1
    c1_cpu_power_found=0
    for c1_cpu_policy in /sys/devices/system/cpu/cpufreq/policy* /sys/devices/system/cpu/cpu*/cpufreq; do
        [ -d "$c1_cpu_policy" ] || continue
        c1_cpu_power_found=1
        c1_emit_item "$c1_cpu_policy.cur=$(c1_read_value "$c1_cpu_policy/scaling_cur_freq"),min=$(c1_read_value "$c1_cpu_policy/scaling_min_freq"),max=$(c1_read_value "$c1_cpu_policy/scaling_max_freq"),governor=$(c1_read_value "$c1_cpu_policy/scaling_governor")"
    done
    for c1_idle_state in /sys/devices/system/cpu/cpu*/cpuidle/state*; do
        [ -d "$c1_idle_state" ] || continue
        c1_cpu_power_found=1
        c1_emit_item "$c1_idle_state.name=$(c1_read_value "$c1_idle_state/name"),usage=$(c1_read_value "$c1_idle_state/usage"),time=$(c1_read_value "$c1_idle_state/time"),disable=$(c1_read_value "$c1_idle_state/disable")"
    done
    [ "$c1_cpu_power_found" -eq 1 ] || printf '%s' unavailable
}

c1_add_wifi_iface() {
    c1_wifi_candidate=$1
    [ -d "/sys/class/net/$c1_wifi_candidate" ] || return
    case " $c1_wifi_ifaces " in
        *" $c1_wifi_candidate "*) ;;
        *) c1_wifi_ifaces="$c1_wifi_ifaces $c1_wifi_candidate" ;;
    esac
}

c1_collect_wifi() {
    c1_wifi_ifaces=
    if [ -r /proc/net/wireless ]; then
        for c1_wifi_iface in $(awk 'NR > 2 { gsub(":", "", $1); print $1 }' /proc/net/wireless 2>/dev/null); do
            c1_add_wifi_iface "$c1_wifi_iface"
        done
    fi
    for c1_wifi_dir in /sys/class/net/wlan* /sys/class/net/wl*; do
        [ -d "$c1_wifi_dir" ] || continue
        c1_add_wifi_iface "${c1_wifi_dir##*/}"
    done

    c1_emit_first=1
    c1_wifi_found=0
    for c1_wifi_iface in $c1_wifi_ifaces; do
        c1_wifi_found=1
        c1_wifi_base=/sys/class/net/$c1_wifi_iface
        c1_wifi_operstate=$(c1_read_value "$c1_wifi_base/operstate")
        c1_wifi_carrier=$(c1_read_value "$c1_wifi_base/carrier")
        c1_wifi_address=$(c1_read_value "$c1_wifi_base/address")
        c1_wifi_rx=$(c1_read_value "$c1_wifi_base/statistics/rx_packets")
        c1_wifi_tx=$(c1_read_value "$c1_wifi_base/statistics/tx_packets")
        c1_wifi_rx_bytes=$(c1_read_value "$c1_wifi_base/statistics/rx_bytes")
        c1_wifi_tx_bytes=$(c1_read_value "$c1_wifi_base/statistics/tx_bytes")
        c1_wifi_wireless=unavailable
        if [ -r /proc/net/wireless ]; then
            c1_wifi_line=$(awk -v name="$c1_wifi_iface" '$1 == name ":" { print; exit }' /proc/net/wireless 2>/dev/null)
            [ -z "$c1_wifi_line" ] || c1_wifi_wireless=$(c1_sanitize "$c1_wifi_line")
        fi
        c1_wifi_link=unavailable
        if command -v iw >/dev/null 2>&1; then
            c1_wifi_command=$(iw dev "$c1_wifi_iface" link 2>/dev/null)
            [ -z "$c1_wifi_command" ] || c1_wifi_link=$(c1_sanitize "$c1_wifi_command")
        elif command -v iwconfig >/dev/null 2>&1; then
            c1_wifi_command=$(iwconfig "$c1_wifi_iface" 2>/dev/null)
            [ -z "$c1_wifi_command" ] || c1_wifi_link=$(c1_sanitize "$c1_wifi_command")
        fi
        c1_emit_item "iface=$c1_wifi_iface,operstate=$c1_wifi_operstate,carrier=$c1_wifi_carrier,address=$c1_wifi_address,rx_packets=$c1_wifi_rx,tx_packets=$c1_wifi_tx,rx_bytes=$c1_wifi_rx_bytes,tx_bytes=$c1_wifi_tx_bytes,wireless=$c1_wifi_wireless,link=$c1_wifi_link"
    done
    [ "$c1_wifi_found" -eq 1 ] || printf '%s' unavailable
}

c1_collect_usb() {
    c1_emit_first=1
    c1_usb_found=0
    c1_usb_legacy=/sys/class/android_usb/android0
    if [ -d "$c1_usb_legacy" ]; then
        c1_usb_found=1
        c1_emit_item "android_usb.enable=$(c1_read_value "$c1_usb_legacy/enable"),android_usb.state=$(c1_read_value "$c1_usb_legacy/state")"
    fi
    for c1_usb_gadget in /sys/kernel/config/usb_gadget/*; do
        [ -d "$c1_usb_gadget" ] || continue
        c1_usb_found=1
        c1_usb_name=${c1_usb_gadget##*/}
        c1_usb_udc=$(c1_read_value "$c1_usb_gadget/UDC")
        c1_usb_state=unavailable
        [ "$c1_usb_udc" = unavailable ] || c1_usb_state=$(c1_read_value "/sys/class/udc/$c1_usb_udc/state")
        c1_usb_functions=
        for c1_usb_config in "$c1_usb_gadget"/configs/*; do
            [ -d "$c1_usb_config" ] || continue
            for c1_usb_function in "$c1_usb_config"/*; do
                [ -L "$c1_usb_function" ] || continue
                if [ -n "$c1_usb_functions" ]; then
                    c1_usb_functions="$c1_usb_functions,${c1_usb_function##*/}"
                else
                    c1_usb_functions=${c1_usb_function##*/}
                fi
            done
        done
        [ -n "$c1_usb_functions" ] || c1_usb_functions=none
        c1_emit_item "gadget=$c1_usb_name,udc=$c1_usb_udc,state=$c1_usb_state,functions=$c1_usb_functions"
    done
    for c1_usb_udc_dir in /sys/class/udc/*; do
        [ -d "$c1_usb_udc_dir" ] || continue
        c1_usb_found=1
        c1_usb_udc_name=${c1_usb_udc_dir##*/}
        c1_emit_item "udc=$c1_usb_udc_name,state=$(c1_read_value "$c1_usb_udc_dir/state")"
    done
    [ "$c1_usb_found" -eq 1 ] || printf '%s' unavailable
}

c1_collect_power_stats() {
    c1_emit_first=1
    c1_power_stats_found=0
    for c1_power_stats_file in /sys/power/suspend_stats/* /sys/kernel/debug/suspend_stats/* /sys/kernel/debug/suspend_stats; do
        [ -f "$c1_power_stats_file" ] && [ -r "$c1_power_stats_file" ] || continue
        c1_power_stats_found=1
        c1_emit_item "$c1_power_stats_file=$(c1_read_value "$c1_power_stats_file")"
    done
    [ "$c1_power_stats_found" -eq 1 ] || printf '%s' unavailable
}

c1_collect_wakeup_sources() {
    c1_emit_first=1
    c1_wakeup_found=0
    c1_wakeup_seen=' '
    for c1_wakeup_node in /sys/class/input/event*/device/power/wakeup /sys/devices/*/power/wakeup /sys/devices/*/*/power/wakeup /sys/devices/*/*/*/power/wakeup /sys/devices/*/*/*/*/power/wakeup; do
        [ -f "$c1_wakeup_node" ] && [ -r "$c1_wakeup_node" ] || continue
        case "$c1_wakeup_seen" in *" $c1_wakeup_node "*) continue ;; esac
        c1_wakeup_seen="$c1_wakeup_seen$c1_wakeup_node "
        c1_wakeup_found=1
        c1_emit_item "$c1_wakeup_node=$(c1_read_value "$c1_wakeup_node")"
    done
    [ "$c1_wakeup_found" -eq 1 ] || printf '%s' unavailable
}

c1_collect_epaper() {
    c1_emit_first=1
    c1_epaper_found=0
    c1_epaper_seen=' '
    for c1_epaper_dir in /sys/devices/platform/*/epaper /sys/class/epaper/* /sys/kernel/debug/epaper /sys/kernel/debug/epdc /sys/kernel/debug/*epaper* /sys/kernel/debug/*epd*; do
        [ -d "$c1_epaper_dir" ] || continue
        case "$c1_epaper_seen" in *" $c1_epaper_dir "*) continue ;; esac
        c1_epaper_seen="$c1_epaper_seen$c1_epaper_dir "
        c1_epaper_dir_has_file=0
        for c1_epaper_file in "$c1_epaper_dir"/*stat* "$c1_epaper_dir"/*count* "$c1_epaper_dir"/*refresh* "$c1_epaper_dir"/*update* "$c1_epaper_dir"/*frame* "$c1_epaper_dir"/*waveform*; do
            [ -f "$c1_epaper_file" ] && [ -r "$c1_epaper_file" ] || continue
            case "$c1_epaper_seen" in *" $c1_epaper_file "*) continue ;; esac
            c1_epaper_seen="$c1_epaper_seen$c1_epaper_file "
            c1_epaper_found=1
            c1_epaper_dir_has_file=1
            c1_emit_item "$c1_epaper_file=$(c1_read_value "$c1_epaper_file")"
        done
        if [ "$c1_epaper_dir_has_file" -eq 0 ]; then
            c1_epaper_found=1
            c1_emit_item "$c1_epaper_dir=present"
        fi
    done
    [ "$c1_epaper_found" -eq 1 ] || printf '%s' unavailable
}

printf 'timestamp_epoch\ttimestamp_utc\telapsed_seconds\tsample\tpower_supply\tcpu_total\tctxt\tprocesses\tcpu_power\tleds\twifi\tusb_gadget\tpower_state\tmem_sleep\twakeup_count\tpower_stats\twakeup_sources\tepaper\n'

c1_started=$(date +%s 2>/dev/null)
case "$c1_started" in ''|*[!0-9]*) echo 'date +%s is unavailable' >&2; exit 69 ;; esac
c1_sample=0
while :; do
    c1_now=$(date +%s 2>/dev/null)
    case "$c1_now" in ''|*[!0-9]*) c1_now=$c1_started ;; esac
    c1_elapsed=$((c1_now - c1_started))
    c1_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)
    [ -n "$c1_utc" ] || c1_utc=unavailable

    c1_power_supply=$(c1_collect_power_supply)
    c1_cpu=$(c1_collect_cpu)
    c1_ctxt=$(c1_collect_ctxt)
    c1_processes=$(c1_collect_processes)
    c1_cpu_power=$(c1_collect_cpu_power)
    c1_leds=$(c1_collect_leds)
    c1_wifi=$(c1_collect_wifi)
    c1_usb=$(c1_collect_usb)
    c1_power_state=$(c1_read_value /sys/power/state)
    c1_mem_sleep=$(c1_read_value /sys/power/mem_sleep)
    c1_wakeup_count=$(c1_read_value /sys/power/wakeup_count)
    c1_power_stats=$(c1_collect_power_stats)
    c1_wakeup_sources=$(c1_collect_wakeup_sources)
    c1_epaper=$(c1_collect_epaper)

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$c1_now" "$c1_utc" "$c1_elapsed" "$c1_sample" "$c1_power_supply" "$c1_cpu" "$c1_ctxt" "$c1_processes" "$c1_cpu_power" "$c1_leds" "$c1_wifi" "$c1_usb" "$c1_power_state" "$c1_mem_sleep" "$c1_wakeup_count" "$c1_power_stats" "$c1_wakeup_sources" "$c1_epaper"

    [ "$c1_elapsed" -ge "$c1_duration" ] && break
    sleep "$c1_interval" || exit 70
    c1_sample=$((c1_sample + 1))
done