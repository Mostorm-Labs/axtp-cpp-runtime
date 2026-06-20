# Third-party Dependencies

The directories in this folder are Git submodules. The superproject pins exact
commits; the version column records the upstream release tag or version string
used to choose that commit.

| Path | Repository | Version | Commit |
|---|---|---|---|
| `third_party/IXWebSocket` | `https://github.com/machinezone/IXWebSocket.git` | `v11.4.6` | `2efe037c9cc96fd536774f17bdb5215161ee5087` |
| `third_party/asio` | `https://github.com/chriskohlhoff/asio.git` | `asio-1-28-2` | `7609450f71434bdc9fbd9491a9505b423c2a8496` |
| `third_party/hidapi` | `https://github.com/libusb/hidapi.git` | `0.16.0` | `c3509c11174fe80ff59a47119433a7db5299af85` |
| `third_party/json` | `https://github.com/nlohmann/json.git` | `v3.11.3-371-gd8ebaf61` | `d8ebaf61d79512e3feea44c69128dea82eff59d9` |
| `third_party/websocketpp` | `https://github.com/zaphoyd/websocketpp.git` | `0.8.2` | `56123c87598f8b1dd471be83ca841ceae07f95ba` |

`hidapi` did not have a `hidapi-0.16.0` release tag in the upstream tag list
when this pin was added. The pinned commit is the exact upstream tree that
matches the previously vendored `0.16.0` sources.
