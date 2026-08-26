#!/bin/sh
# Board-side capture debug helper (test tool only, NOT product code).
# Compares gst-launch pipelines and the reference gst_aiq_preview so camera
# bring-up issues can be isolated from media_engine logic.

export LD_LIBRARY_PATH=/oem/usr/lib:/oem/usr/lib/gstreamer-1.0
export GST_PLUGIN_PATH=/oem/usr/lib/gstreamer-1.0
export GST_PLUGIN_SCANNER=/oem/usr/libexec/gstreamer-1.0/gst-plugin-scanner

echo "=== versions ==="
/oem/usr/bin/gst-launch-1.0 --version 2>&1 | head -3

run_launch() {
	name=$1
	desc=$2
	echo "=== $name ==="
	/oem/usr/bin/gst-launch-1.0 "$desc" -v >"/tmp/me/$name.log" 2>&1 &
	pid=$!
	sleep 4
	kill "$pid" 2>/dev/null
	sleep 1
	grep -E "ERROR|Cannot|WARN|caps =" "/tmp/me/$name.log" | head -8
}

run_launch one_arg_nq \
	'v4l2src device=/dev/video24 io-mode=dmabuf ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! fakesink'

run_launch quoted \
	'v4l2src device="/dev/video24" io-mode=dmabuf ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! fakesink'

run_launch quoted_sq \
	"v4l2src device='/dev/video24' io-mode=dmabuf ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! fakesink"

run_launch tee_base \
	'v4l2src device="/dev/video24" io-mode=dmabuf ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! tee name=t t. ! queue ! fakesink'

echo "=== gst_aiq_preview (AIQ + mmap capture) ==="
/oem/usr/bin/gst_aiq_preview \
	-p "v4l2src device=/dev/video24 io-mode=mmap ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! fakesink" \
	>/tmp/me/preview.log 2>&1 &
pid=$!
sleep 8
kill "$pid" 2>/dev/null
sleep 1
grep -E "AIQ|aiq|ERROR|Cannot|WARN|pipeline" /tmp/me/preview.log | head -12
