package main

import (
	"bufio"
	"fmt"
	"io"
	"strconv"
	"strings"
)

// parsePidstatCPU reads `pidstat -p <pid> 1 N -u` output and returns the
// mean %CPU across samples, dropping the first sample (often a low outlier)
// and any "Average:" trailer line.
//
// pidstat output rows look like:
//
//	HH:MM:SS  UID  PID  %usr  %system  %guest  %wait  %CPU  CPU  Command
//
// We locate the %CPU column from the header line, then parse subsequent
// rows that start with a time-of-day token (HH:MM:SS).
func parsePidstatCPU(r io.Reader) (mean float64, samples int, err error) {
	sc := bufio.NewScanner(r)
	cpuCol := -1
	var values []float64

	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		fields := strings.Fields(line)
		if cpuCol < 0 {
			// Header: detect by presence of "%CPU"
			for i, f := range fields {
				if f == "%CPU" {
					cpuCol = i
					break
				}
			}
			continue
		}
		// Ignore "Average:" trailer
		if strings.HasPrefix(fields[0], "Average") {
			continue
		}
		// Must look like a time-of-day row: HH:MM:SS
		if !isTimeOfDay(fields[0]) {
			continue
		}
		if cpuCol >= len(fields) {
			continue
		}
		v, perr := strconv.ParseFloat(fields[cpuCol], 64)
		if perr != nil {
			continue
		}
		values = append(values, v)
	}
	if err := sc.Err(); err != nil {
		return 0, 0, err
	}
	if len(values) < 2 {
		return 0, 0, fmt.Errorf("pidstat: not enough samples (got %d)", len(values))
	}
	// Drop the first sample (pidstat's first interval can be a cold-start outlier).
	values = values[1:]
	sum := 0.0
	for _, v := range values {
		sum += v
	}
	return sum / float64(len(values)), len(values), nil
}

func isTimeOfDay(s string) bool {
	// Matches HH:MM:SS with optional AM/PM suffix not present in -u output.
	if len(s) != 8 {
		return false
	}
	if s[2] != ':' || s[5] != ':' {
		return false
	}
	for _, idx := range []int{0, 1, 3, 4, 6, 7} {
		if s[idx] < '0' || s[idx] > '9' {
			return false
		}
	}
	return true
}

func runClient(args []string) {
	panic("not implemented")
}
