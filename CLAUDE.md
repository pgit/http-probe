# Working on the crawler

A web crawler on two libraries of the same author, each in a repository of its own: anyhttp for HTTP
and c-ares-async for DNS. [README.md](README.md) has the presets and the build commands.

## Safety rules

These hold for all code in this repository, including tests and experiments.

- **Never open, fetch or connect to anything in c-ares-async's `url_list.txt`, and never use it as
  input.** It is for DNS lookups only: 1000 mixed sites, some adult or spam-looking. The crawler is
  the first program here that could turn those names into connections.
- **Tests crawl local servers on loopback only.**
- **Once the program crawls, it refuses to run without an explicit host allowlist.** robots.txt and
  per-host rate limits are built in from the start, not added later.
- **Run tests and programs under `timeout -s KILL`**, 5 seconds per test. Never let a suite sit on a
  hang.

## The libraries

- Changes to a library belong in that library's repository and session, with a test in its suite.
  Do not work around a library bug in crawler code: get it fixed there, then move the pin.
- Moving a pin in [CMakeLists.txt](CMakeLists.txt) is a commit of its own. CI builds from the pins
  only, so a crawler change that needs a library change has to wait for that change to be pushed.
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

Commit or push only when asked. `gh`'s token is read-only, so push over SSH to git@github.com; Peter
opens the pull requests.
