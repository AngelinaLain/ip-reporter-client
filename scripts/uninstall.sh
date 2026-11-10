#!/bin/sh
# 默认保留配置和服务账户，以便之后重新安装并保持设备标识。
set -eu
purge=0
case "${1:-}" in
    '') ;;
    --purge-config) purge=1; shift ;;
    -h|--help) echo 'Usage: sudo sh scripts/uninstall.sh [--purge-config]'; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
esac
[ "$#" -eq 0 ] || { echo 'Unexpected extra arguments.' >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo 'Run as root (sudo).' >&2; exit 1; }
case "$(uname -s)" in
    Linux)
        if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
            if [ -f /etc/systemd/system/ip-reporter.service ]; then
                systemctl disable --now ip-reporter.service
                rm -f /etc/systemd/system/ip-reporter.service
                systemctl daemon-reload
                systemctl reset-failed ip-reporter.service >/dev/null 2>&1 || true
            fi
        elif command -v rc-service >/dev/null 2>&1; then
            if [ -f /etc/init.d/ip-reporter ]; then
                if rc-service ip-reporter status >/dev/null 2>&1; then rc-service ip-reporter stop; fi
                rc-update del ip-reporter default
                rm -f /etc/init.d/ip-reporter
            fi
        else echo 'No supported Linux service manager found.' >&2; exit 1; fi ;;
    Darwin)
        if launchctl print system/org.ip-reporter.agent >/dev/null 2>&1; then
            launchctl bootout system/org.ip-reporter.agent
        fi
        rm -f /Library/LaunchDaemons/org.ip-reporter.agent.plist ;;
    FreeBSD)
        if [ -f /usr/local/etc/rc.d/ip_reporter ]; then
            if service ip_reporter onestatus >/dev/null 2>&1; then service ip_reporter onestop; fi
            sysrc -x ip_reporter_enable >/dev/null 2>&1 || true
            rm -f /usr/local/etc/rc.d/ip_reporter
        fi ;;
    *) echo 'Unsupported service manager.' >&2; exit 1 ;;
esac
rm -f /usr/local/bin/ip-reporter
if [ "$purge" = 1 ]; then rm -f /etc/ip-reporter/config.json; fi
echo 'IP Reporter uninstalled.'
if [ "$purge" = 0 ]; then echo 'Configuration retained at /etc/ip-reporter/config.json'; fi
