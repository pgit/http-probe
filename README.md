# http-probe

Requests hosts the way their HTTPS records say, on [anyhttp](https://github.com/pgit/anyhttp) (HTTP) and
[c-ares-async](https://github.com/pgit/c-ares-async) (DNS).

> [!WARNING]
> This repository is purely experimental. It is not meant for production use, and anything in it
> can change or break at any time, without notice.


## What it does

```sh
http-probe --dns-server 1.1.1.1 cloudflare.com
http-probe --dns-server 1.1.1.1 --file hostnames.txt
```

It looks up the HTTPS records of each host in DNS ([RFC 9460](https://www.rfc-editor.org/rfc/rfc9460)),
and connects to those that offer HTTP/3. From each of them it requests `https://<host>/` once, and
prints the status, the route, the time it took and the header fields of the response, along with the
host's records. A redirect is reported and not followed, and nothing else on the host is requested.

HTTP/3 is what it takes because anyhttp's client has TLS on HTTP/3 only, so far. There is a line of
output for every host that offers it and every lookup that failed, and `--verbose` adds the others.
The counts at the end look like this, on stderr:

```
12 answered, 1 failed, 0 with HTTP/3 out of reach, 180 without HTTP/3, 2 lookups failed
```

| The host                                      | Outcome                                     |
|-----------------------------------------------|---------------------------------------------|
| has a record with `h3` in its ALPN set        | answered or failed, to the port of the record |
| has that only on a target that is not itself  | out of reach: anyhttp can't connect to one name and verify another |
| has records without `h3`, or none at all      | without HTTP/3, not connected to            |
| does not exist                                | without HTTP/3, not connected to            |
| gets no answer from DNS                       | lookup failed, so there is no telling       |

Aliases are followed, a record has to be compatible, and the lowest priority goes first. Over HTTP/3,
the address comes from anyhttp's own resolver and not from c-ares, so `--dns-server` and the address
hints in the record do not apply to it.

**Use `--dns-server`.** The resolver in the devcontainer answers with no data for HTTPS records, even
where they exist, which looks just like no host having any.

**The program can take a while to end.** If a host with HTTP/3 did not answer, anyhttp goes on sending
to it for 30 seconds after the timeout. The counts are printed when the work is done, ahead of that.

To see it work without going anywhere, `demo_site` runs a site and a DNS server on loopback, and
prints the command to request it with:

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
