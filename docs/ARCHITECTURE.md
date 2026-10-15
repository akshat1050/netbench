# NetBench architecture

NetBench is a single Linux CLI process. It initiates every test locally and never installs or
contacts a NetBench agent on the target. The target is an IPv4 literal that must match at least one
`--allow-cidr` supplied for the run.

```text
CLI parser + safety validation
             |
          selected engine
      /      |       |       \
   TCP      UDP     ICMP   source validation
      \      |       |       /
  thread-safe metrics + monotonic timestamps
             |
 terminal progress + terminal/JSON/CSV final report
```

## Safety boundary

Every run is bounded by requested values and independently by runtime maximums for packets per
second, bandwidth, connection rate, established concurrency, and duration. Runtime maximums cannot
exceed compiled ceilings. Burst profiles are additionally limited to 10 seconds. General source
spoofing, raw arbitrary packets, reflection helpers, target authentication, and receiver services
are outside the design. The sole exception is the isolated `validate-source` engine described
below.

Profiles never exceed the requested rate:

- `baseline`: 10% of the requested rate, with a floor of one operation per second.
- `sustained`: the requested rate for the bounded duration.
- `ramp`: linear growth from 10% to 100% over the duration.
- `burst`: the requested rate for at most 10 seconds.

Where a response exists, engines take a small probe before load, measure responses during load, and
probe again during the configured recovery window. An unavailable observation is reported as such;
it is not converted into receiver-side loss.

## Engines

### TCP

Nonblocking sockets are opened at the configured connection rate and retained up to the concurrency
limit until the run ends. `epoll` observes completion, timeout, reset, and other socket errors.
Connection latency is measured from `connect()` initiation to `SO_ERROR == 0`. Linux `TCP_INFO`
provides retransmission counts when available. No bytes are sent after establishment.

### UDP

A nonblocking connected UDP socket submits fixed-size datagrams under both PPS and bandwidth
limits. Successful `send()` calls are sender submissions, not proof of delivery. Linux `IP_RECVERR`
error-queue entries provide ICMP destination-unreachable feedback when the kernel exposes it.

### ICMP

The engine sends ICMP Echo requests using a Linux ping socket, falling back to a raw ICMP socket
when permitted. Sequence numbers correlate replies with monotonic send timestamps. Only this engine
reports packet loss, derived from transmitted requests without matching replies by their timeout.

### Source validation

`SourcePolicy` restricts both endpoints and their CIDRs to RFC 1918, requires exact interface and
CIDR allowlists, and enforces immutable ceilings of 10 PPS, 30 seconds, and 300 probes.
`LabEnvironment` verifies the interface, network namespace or virtual-peer isolation, and effective
`CAP_NET_RAW`. The engine then sends a fixed ICMP Echo packet with deterministic identifiers and
revalidates the policy and environment before each send. It records sender submission separately
from correlated local ICMP observations and never infers forwarding or drop behavior without an
external observer.

## Concurrency and ownership

RAII owns all file descriptors. Each engine runs in a `std::jthread` controlled by a stop token;
its sockets are nonblocking and multiplexed by `epoll`. Counters are atomic and latency samples are
mutex-protected so progress reporting can safely take snapshots.

