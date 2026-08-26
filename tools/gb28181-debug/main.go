// Command gb28181-debug is a development-only media simulator.
//
// It stands in for media_engine while that component is not ready: it pushes
// a software-encoded H.264 test pattern over RTP to the address a GB28181
// platform advertised in its INVITE. It is NOT part of the firmware product.
package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"syscall"
)

func main() {
	dest := flag.String("dest", "192.168.1.88:10003", "RTP destination host:port")
	ssrc := flag.Int64("ssrc", 200004568, "RTP SSRC (decimal; same as GB28181 y= value)")
	pt := flag.Int("pt", 98, "RTP payload type")
	size := flag.String("size", "640x360", "test pattern size WxH")
	fps := flag.Int("fps", 25, "frame rate")
	bitrate := flag.Int("bitrate", 800, "video bitrate in kbps")
	duration := flag.Int("duration", 0, "push duration in seconds; 0 = until Ctrl+C")
	ffmpegBin := flag.String("ffmpeg", "ffmpeg", "path to ffmpeg binary")
	flag.Parse()

	log := slog.Default()
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	args := []string{
		"-hide_banner", "-loglevel", "warning",
		"-re", "-f", "lavfi", "-i", "testsrc2=size=" + *size + ":rate=" + strconv.Itoa(*fps),
		"-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
		"-g", "50", "-b:v", strconv.Itoa(*bitrate) + "k",
	}
	if *duration > 0 {
		args = append(args, "-t", strconv.Itoa(*duration))
	}
	args = append(args,
		"-f", "rtp",
		"-payload_type", strconv.Itoa(*pt),
		"-ssrc", strconv.FormatInt(*ssrc, 10),
		"rtp://"+*dest,
	)

	log.Info("pushing simulated GB28181 stream",
		"dest", *dest, "ssrc", *ssrc, "pt", *pt, "size", *size,
		"fps", *fps, "bitrate", *bitrate, "duration", *duration)
	log.Info("command", "cmd", (*ffmpegBin)+" "+fmt.Sprint(args))

	cmd := exec.CommandContext(ctx, *ffmpegBin, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		if ctx.Err() != nil {
			log.Info("stopped by signal")
			return
		}
		log.Error("ffmpeg failed", "error", err)
		os.Exit(1)
	}
}
