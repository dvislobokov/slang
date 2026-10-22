// startup: the configuration CLI, Go side of main.zl (encoding/json).
package main

import (
	"encoding/json"
	"fmt"
	"os"
)

type Route struct {
	Path      string
	Upstream  string
	TimeoutMs int
	Retries   int
	Tags      []string
}

type Config struct {
	Name    string
	Version int
	Debug   bool
	Routes  []Route
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: cli <config.json>")
		os.Exit(2)
	}
	b, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	var cfg Config
	if err := json.Unmarshal(b, &cfg); err != nil {
		panic(err)
	}
	timeouts, tags := 0, 0
	for _, r := range cfg.Routes {
		timeouts += r.TimeoutMs * r.Retries
		tags += len(r.Tags)
	}
	fmt.Println(cfg.Name, cfg.Version, cfg.Debug, "routes", len(cfg.Routes), "timeouts", timeouts, "tags", tags)
}
