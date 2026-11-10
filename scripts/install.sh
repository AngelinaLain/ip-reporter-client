#!/bin/sh
# 安装到系统服务；默认保留已有配置，仅在 --reconfigure 时重建。
set -eu
umask 077

usage() {
    cat <<'EOF'
Usage: sudo sh scripts/install.sh --endpoint URL [options]
  --binary PATH             Built ip-reporter binary (auto-detected if omitted)
  --endpoint URL            HTTP(S) receiver URL; required for initial config
  --token-file PATH         Read bearer token from file, never from argv
  --device-id ID            Explicit identity (existing ID retained on reconfigure)
  --poll-seconds N          Address check interval (default: 60)
  --heartbeat-seconds N     Heartbeat interval; 0 disables it (default: 300)
  --timeout-seconds N       Request timeout (default: 10)
  --allow-http              Explicitly permit unencrypted HTTP
  --reconfigure             Replace existing configuration
EOF
}

binary=''; endpoint=''; token_file=''; device_id=''
poll_seconds=60; heartbeat_seconds=300; timeout_seconds=10
allow_http=0; reconfigure=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary|--endpoint|--token-file|--device-id|--poll-seconds|--heartbeat-seconds|--timeout-seconds)
            [ "$#" -ge 2 ] || { echo "Missing value for $1" >&2; exit 2; }
            option=$1; value=$2; shift 2
            case "$option" in
                --binary) binary=$value ;; --endpoint) endpoint=$value ;;
                --token-file) token_file=$value ;; --device-id) device_id=$value ;;
                --poll-seconds) poll_seconds=$value ;; --heartbeat-seconds) heartbeat_seconds=$value ;;
                --timeout-seconds) timeout_seconds=$value ;;
            esac ;;
        --allow-http) allow_http=1; shift ;;
        --reconfigure) reconfigure=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done
[ "$(id -u)" = 0 ] || { echo 'Run this installer as root (sudo).' >&2; exit 1; }
os=$(uname -s)
case "$os" in
    Linux)
        if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then manager=systemd
        elif command -v rc-service >/dev/null 2>&1; then manager=openrc
        else echo 'Linux requires a running systemd or OpenRC service manager.' >&2; exit 1; fi ;;
    Darwin) manager=launchd ;;
    FreeBSD) manager=freebsd ;;
    *) echo "Unsupported service manager on $os; run the binary with your supervisor." >&2; exit 1 ;;
esac
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
services_dir=$script_dir/../packaging/services
if [ ! -d "$services_dir" ]; then services_dir=$script_dir/../services; fi
[ -d "$services_dir" ] || { echo 'Service templates are missing from the release package.' >&2; exit 1; }
if [ -z "$binary" ]; then
    for candidate in "$script_dir/../bin/ip-reporter" "$script_dir/../../../bin/ip-reporter" \
        "$script_dir/../build/ip-reporter"; do
        if [ -f "$candidate" ] && [ -x "$candidate" ]; then binary=$candidate; break; fi
    done
fi
[ -n "$binary" ] && [ -f "$binary" ] && [ -x "$binary" ] || {
    echo 'Cannot find an executable ip-reporter binary; use --binary PATH.' >&2; exit 1;
}
binary_dir=$(CDPATH='' cd -- "$(dirname -- "$binary")" && pwd)
binary=$binary_dir/$(basename -- "$binary")
# 在服务路径之外完成配置检查，避免错误参数破坏现有服务。
config_dir=/etc/ip-reporter
config_path=$config_dir/config.json
target_binary=/usr/local/bin/ip-reporter
new_binary=/usr/local/bin/.ip-reporter.new.$$
new_config=$config_dir/.config.new.$$
needs_config=0
if [ "$reconfigure" = 1 ] || [ ! -f "$config_path" ]; then needs_config=1; fi
if [ "$needs_config" = 1 ] && [ -z "$endpoint" ]; then
    echo '--endpoint is required for first installation or --reconfigure.' >&2; exit 2
fi
if [ "$needs_config" = 0 ] && { [ -n "$endpoint" ] || [ -n "$token_file" ] || [ -n "$device_id" ] || [ "$allow_http" = 1 ]; }; then
    echo 'Existing configuration is preserved; use --reconfigure to change settings.' >&2
fi
# 使用程序校验旧配置并仅读取 ID；配置损坏时要求显式 ID，防止静默新增机器。
if [ "$needs_config" = 1 ] && [ -z "$device_id" ] && [ -f "$config_path" ]; then
    if ! device_id=$("$binary" --print-device-id --config "$config_path"); then
        echo 'Cannot preserve device_id from existing configuration. Repair it or supply --device-id explicitly.' >&2
        exit 2
    fi
    case "$device_id" in ''|*[!A-Za-z0-9._-]*) echo 'Existing device_id is invalid.' >&2; exit 2 ;; esac
    [ "${#device_id}" -le 128 ] || { echo 'Existing device_id is invalid.' >&2; exit 2; }
fi
cleanup() { rm -f "$new_binary" "$new_config"; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
mkdir -p /usr/local/bin "$config_dir"
chmod 750 "$config_dir"
if [ "$needs_config" = 1 ]; then
    set -- --init --config "$new_config" --endpoint "$endpoint" \
        --poll-seconds "$poll_seconds" --heartbeat-seconds "$heartbeat_seconds" --timeout-seconds "$timeout_seconds"
    if [ -n "$token_file" ]; then set -- "$@" --token-file "$token_file"; fi
    if [ -n "$device_id" ]; then set -- "$@" --device-id "$device_id"; fi
    if [ "$allow_http" = 1 ]; then set -- "$@" --allow-http; fi
    "$binary" "$@"
    "$binary" --check-config --config "$new_config"
else
    "$binary" --check-config --config "$config_path"
fi
cp "$binary" "$new_binary"
chmod 755 "$new_binary"
chown root "$new_binary"

# 所有平台均用低权限账户运行；只让该账户读取配置。
case "$manager" in
    systemd|openrc)
        service_user=ip-reporter
        if ! id -u "$service_user" >/dev/null 2>&1; then
            if command -v useradd >/dev/null 2>&1; then
                nologin=/usr/sbin/nologin; [ -x "$nologin" ] || nologin=/sbin/nologin
                useradd --system --user-group --no-create-home --home-dir /nonexistent --shell "$nologin" "$service_user"
            elif command -v adduser >/dev/null 2>&1 && command -v addgroup >/dev/null 2>&1; then
                addgroup -S "$service_user"
                adduser -S -D -H -h /nonexistent -s /sbin/nologin -G "$service_user" "$service_user"
            else echo 'Create an unprivileged ip-reporter user before installing.' >&2; exit 1; fi
        fi
        service_group=$(id -gn "$service_user") ;;
    launchd) service_user=nobody; service_group=nobody ;;
    freebsd)
        service_user=ip_reporter
        if ! id -u "$service_user" >/dev/null 2>&1; then
            pw useradd "$service_user" -d /nonexistent -s /usr/sbin/nologin -c 'IP Reporter service'
        fi
        service_group=$(id -gn "$service_user") ;;
esac
chown "root:$service_group" "$config_dir"
if [ "$needs_config" = 1 ]; then
    chmod 640 "$new_config"
    chown "root:$service_group" "$new_config"
fi

case "$manager" in
    systemd)
        if systemctl is-active --quiet ip-reporter.service; then systemctl stop ip-reporter.service; fi ;;
    openrc) if rc-service ip-reporter status >/dev/null 2>&1; then rc-service ip-reporter stop; fi ;;
    launchd)
        if launchctl print system/org.ip-reporter.agent >/dev/null 2>&1; then
            launchctl bootout system/org.ip-reporter.agent
        fi ;;
    freebsd)
        if service ip_reporter onestatus >/dev/null 2>&1; then service ip_reporter onestop; fi ;;
esac
mv -f "$new_binary" "$target_binary"
if [ "$needs_config" = 1 ]; then mv -f "$new_config" "$config_path"; fi
chmod 640 "$config_path"
chown "root:$service_group" "$config_path"

case "$manager" in
    systemd)
        cp "$services_dir/ip-reporter.service" /etc/systemd/system/ip-reporter.service
        chmod 644 /etc/systemd/system/ip-reporter.service
        systemctl daemon-reload
        systemctl enable --now ip-reporter.service ;;
    openrc)
        cp "$services_dir/ip-reporter.openrc" /etc/init.d/ip-reporter
        chmod 755 /etc/init.d/ip-reporter
        rc-update add ip-reporter default
        rc-service ip-reporter start ;;
    launchd)
        cp "$services_dir/org.ip-reporter.agent.plist" /Library/LaunchDaemons/org.ip-reporter.agent.plist
        chown root:wheel /Library/LaunchDaemons/org.ip-reporter.agent.plist
        chmod 644 /Library/LaunchDaemons/org.ip-reporter.agent.plist
        plutil -lint /Library/LaunchDaemons/org.ip-reporter.agent.plist
        launchctl bootstrap system /Library/LaunchDaemons/org.ip-reporter.agent.plist ;;
    freebsd)
        mkdir -p /usr/local/etc/rc.d
        cp "$services_dir/ip_reporter.freebsd" /usr/local/etc/rc.d/ip_reporter
        chmod 755 /usr/local/etc/rc.d/ip_reporter
        sysrc ip_reporter_enable=YES
        service ip_reporter start ;;
esac
echo "Installed IP Reporter ($manager). Config: $config_path"
