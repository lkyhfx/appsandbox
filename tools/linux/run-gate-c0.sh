#!/bin/sh
set -eu

RUNTIME_DIR=/run/user/$(id -u)
XAUTH=$(find "$RUNTIME_DIR" -maxdepth 1 -name '.mutter-Xwaylandauth.*' -print -quit)
export XDG_RUNTIME_DIR="$RUNTIME_DIR"
export WAYLAND_DISPLAY=wayland-0
export DISPLAY=:0
export XAUTHORITY="$XAUTH"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$RUNTIME_DIR/bus"

mark()
{
	printf 'ASB_C0_WORKLOAD %s\n' "$1" | sudo tee /dev/kmsg >/dev/null
}

kill_child()
{
	if [ "${child_pid:-}" ]; then
		kill "$child_pid" 2>/dev/null || true
		wait "$child_pid" 2>/dev/null || true
		child_pid=
	fi
}
trap kill_child EXIT INT TERM

mark IDLE_START
sleep 10
mark IDLE_END

mark WINDOW_DRAG_START
glxgears >/tmp/asb-c0-glxgears-drag.log 2>&1 &
child_pid=$!
sleep 2
window_id=$(xdotool search --sync --name glxgears | head -n 1)
i=0
while [ "$i" -lt 360 ]; do
	x=$((80 + (i % 120) * 8))
	y=$((80 + (i % 70) * 6))
	xdotool windowmove "$window_id" "$x" "$y"
	i=$((i + 1))
	sleep 0.01
done
kill_child
mark WINDOW_DRAG_END

mark GLX_START
timeout 12 glxgears >/tmp/asb-c0-glxgears.log 2>&1 || true
mark GLX_END

mark VULKAN_START
timeout 12 vkcube >/tmp/asb-c0-vkcube.log 2>&1 || true
mark VULKAN_END

mark VIDEO_START
timeout 12 gst-launch-1.0 -q videotestsrc is-live=true pattern=ball \
	! video/x-raw,framerate=60/1,width=1280,height=720 \
	! videoconvert ! glimagesink sync=true \
	>/tmp/asb-c0-video.log 2>&1 || true
mark VIDEO_END

mark MATRIX_END
