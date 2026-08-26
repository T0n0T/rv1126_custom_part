# Board-side gdb script (test tool only): check whether the double-unref'd
# object is the tee src pad or the branch sink pad.
set pagination off
set confirm off
set environment LD_LIBRARY_PATH /oem/usr/lib:/oem/usr/lib/gstreamer-1.0
set environment GST_PLUGIN_PATH /oem/usr/lib/gstreamer-1.0
set environment GST_PLUGIN_SCANNER /oem/usr/libexec/gstreamer-1.0/gst-plugin-scanner

break gst_runner_stop_live
commands
silent
set $t = ((GstRunner *)r)->live.tee_src
set $b = ((GstRunner *)r)->live.branch_sink
set $bn = ((GstRunner *)r)->live.branch
printf "STOP tee_src=%p branch_sink=%p bin=%p\n", $t, $b, $bn
continue
end

break gst_object_unref
commands
silent
set $obj = $x0
set $rc = *(unsigned int *)((char *)$obj + 8)
if $rc == 0
printf "HIT obj=%p (tee_src=%p branch_sink=%p)\n", $obj, $t, $b
if $obj == $t
printf "MATCH tee_src pad\n"
end
if $obj == $b
printf "MATCH branch_sink pad\n"
end
bt 12
end
continue
end

run --config media_engine.yaml
