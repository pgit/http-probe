# Working on http-probe

It requests `https://<host>/` once per host and reports what came back, connecting the way the
host's HTTPS records say. It is built on two libraries of the same author, each in a repository of
its own: anyhttp for HTTP and c-ares-async for DNS.
[README.md](README.md) has the presets and the build commands.

## Safety rules

These hold for all code in this repository, including tests and experiments.

- **One request per host, and nothing more.** c-ares-async's lists of hosts are fine as input here,
  which is what makes this rule matter: they have 1000 mixed sites, some adult or spam-looking. So
  nothing is followed: no links, no redirects and no Alt-Svc. Another route is for a connection that
  failed: once a request has gone out, that was it for the host, however it ended.
- **Tests connect to local servers on loopback only.**
- **If the program ever crawls, it refuses to run without an explicit host allowlist,** with
  robots.txt and per-host rate limits built in from the start, not added later.
- **Run tests and programs under `timeout -s KILL`**, 5 seconds per test. Never let a suite sit on a
  hang.

## The code

[routes.cpp](src/routes.cpp) says what the HTTPS records of a name mean for how to connect, without
any I/O, and [prober.cpp](src/prober.cpp) does the rest. The tests in [test/](test) run against
[Site](test/site.hpp), a real anyhttp server and a DNS server with a zone of its own, on loopback.

## The libraries

- Changes to a library belong in that library's repository and session, with a test in its suite.
  Do not work around a library bug in this code: get it fixed there, then move the pin.
- Ask for a library change with an issue in that library's repository, using `gh`. Peter reviews the
  full text first, and the same goes for comments on issues and for closing them. The repositories
  are public, so reproduce on loopback and name no hosts from the lists.
- Moving a pin in [CMakeLists.txt](CMakeLists.txt) is a commit of its own. CI builds from the pins
  only, so a change here that needs a library change has to wait for that change to be pushed.
- The `dev` preset builds the sibling checkouts as they are, on whatever branch they are on, and can
  differ from the pins. Check with the `pinned` preset before committing.
- Everything in one process has to agree on the standard library, the TLS library and the I/O
  runtime. [CMakeLists.txt](CMakeLists.txt) decides these for both libraries, so change them there.
  Do not configure a tree by hand: `cc` and `c++` are GCC on this host, and a tree without clang
  silently gets the wrong compiler.

## Style

As in anyhttp: clang-format with 3-space indent and 100 columns, members `foo_`, functions
`snake_case`, types `CamelCase`. Commit subjects are prefixed `fix:`, `refactor:`, `docs:`, `chore:`
or `test:`.

Commit or push only when asked. Push over SSH to git@github.com; Peter opens the pull requests.
