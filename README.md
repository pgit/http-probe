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
and requests `https://<host>/` once, over TLS, in the way that the records say. It prints the status,
the route, the IP address it connected to, the time it took and the header fields of the response,
along with the host's records. A redirect is reported and not followed, and nothing else on the host
is requested.

There is output for every host but those that do not exist, which `--all` adds. The counts at the end
look like this, on stderr:

```
190 answered, 1 failed, 3 out of reach, 12 not found, 2 lookups failed
```

| The host                                      | Outcome                                     |
|-----------------------------------------------|---------------------------------------------|
| has a record on its own name                  | answered or failed, by the protocols and the port of the record |
| has no record, or none that is compatible     | answered or failed, over TCP on port 443, HTTP/2 or HTTP/1.1 as the server likes |
| has records for other targets only, or an alias to another name | out of reach, not connected to: anyhttp can't connect to one name and verify another |
| has a record that says the service is not available | out of reach, not connected to        |
| does not exist                                | not found, not connected to                 |
| gets no answer from DNS, or has no address    | lookup failed, so there is no telling, not connected to |

Aliases are followed, a record has to be compatible, and the lowest priority goes first. Within a
record, HTTP/3 goes first, and then TCP on the same port: HTTP/2 if the record has `h2`, otherwise
HTTP/1.1, which is in every record that does not say `no-default-alpn`. The route in the output is
what the server went for, which may be HTTP/1.1 where HTTP/2 was offered.

**One request means one.** The next route is for when connecting failed. Once a request has gone out,
the host has been asked, so that a timeout or a response cut short ends it there, as a failure.

The address comes from c-ares, so `--dns-server` applies to it, and `/etc/hosts` as well. The address
hints in a record do not. Addresses are tried one at a time, in the order c-ares sorts them, within
the timeout of the route, until one of them can be connected to. A failure says which address it was
on. Certificates are always verified, against the host's name.

**To see what is going on,** `-v` logs to stderr: the requests and responses from anyhttp, the queries
from c-ares and the steps in between. `-vv` adds all of QUIC. Hosts run at the same time mix in the
log, so `--concurrency 1` helps. Colours are for terminals only unless there is `--color`, except that
anyhttp puts them into some of its log messages regardless.

**Use `--dns-server`.** The resolver in the devcontainer answers with no data for HTTPS records, even
where they exist, which looks just like no host having any.

**The program can take a while to end.** If a host did not answer over HTTP/3, anyhttp goes on sending
to it for 30 seconds after the timeout. The counts are printed when the work is done, ahead of that.

**A response goes to memory as a whole,** as anyhttp has no way of getting at it piece by piece with a
GET yet. So only `--timeout-ms` limits how large one can get.

To see it work without going anywhere, `demo_site` runs a site and a DNS server on loopback, and
prints the command to request it with:

```sh
./build/test/demo_site
```


## Build

C++23 and CMake 3.30 or later. The devcontainer in [.devcontainer](.devcontainer) has the compilers
and every dependency that the two libraries need. The libraries themselves are fetched from GitHub,
at the commits pinned in [CMakeLists.txt](CMakeLists.txt):

```sh
cmake --preset pinned
cmake --build --preset pinned
ctest --preset pinned
```

The tree is `build/`, with clang, libc++, OpenSSL and the Asio runtime.
