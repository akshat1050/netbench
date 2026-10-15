# NetBench CLI specification

```text
netbench tcp  --target IPv4 --allow-cidr CIDR --port PORT
              --connections N --rate Ncps --duration TIME [options]
netbench udp  --target IPv4 --allow-cidr CIDR --port PORT
              --pps N --size BYTES --duration TIME [options]
netbench icmp --target IPv4 --allow-cidr CIDR
              --rate Npps --duration TIME [options]
netbench validate-source --target IPv4 --destination-allow-cidr CIDR
              --interface IFACE --allow-interface IFACE --spoof-source
              --source-ip IPv4 --source-allow-cidr CIDR
              --pps N --duration TIME [--report FILE.json|FILE.csv]
```

Common options:

```text
--profile baseline|sustained|ramp|burst  Load shape (default: sustained)
--timeout TIME                           Per-response/connect timeout (default: 3s)
--recovery TIME                          Post-load observation window (default: 3s)
--interval TIME                          Terminal progress interval (default: 1s)
--json FILE                              Write final JSON report
--csv FILE                               Write one-row CSV report
--max-pps N                              Runtime PPS safety limit
--max-bandwidth RATE                     Runtime bandwidth safety limit
--max-connection-rate Ncps               Runtime connect-rate safety limit
--max-concurrency N                      Runtime established-socket safety limit
--max-duration TIME                      Runtime duration safety limit
```

`--allow-cidr` is repeatable and mandatory. Targets are IPv4 literals; DNS is deliberately excluded
so authorization cannot change after validation. Rates accept `pps`, `cps`, `bps`, `kbps`, `mbps`,
or `gbps` as appropriate. Times accept `ms`, `s`, or `m`.

TCP `--connections` is the maximum number of simultaneously established/connecting sockets;
`--rate` paces their creation. UDP `--pps` controls datagram attempts and `--size` controls each
datagram. ICMP `--rate` controls Echo requests. All three commands stop at `--duration`, Ctrl+C, or
their applicable bounds.

Progress is written to stderr so stdout remains usable. The final human report is written to stdout.
JSON and CSV exports contain sender-side evidence and explicitly leave unsupported observations
unavailable; UDP never labels missing acknowledgements as packet loss.

## Source-address validation

`validate-source` is not a load profile. It emits one fixed, deterministic ICMP Echo probe format
from the explicitly configured source address. The command requires all of these conditions:

- the current process is in a dedicated network namespace, or the allowlisted interface is a
  virtual peer interface;
- the interface exists, is not loopback, and is named by both `--interface` and
  `--allow-interface`;
- source and destination are usable RFC 1918 host addresses in wholly RFC 1918 CIDR allowlists;
- `--spoof-source` and `--source-ip` are both present;
- effective `CAP_NET_RAW` is available; and
- rate, duration, and total probes do not exceed 10 PPS, 30 seconds, and 300 respectively.

The policy is checked before opening the raw socket and before every transmission. The engine has
no source randomization, TCP/UDP mode, reflection payload, or configurable packet body. A report
records actual interface and namespace, configured endpoints, attempts, successful local
transmissions, local errors, and any correlated ICMP observations. It does not interpret a local
send or missing local reply as firewall acceptance or rejection.
