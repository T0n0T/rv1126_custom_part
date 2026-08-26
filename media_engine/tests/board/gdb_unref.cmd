# Board-side gdb script (test tool only): catch the double gst_object_unref.
set pagination off
set confirm off
set environment LD_LIBRARY_PATH /oem/usr/lib:/oem/usr/lib/gstreamer-1.0
set environment GST_PLUGIN_PATH /oem/usr/lib/gstreamer-1.0
set environment GST_PLUGIN_SCANNER /oem/usr/libexec/gstreamer-1.0/gst-plugin-scanner
break gst_object_unref
commands
silent
set $obj = $x0
set $rc = *(unsigned int *)((char *)$obj + 8)
if $rc == 0
printf "=== HIT double-unref addr=%p ref=%d ===\n", $obj, $rc
frame 1
info registers x19 x20 x21 x22 x0
bt 30
x/8gx $obj
x/48bx $obj
x/24gx $x19
x/8gx $x22
info sharedlibrary
info files
end
continue
end
run --config media_engine.yaml
