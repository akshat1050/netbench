# NetBench CLI specification

```text
netbench tcp  --target IPv4 --allow-cidr CIDR --port PORT
              --connections N --rate Ncps --duration TIME [options]
netbench udp  --target IPv4 --allow-cidr CIDR --port PORT
              --pps N --size BYTES --duration TIME [options]
netbench icmp --target IPv4 --allow-cidr CIDR
              --rate Npps --duration TIME [options]
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
