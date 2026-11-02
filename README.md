# crawler

A simple web crawler on [anyhttp](https://github.com/pgit/anyhttp) (HTTP/1.1, HTTP/2 and HTTP/3) and
[c-ares-async](https://github.com/pgit/c-ares-async) (DNS).

> [!WARNING]
> This repository is purely experimental. It is not meant for production use, and anything in it
> can change or break at any time, without notice.


## What it does

```sh
crawler --allow example.com https://example.com/
```

It starts from the URLs given and follows links, one request at a time, breadth first. It refuses to
run without `--allow`, and only ever connects to the hosts named there, by their exact names. Each
host gets its `robots.txt` read first, and requests to it are at least `--delay-ms` apart, or further
if its `Crawl-delay` says so. `crawler` without arguments lists the options.

How to connect is up to the host's HTTPS records in DNS ([RFC 9460](https://www.rfc-editor.org/rfc/rfc9460)):
aliases are followed, a record has to be compatible, and the lowest priority goes first. The port
comes from the record, and an `http://` URL with a record is treated as `https://`, without going
back to cleartext.

What can be used of that is limited by what anyhttp's client does so far:

| The host has                              | What happens                                  |
|-------------------------------------------|-----------------------------------------------|
| a record with `h3` in its ALPN set        | HTTP/3, to the port of the record             |
| a record without `h3`                     | rejected: needs TLS over TCP                  |
| a record with a target that is not itself | rejected: can't verify one name and connect to another |
| no record, on an `https://` URL           | rejected: needs TLS over TCP                  |
| no record, on an `http://` URL            | HTTP/1.1 in the clear, to the addresses from c-ares |

Every rejection is printed with its reason. Over HTTP/3, the address comes from anyhttp's own resolver
and not from c-ares, so `--dns-server` and the address hints in the record do not apply to it.

The resolver in the devcontainer may answer with no data for HTTPS records, even where they exist
(see c-ares-async's [example](https://github.com/pgit/c-ares-async/blob/master/example/resolve/README.md)).
`--dns-server` is what gets around that.

To see it work without going anywhere, `demo_site` runs a site and a DNS server on loopback, and
prints the command to crawl them with:

```sh
./build/test/demo_site
```


## Build

C++23 and CMake 3.30 or later. The devcontainer in [.devcontainer](.devcontainer) has the compilers
and every dependency that the two libraries need. There are two presets:

| Preset   | Tree            | Libraries come from                                             |
|----------|-----------------|-----------------------------------------------------------------|
| `dev`    | `build/`        | the checkouts next to this one, as they are                     |
| `pinned` | `build-pinned/` | GitHub, at the commits pinned in [CMakeLists.txt](CMakeLists.txt); what CI builds |

```sh
cmake --preset pinned
cmake --build --preset pinned
ctest --preset pinned
```

Both presets use clang, libc++, OpenSSL and the Asio runtime.
