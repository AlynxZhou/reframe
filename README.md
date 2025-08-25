ReFrame
=======

DRM/KMS based remote desktop for Linux that supports Wayland/NVIDIA/headless/login…
-----------------------------------------------------------------------------------

# WIP

I might rebase and force push master branch at any time!

# How to Debug

Different system may use different name for cards and connectors, find them via `/sys/class/drm/` and modify the configuration file to choose the available one.

```
$ mkdir build && cd build && meson setup --prefix=/usr --buildtype=debug . .. && meson compile
# setcap cap_sys_admin+ep reframe-streamer/reframe-streamer
$ G_MESSAGES_DEBUG=ReFrame ./reframe-streamer/reframe-streamer --config=../dists/camelot.conf --keep-listen
$ EGL_PLATFORM=surfaceless G_MESSAGES_DEBUG=ReFrame ./reframe-server/reframe-server --config=../dists/camelot.conf --debug-win
```

# TODO

- [X] Send DMA BUF fds via UNIX domain socket from streamer to server.
- [X] Send input events via UNIX domain socket from server to streamer.
- [ ] Use systemd socket to start streamer on demand, start streamer on first connection and stop streamer on last connection.
- [ ] Permission control with systemd unit, only streamer has SYS_CAP_ADMIN, others in normal users.
- [X] Accept config file as parameters so we can start different daemon for different connectors.
- [ ] Error handling.
- [ ] Free pointers on stopping.
