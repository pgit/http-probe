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
http-probe --dot --dns-server one.one.one.one cloudflare.com
```

It looks up the HTTPS records of each host in DNS ([RFC 9460](https://www.rfc-editor.org/rfc/rfc9460)),
and requests `https://<host>/` once, over TLS, in the way that the records say. It prints the status,
the route, the IP address it connected to, the time it took and the header fields of the response,
along with the host's records. A redirect is reported and not followed, and nothing else on the host
is requested. [Here is what that looks like](doc/README.md).

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
| offers none of the protocols of `--h3`, `--h2` and `--h1`, with any of them | left out, not connected to |
| could be connected to, with `--dns-only`      | resolved, not connected to                  |
| could be connected to, with `--each-address`  | answered if all of its addresses did, otherwise failed |

Aliases are followed, a record has to be compatible, and the lowest priority goes first. Within a
record, HTTP/3 goes first, and then TCP on the same port: HTTP/2 if the record has `h2`, otherwise
HTTP/1.1, which is in every record that does not say `no-default-alpn`. The route in the output is
what the server went for, which may be HTTP/1.1 where HTTP/2 was offered.

**With `--h3`, `--h2` or `--h1`,** only the protocols given are spoken, in any combination, and all
three without any of them. HTTP/3 needs a compatible record with `h3` in its `alpn`. HTTP/2 and
HTTP/1.1 need a record that offers them, or no compatible record at all, which means port 443. A host
that offers none of the protocols given, on whichever target, is left out after its HTTPS lookup,
printed only with `--all` and with a note on why, and counted at the end as `5 left out`. The others
are treated as above, with the routes of those protocols only, so that one can still end up out of
reach. With `--h3` alone, that means no going over TCP when HTTP/3 can not be connected to.

anyhttp offers HTTP/1.1 along with HTTP/2, and a server may take it, or not use ALPN at all, which
comes to the same. With `--h2` but not `--h1`, such a server is not sent the request. That counts as a
connection that failed, so the next address or route gets its turn.

**With `--dns-only`,** it stops at the point where it would connect, so that no host is connected to
or requested. A host that would have been goes by `resolved` and comes with its records and all of
its addresses, in the order they would be tried:

```
example.com: resolved
  record: HTTPS 1 . alpn=h3,h2
  address: 2001:db8::1
  address: 192.0.2.1
```

All other hosts get the outcome they always get, which leaves `answered` and `failed` at 0. The addresses
are only looked up for a host that would have been connected to, so an `out of reach` one has none.
The counts at the end then add how many there were, as `5 resolved`, and `--h3`, `--h2` and `--h1`
work as above.

**With `--each-address`,** every address of a host gets a request of its own instead of only the first
that can be connected to, one after the other, with a line per address for the status line of the
response or what went wrong:

```
example.com: answered
  record: HTTPS 1 . alpn=h3,h2
  address: 2001:db8::1 200 OK
  address: 192.0.2.1 200 OK
```

All other hosts get the outcome they always get. A host counts as `answered` only if all of its
addresses did, whatever the status, and otherwise as `failed`. This is the one exception to one request
per host, and it can't go with `--dns-only`. The addresses are all asked at the same time, so a host
takes as long as its slowest address, and they are printed in the order of trying them all the same.
`--timeout-ms` then is per address on each route. Beyond
that, the next paragraph holds for each address, and nothing is followed here either. HTTP/2 and HTTP/3
have no reason phrase, so there the `OK` is what that status is called.

**One request means one.** The next route is for when connecting failed. Once a request has gone out,
the host has been asked, so that a timeout or a response cut short ends it there, as a failure.

The address comes from c-ares, so `--dns-server` applies to it, and `/etc/hosts` as well. The address
hints in a record do not. Addresses are tried one at a time, IPv6 first and otherwise in the order
c-ares sorts them, within the timeout of the route, until one of them can be connected to. So an IPv6
address that does not answer at all takes the whole timeout of the route, and IPv4 does not get a turn.
A failure says which address it was on. Certificates are always verified, against the host's name.

**To see what is going on,** `-v` logs to stderr: the steps of the program and the queries from c-ares.
`-vv` adds all that anyhttp has to say, QUIC included, with a line like `--- 42 ---` after each turn of
the event loop, in red if it took 10 ms or more, to show which turn a log line belongs to. Hosts run at
the same time mix in the log, so `--concurrency 1` helps. Colours are for terminals only unless there
is `--color`, except that anyhttp puts them into some of its log messages regardless.

**Use `--dns-server`.** The resolver in the devcontainer answers with no data for HTTPS records, even
where they exist, which looks just like no host having any. It takes addresses and host names, with a
port behind either. A name is looked up in the clear with the servers of the system, whatever the
queries proper go out as, and only its first address becomes a server.

**`--dot` asks over DNS over TLS** ([RFC 7858](https://www.rfc-editor.org/rfc/rfc7858)), on port 853
unless the server says otherwise, for the records and the addresses alike. The certificate of the
server needs a name to be checked against. That is the host name in `--dns-server`, and with an
address, or several names, `--tls-hostname` has to give it. `--tls-no-verify` skips the check, which
is for testing and has nothing to do with the certificates of the hosts. A failed handshake makes the
hosts lookup failures, and says why in the log. `--ca-file` is for the hosts as well: the certificate
of the DNS server goes by the trust store of OpenSSL, which `SSL_CERT_FILE` can change.

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

The tree is `build/`, with clang, libc++, OpenSSL and Capy and Corosio as the runtime.
