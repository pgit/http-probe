# crawler

A simple web crawler on [anyhttp](https://github.com/pgit/anyhttp) (HTTP/1.1, HTTP/2 and HTTP/3) and
[c-ares-async](https://github.com/pgit/c-ares-async) (DNS).

**It does not crawl anything yet.** The `crawler` program sets up both libraries on one event loop
and exits, which shows that they build and link into one process.

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
