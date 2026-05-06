"""Generate `packet_state.json` from a captured pcap of Star.exe traffic.

The canonical Python pipeline:

    pcap → SharpPcap (live) / scapy (offline) → TcpReassembler
        → PacketParser.dispatch → GameStateManager.update(...)

This generator skips the live-capture step and walks the pcap with
`scapy.utils.PcapReader`, feeds the reassembled streams into the
canonical Python `packet_parser.dispatch_packet`, then dumps the
final `GameStateManager.snapshot()` through the canonical envelope.

Used by `SaoAuto.ParityTests/PacketStateParityTests.cs` (to land in
a later session) which feeds the same pcap into the C# port and
compares with `numericEpsilon = 0` (state is integer-valued).

Tape note: pcaps are too big to commit. `source` records sha256 +
captured_at so reviewers can track provenance without diffing bytes.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

from _common import add_common_args, import_canonical_runtime, write_fixture


def _sha256_short(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        type=Path,
        required=True,
        help="Captured pcap file (Star.exe traffic).",
    )
    parser.add_argument(
        "--server-port",
        type=int,
        default=10800,
        help="Game server TCP port (default: 10800).",
    )
    add_common_args(parser, "packet_state.json")
    args = parser.parse_args()

    if not args.input.exists():
        raise SystemExit(f"pcap not found: {args.input}")

    import_canonical_runtime()
    try:
        from scapy.utils import PcapReader  # type: ignore
        from scapy.layers.inet import TCP, IP  # type: ignore
    except ImportError:
        raise SystemExit(
            "scapy is required for offline pcap replay (`pip install scapy`)."
        )
    try:
        from sao_auto.packet_parser import dispatch_packet  # type: ignore
        from sao_auto.tcp_reassembler import TcpReassembler  # type: ignore
        from sao_auto.game_state import GameStateManager  # type: ignore
    except ImportError as e:
        raise SystemExit(f"canonical runtime missing: {e}")

    state = GameStateManager()
    reasm = TcpReassembler()
    n_packets = 0
    n_messages = 0

    with PcapReader(str(args.input)) as reader:
        for pkt in reader:
            if not (pkt.haslayer(TCP) and pkt.haslayer(IP)):
                continue
            tcp = pkt[TCP]
            if tcp.sport != args.server_port and tcp.dport != args.server_port:
                continue
            payload = bytes(tcp.payload)
            if not payload:
                continue
            n_packets += 1
            for msg in reasm.feed(
                src=(pkt[IP].src, tcp.sport),
                dst=(pkt[IP].dst, tcp.dport),
                seq=tcp.seq,
                payload=payload,
            ):
                dispatch_packet(msg, state)
                n_messages += 1

    snapshot = state.snapshot()
    write_fixture(
        args.output,
        kind="packet_state",
        source=f"{args.input.name} sha256={_sha256_short(args.input)} "
               f"packets={n_packets} messages={n_messages}",
        data=snapshot,
        force=args.force,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
