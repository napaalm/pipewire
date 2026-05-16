#!/usr/bin/env bash

set -e

pipewire_bin="$1"
disable_deadline="${PIPEWIRE_DISABLE_MODULE_DEADLINE:-}"

if [ -n "$disable_deadline" ] && [ "$disable_deadline" != "0" ]; then
	echo "module-deadline disabled; skipping CAP_SYS_NICE setup"
	exit 0
fi

setcap_bin="$(command -v setcap 2>/dev/null || true)"
if [ -z "$setcap_bin" ]; then
	echo "setcap not found; running without CAP_SYS_NICE"
	exit 0
fi

getcap_bin="$(command -v getcap 2>/dev/null || true)"
if [ -n "$getcap_bin" ]; then
	if "$getcap_bin" "$pipewire_bin" 2>/dev/null | grep -q 'cap_sys_nice=ep'; then
		echo "CAP_SYS_NICE already enabled on $pipewire_bin"
		exit 0
	fi
else
	echo "getcap not found; cannot verify existing capabilities"
fi

setcap_mode="${PIPEWIRE_SETCAP:-ask}"
answer="n"

case "$setcap_mode" in
	[yY]|[yY][eE][sS]|1|true|TRUE)
		answer="y"
		;;
	[nN]|[nN][oO]|0|false|FALSE)
		answer="n"
		;;
	ask|ASK|"")
		if [ -t 0 ]; then
			printf "Enable CAP_SYS_NICE on %s to use module-deadline? [y/N] " "$pipewire_bin"
			read -r answer
		else
			echo "non-interactive execution detected; enabling CAP_SYS_NICE setup automatically"
			answer="y"
		fi
		;;
	*)
		echo "invalid PIPEWIRE_SETCAP='$setcap_mode'; expected yes|no|ask"
		echo "running without CAP_SYS_NICE"
		;;
esac

case "$answer" in
	[yY]|[yY][eE][sS])
		sudo_bin="$(command -v sudo 2>/dev/null || true)"
		if [ -n "$sudo_bin" ]; then
			"$sudo_bin" "$setcap_bin" cap_sys_nice=ep "$pipewire_bin" || \
				echo "setcap failed; running without CAP_SYS_NICE"
		else
			"$setcap_bin" cap_sys_nice=ep "$pipewire_bin" || \
				echo "setcap failed and sudo not found; running without CAP_SYS_NICE"
		fi
		;;
	*)
		echo "CAP_SYS_NICE not enabled; running without module-deadline capability"
		;;
esac
