# DSO-PSIRP Browser

Content-centric networking stack: **PSIRP + DSO deterministic task graphs + GTK4/WebKitGTK browser**

Internet organized by **content name**, not IP address.

## Architecture

```
                    ┌─────────────────────────────────────────┐
                    │           DSO-PSIRP Browser             │
                    │      (GTK4 + WebKitGTK 6.0)             │
                    └─────────────┬───────────────────────────┘
                                  │ fetch(/site/page.html)
                                  ▼
                    ┌─────────────────────────────────────────┐
                    │         DSO Task Graph                   │
                    │  fetch → parse → resolve → render        │
                    │  (WCET contracts, deterministic)         │
                    └─────────────┬───────────────────────────┘
                                  │
                    ┌─────────────▼───────────────────────────┐
                    │           PSIRP Layer                    │
                    │  Content naming: /site/page.html         │
                    │  Interest/Data packets                   │
                    │  Arena-backed content store               │
                    └─────────────┬───────────────────────────┘
                                  │
              ┌───────────────────┼───────────────────┐
              ▼                   ▼                   ▼
        ┌──────────┐       ┌──────────┐       ┌──────────┐
        │  Peer A  │◄─────►│  Peer B  │◄─────►│  Peer C  │
        │ /games   │       │ /music   │       │ /docs    │
        └──────────┘       └──────────┘       └──────────┘
              │                   │                   │
              └───────────────────┴───────────────────┘
                    Mesh Overlay (UDP multicast)
```

## Features

- **Content by name** — no DNS, no CDN, no single point of failure
- **Deterministic fetching** — DSO guarantees WCET for each pipeline stage
- **HTML parsing** — sub-resource extraction (CSS/JS/images)
- **Mesh overlay** — multi-peer FIB routing, multicast discovery
- **DHT content discovery** — Kademlia-style (XOR) routing maps content hash → holder peers; FIB falls back to DHT when no explicit route exists
- **Ed25519 signing** — real asymmetric content signing/verification (libsodium), tamper-evident end-to-end
- **Arena-backed content store** — zero-alloc, cache-friendly
- **GTK4 browser** — content-name address bar (`/site/page`)

## Quick Start

### Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Run Publisher

```bash
./publisher 9700 ./content
```

### Fetch Content

```bash
./client /test/page.html 127.0.0.1 9700
```

### Run Browser

```bash
./dso-browser
# Enter content name: /test/page.html
```

### Run Mesh Demo (3 peers)

```bash
# Terminal 1
./mesh_demo /games 9700 ./content/games

# Terminal 2
./mesh_demo /music 9701 ./content/music

# Terminal 3
./mesh_demo /docs  9702 ./content/docs
```

Mesh commands:
```
games> fetch /music/song.mp3     # Fetch from peer
games> publish /games            # Announce prefix
games> peers                     # List connected peers
games> fib                       # Show routing table
games> beacon                    # Send beacon
```

## Components

| Component | Description |
|-----------|-------------|
| `psirp.c/h` | Content naming, Interest/Data packets, content store, FIB |
| `net.c/h` | UDP transport, multicast discovery, blocking fetch |
| `html_parse.c/h` | HTML parser — extracts CSS/JS/images from `<link>`, `<script>`, `<img>` |
| `dso_psirp.c/h` | DSO task graph integration with WCET contracts |
| `mesh.c/h` | Mesh overlay — peer discovery, FIB routing, beacons |
| `browser.cpp` | GTK4 + WebKitGTK 6.0 browser with content-name addressing |
| `publisher.cpp` | Content server — serves files by PSIRP names |
| `client.cpp` | CLI content fetcher |
| `mesh_demo.c` | Interactive mesh demo with 3+ peers |

## Content Naming

Content is addressed by hierarchical names:

```
/site/page.html
/site/css/style.css
/games/asteroids/level1.data
/music/song.mp3
```

### URL Resolution

Relative URLs are resolved against the base:

| Base | Relative | Resolved |
|------|----------|----------|
| `/site/page.html` | `style.css` | `/site/style.css` |
| `/site/page.html` | `/other.css` | `/other.css` |
| `/site/page.html` | `http://cdn/x.js` | `/cdn/x.js` |

## DSO Task Graph

Fetching is deterministic — each stage has WCET contract:

```
fetch_html (4s WCET)
    │
    ▼
parse_html (1ms)
    │
    ├──────────────────┐
    ▼                  ▼
fetch_css (2ms)   fetch_images (5ms)
    │                  │
    └──────────────────┘
           │
           ▼
      render (16ms @ 60fps)
```

## Testing

```bash
cd build
./test_psirp       # 10/10 — naming, serialization, CS, FIB, network
./test_html_parse  # 9/9 — CSS/JS/image extraction, URL resolution
./test_mesh        # 6/6 — peer management, FIB, prefix matching
```

## Dependencies

- **DSO Runtime** — `/home/lain/dso-runtime/` (libdso.a)
- **GTK4** — 4.22+
- **WebKitGTK** — 6.0 (webkitgtk-6.0)
- **libsodium** — Ed25519 signing (crypto_sign_ed25519)
- **CMake** — 3.20+
- **pthreads**

## DHT & Content Discovery

`dht.c` implements a Kademlia-style distributed hash table keyed by the
64-bit PSIRP name hash (XOR distance metric). It maps `content_hash →
holder peers` and does **not** store content itself. Resolution:

1. Compute name hash `H` of the requested content.
2. `dht_lookup(H)` returns the k closest known holders in XOR space.
3. Interest is sent directly to those holders (FIB fallback when no route).
4. When a peer serves content locally it announces itself to the DHT
   (`dht_announce`) so future requesters can find it.

This removes the need for a static FIB: content is locatable by name alone,
across an unstructured mesh.

## Dynamic Content

Static files are trivially cacheable, but real sites change (DB-backed
pages, feeds, counters). PSIRP handles this with four mechanisms:

- **Versioned names** — `/news@42` addresses a specific version; `/news`
  (no `@version`) resolves to the *newest* stored version. Caching stays
  correct because each version is content-addressable.
- **Pub/Sub** (`pubsub.c`) — publishers announce new versions into the DHT;
  subscribers watch a topic and pull `/topic@<newest>`. This is how a
  "dynamic" site becomes a sequence of cacheable, signed snapshots.
- **Chunking** (`psirp_cs_store_chunked`) — content > 64 KB is split into
  64 KB chunks (each content-addressable, dedup by hash) plus a manifest.
  Reassembly is transparent via `psirp_cs_lookup_chunked`.
- **Compute-on-peer** (`compute.c`) — for truly live rendering, a request
  carries parameters and an executor peer runs a registered handler (DSO
  task graph) and returns a **signed** result. The requester verifies the
  signature, not the transport.
- **Smart cache** — the content store is size-limited and LRU-evicts by byte
  budget (`psirp_cs_set_budget`), so large HTML/media don't blow up disk.

## Security

Content is signed with **Ed25519** (`signing.c`, libsodium). Each published
object carries `[signature(64) | public_key(32) | content_len(4) | content]`.
A receiver verifies the signature against the author's public key before
accepting the content — tampering or a wrong key is rejected. The secret key
is the libsodium expanded 64-byte form. For the PS Vita port, define
`PSIRP_SIGN_REF10` and supply a tiny ref10 implementation; the API is unchanged.

Known gaps (not yet implemented): peer authentication on the wire, transport
encryption (DTLS/noise), and trust/reputation layers. The content-integrity
layer is in place; transport security is the next step.

## How It's Different

| Traditional Internet | DSO-PSIRP |
|---------------------|-----------|
| GET `https://cdn.example.com/video.mp4` | Fetch `/site/video.mp4` |
| DNS → IP → CDN → Server | Name → DHT/FIB → Peer |
| HTTP/1.1 or HTTP/2 | Interest/Data (binary) |
| No determinism | WCET contracts |
| Client-server | Peer-to-peer mesh |
| Cache via CDN | Content store (in-network) |
| TLS endpoint auth | Ed25519 content signing |

## License

MIT
