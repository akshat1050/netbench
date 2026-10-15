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
- A separate, low-rate `validate-source` path for anti-spoofing policy checks in
  isolated RFC 1918 labs.

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

### Isolated source-address validation

Create the lab first and use addresses from that lab. This example assigns the destination to one
end of a veth pair and runs NetBench from the other network namespace:

```bash
sudo ip netns add nb-source-lab
sudo ip netns add nb-target-lab
sudo ip link add nb-probe0 type veth peer name nb-observe0
sudo ip link set nb-probe0 netns nb-source-lab
sudo ip link set nb-observe0 netns nb-target-lab
sudo ip -n nb-source-lab link set lo up
sudo ip -n nb-source-lab addr add 10.203.0.10/24 dev nb-probe0
sudo ip -n nb-source-lab link set nb-probe0 up
sudo ip -n nb-target-lab link set lo up
sudo ip -n nb-target-lab addr add 10.203.0.30/32 dev lo
sudo ip -n nb-target-lab addr add 10.203.0.20/24 dev nb-observe0
sudo ip -n nb-target-lab link set nb-observe0 up

sudo ip netns exec nb-source-lab ./build/netbench validate-source \
  --target 10.203.0.20 --destination-allow-cidr 10.203.0.0/24 \
  --interface nb-probe0 --allow-interface nb-probe0 \
  --spoof-source --source-ip 10.203.0.30 \
  --source-allow-cidr 10.203.0.0/24 \
  --pps 5 --duration 10s --report source-validation.json
```

The spoofed source must be a lab-owned address reserved for the test; above it is assigned only in
the observer namespace. `validate-source` requires
`CAP_NET_RAW`; it never elevates privileges. It sends only a fixed ICMP Echo probe, is hard-limited
to 10 PPS, 30 seconds, and 300 probes, and accepts only RFC 1918 host addresses in explicit source,
destination, and interface allowlists. Loopback, link-local, multicast, public, network, and
broadcast source addresses are rejected. Source options are rejected by `tcp`, `udp`, and `icmp`.

Successful local transmission is not proof of firewall forwarding or dropping. The report keeps
local submissions and locally visible ICMP observations separate and marks external verification
false unless a separate observer establishes the result; NetBench does not provide such an
observer.

Defaults cap each run at 100,000 PPS, 1 Gbps, 1,000 connections/s, 1,024 concurrent connections,
and 60 seconds. `--max-*` options may lower those runtime limits or raise them only as far as the
compiled ceilings: 1,000,000 PPS, 10 Gbps, 10,000 connections/s, 4,096 concurrent connections, and
300 seconds. Burst duration cannot exceed 10 seconds.

Use NetBench only on systems and networks you own or are authorized to test. It deliberately omits
general-purpose source spoofing, source rotation, reflection/amplification behavior, spoofed TCP,
unrestricted flooding, DNS target resolution, and target-side receiver code.
