#!/bin/sh
# Runs parse_test with pipeline variants (test tool only).
cd /tmp/me || exit 1
export LD_LIBRARY_PATH=/oem/usr/lib:/oem/usr/lib/gstreamer-1.0

./parse_test \
	"v4l2src device=/dev/video24 io-mode=dmabuf ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! fakesink" \
	'v4l2src device="/dev/video24" io-mode=dmabuf ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! fakesink' \
	"v4l2src device='/dev/video24' io-mode=dmabuf ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! fakesink" \
	"queue ! mpph264enc bitrate=4096 ! h264parse ! rtph264pay pt=98 ssrc=123456789 config-interval=1 ! udpsink host=\"192.168.1.88\" port=20000 sync=false" \
	"mpph264enc" \
	"queue ! mpph264enc" \
	"mpph264enc ! h264parse" \
	"mpph264enc bitrate=4096 ! h264parse" \
	"queue ! mpph264enc bitrate=4096 ! h264parse" \
	"mpph264enc ! h264parse ! rtph264pay pt=98"
