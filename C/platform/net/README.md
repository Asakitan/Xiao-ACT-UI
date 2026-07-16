# `platform/net`

Packet capture (Npcap), TCP reassembly pipeline, HTTPS + WSS clients,
TLS certificate pinning and URL helpers.  Everything the platform needs
to talk to the outside world — with no game knowledge baked in.

## Corresponding Python source

| C++ header            | Python source                                                 |
| --------------------- | ------------------------------------------------------------- |
| `npcap_capture.h`     | `plugins/star_resonance_plugin/net/packet_capture.py`         |
| `packet_pipeline.h`   | `plugins/star_resonance_plugin/net/packet_bridge.py`, `parser_adapter.py` |
| `http_client.h`       | ad-hoc `requests` calls in the launcher                       |
| `ws_client.h`         | not yet in Python (planned for cloud sync)                    |
| `tls.h`               | not yet in Python                                             |
| `url.h`               | scattered `urllib.parse` calls                                |

## Public headers

- `npcap_capture.h` — wpcap.lib wrapper + capture-worker callback surface.
- `packet_pipeline.h` — endpoint lock + TCP reassembly + application-frame delivery.
- `http_client.h` — blocking GET/POST for License + cloud config.
- `ws_client.h` — WSS client for the optional cloud channel.
- `tls.h` — SPKI-pin sets for the security-critical scopes.
- `url.h` — RFC 3986 parse + percent-encode.

## Phase plan

- **Phase 1 (this skeleton)** — headers + stub cpps.
- **Phase 3** — real Npcap load-time dispatch + capture worker; WinHTTP
  clients; SPKI pin verifier.  Concrete parsers live one directory up
  under `../plugins/star_resonance_plugin/`.

## Integration notes for Agent 6

`sao_net.dll` links `wpcap.lib` from Npcap.  Agent 6 needs to ensure
either:

1. Npcap SDK is on the include/link paths at build time (recommended:
   vendor via vcpkg overlay port), OR
2. The Phase 3 implementation uses runtime `LoadLibrary` on
   `wpcap.dll` so the exe still starts without Npcap installed.

Design here assumes option 2 so the launcher can surface a "please
install Npcap" hint instead of failing to boot.
