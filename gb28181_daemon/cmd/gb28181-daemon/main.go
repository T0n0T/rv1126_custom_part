package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"xiaoyu/gb28181-daemon/internal/app"
	"xiaoyu/gb28181-daemon/internal/config"
)

func main() {
	configPath := flag.String("config", "configs/gb28181.json", "path to JSON config")
	flag.Parse()

	cfg, err := config.Load(*configPath)
	if err != nil {
		slog.Error("load config", "error", err)
		os.Exit(1)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if err := app.Run(ctx, cfg); err != nil {
		fmt.Fprintln(os.Stderr, "daemon error:", err)
		os.Exit(1)
	}
}
