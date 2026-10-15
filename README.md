# NetBench

NetBench is a standalone Linux C++20 CLI for controlled, single-sided IPv4 network resilience
testing. It needs no target-side agent, NetBench receiver, token, API, or application acknowledgement.
Every target must match a CIDR explicitly authorized on that command line.

## What it measures

- TCP: paced nonblocking connection establishment, established concurrency, success/failure,
  connect latency, timeout, reset/socket errors, success rate, and Linux `TCP_INFO`
  retransmissions where available.
- UDP: attempted PPS, successful local datagram submissions, sender throughput, local send failures,
  and Linux error-queue ICMP destination-unreachable feedback. It does not infer receiver packet loss.
- ICMP Echo: RTT, timeouts, packet loss, latency percentiles, and before/during/after responsiveness.
- Process CPU utilization and peak memory for every run.
- Baseline, sustained, ramp, and short burst load shapes, plus a bounded recovery observation window.
- Human terminal reports, live rate updates, and optional JSON/CSV exports.

The implementation uses nonblocking POSIX sockets, `epoll`, RAII descriptors, monotonic clocks,
`std::jthread`, stop tokens, and a thread-safe metrics collector. See
[architecture](docs/ARCHITECTURE.md) and the [CLI specification](docs/CLI.md).

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Optional sanitizer build:

```bash
cmake -S . -B build-asan -DNETBENCH_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

ICMP uses a Linux ping socket and falls back to a raw ICMP socket. Configure `ping_group_range` or
grant `CAP_NET_RAW` if the OS does not permit either. NetBench itself does not elevate privileges.

## Examples

```bash
./build/netbench tcp --target 192.0.2.10 --allow-cidr 192.0.2.0/24 \
  --port 443 --connections 100 --rate 50cps --duration 30s

./build/netbench udp --target 192.0.2.10 --allow-cidr 192.0.2.0/24 \
  --port 9000 --pps 1000 --size 512 --duration 30s

./build/netbench icmp --target 192.0.2.10 --allow-cidr 192.0.2.0/24 \
  --rate 10pps --duration 30s --json result.json --csv result.csv
```

Defaults cap each run at 100,000 PPS, 1 Gbps, 1,000 connections/s, 1,024 concurrent connections,
and 60 seconds. `--max-*` options may lower those runtime limits or raise them only as far as the
compiled ceilings: 1,000,000 PPS, 10 Gbps, 10,000 connections/s, 4,096 concurrent connections, and
300 seconds. Burst duration cannot exceed 10 seconds.

Use NetBench only on systems and networks you own or are authorized to test. It deliberately omits
source spoofing, reflection/amplification behavior, unrestricted flooding, DNS target resolution,
and target-side receiver code.
