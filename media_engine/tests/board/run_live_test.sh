#!/bin/sh
# Board-side live-stream RPC test (test tool only).
# Streams H.264 RTP to 192.168.1.88:20000 (host UDP listener), then stops.

cd /tmp/me || exit 1

P='{"session_id":"s1","channel_id":"ch1","codec":"h264","width":1920,"height":1080,"fps":30,"bitrate":4096,"dest_ip":"192.168.1.88","dest_port":20000,"ssrc":"123456789","payload_type":98}'

echo "== ping =="
./rpc_client /tmp/me.sock media.ping

echo "== start_live =="
./rpc_client /tmp/me.sock media.start_live "$P"

echo "== streaming for 5s =="
sleep 5

echo "== get_status (live) =="
./rpc_client /tmp/me.sock media.get_status

echo "== stop_live =="
./rpc_client /tmp/me.sock media.stop_live '{"session_id":"s1"}'

echo "== get_status (idle) =="
./rpc_client /tmp/me.sock media.get_status
