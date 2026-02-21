I am very happy because when you are reading this HACKING.md, it means you are going to adding some features or fix some bugs for this project. To quickly understand the program architecture (and save tokens for your coding LLM), I'll take some notes here to introduct it for you.

# Introduction

Let's start with this ASCII chart:

```
   +------------------+                      +-------------------+    
   |                  |                      |                   |    
   | reframe-streamer |                      | reframe-streamer  |    
   |                  |                      |                   |    
   |reframe/eDP-1.sock|                      |reframe/HDMI-1.sock|    
   |                  |                      |                   |    
   +---------^--------+                      +---------^---------+    
             |                                         |              
             |                                         |              
             |                                         |              
+------------+-------------+             +-------------+-------------+
|                          |             |                           |
|     reframe-server       |             |      reframe-server       |
|                          |             |                           |
|reframe-session/eDP-1.sock|             |reframe-session/HDMI-1.sock|
|                          |             |                           |
+-------^---------^--------+             +---------^---------^-------+
        |         |                                |         |        
        |         |                                |         |        
        |         |       +---------------+        |         |        
        |         |       |               |        |         |        
        |         +-------+reframe-session+--------+         |        
        |                 |               |                  |        
        |                 +---------------+                  |        
        |                                                    |        
        |                                                    |        
        |                 +---------------+                  |        
        |                 |               |                  |        
        +-----------------+reframe-session+------------------+        
                          |               |                           
                          +---------------+                           
```

The arrow from A to B means B creates and listens to UNIX socket, and A connects to the socket. It is easy to understand that `reframe-session` connects to `reframe-server`, but why `reframe-server` connects to `reframe-streamer`, not the contrary? Because we could let systemd handle the socket and only starts `reframe-streamer` when `reframe-server` really connects to it. If there is no VNC connections, it won't connect to the socket.

Normally sockets will be set to `0644` and `reframe:reframe`, so you could add yourself to `reframe` group, and run `reframe-server` without `sudo`.

I'll introduce them 1 by 1.

## reframe-server

This is where we handle remote desktop implementation. It is a VNC server runs as a special (by default it is the `reframe` user) user, listens to port and accepts network connection from VNC client. And then processes the following jobs:

- Decoding DMA-BUF fds from `reframe-streamer` into pixels via EGL/OpenGL ES and feeding them to VNC.
- Converting VNC input events to Linux input events and feeding them to `reframe-streamer`.
- Creating and listening to session sockets and sync clipboard text between VNC and user sessions.

Each `reframe-server` handles 1 monitor, so there might be many instances handling different monitors.

## reframe-streamer

This is where we handle privileged DRM and uinput jobs. It runs as `root`, grabs monitor DRM framebuffer and export it as DMA-BUF fds, writes Linux input events to uinput device. Because it is privileged, it should be slim and only do necessary things.

`reframe-streamer` creates (non-systemd) and listens to socket so `reframe-server` could connect to it. Each `reframe-streamer` is paired with 1 `reframe-server`, so there might be many instances.

Connection to `reframe-streamer` is handled in `reframe-server/rf-streamer.c`.

## reframe-session

This is where we handle user session jobs, because `reframe-server` and `reframe-streamer` are not running as normal user. It will run as XDG autostart to start with user's GUI sessions. Currently we only use it to sync clipboard text, because clipboard is per-user.

We have no way to pair user sessions with VNC connections, so `reframe-session` will connect to all sockets in the given directory, and talk with all `reframe-server` instances, if there is no socket currently, it will wait until sockets are created. There might be many `reframe-session` instances.

Connection to `reframe-session` is handled in `reframe-server/rf-session.c`.

# Authentication

Because `reframe-session` is running with user session, if we only check whether a process's GID is `reframe`, we cannot prevent other processes under the same user session.

We limit socket client via their executable binary, the kernel allows us to get socket peer PID, and if you have `CAP_SYS_PTRACE`, you could read `/proc/<pid>/exe`, it is a link to the process executable binary.

For `reframe-streamer`, it is easy because it could have `CAP_SYS_PRTACE`, so it will check whether its client is `reframe-server`. For `reframe-server`, it is hard because it does not have the permission. We have to pass client PID to `reframe-streamer`, and let `reframe-streamer` authenticate it by checking whether it is `reframe-session`, then pass result back. This is async IPC, so we will first put the connection into a pending list, and after getting result, we will decide whether drop the connection, or move it into connection list.

# Coding

This project follows [Linux kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html). There is a `.clang-format`, so you could run `clang-format --verbose -i reframe-{common,streamer,server,session}/*.{c,h}` to format all sources, it is suggested but not a must, because if you forget it, I'll do it.

While this project uses GLib, I prefer not to use [GLib basic types](https://docs.gtk.org/glib/types.html), because most of them are just aliases of C types, and I prefer newer C standard so just use C types. For example, use `size_t`, `unsigned char` and `void *` instead of `gsize`, `guchar` and `gpointer`.

`gboolean` might be hard to deal with, because it is not `bool` from `stdbool.h`. Replacing `gboolean` with `bool` directly leads into type mismatch and memory leak. However, we could always use `int` instead of `gboolean` for the type and use `true` and `false` instead of `TRUE` and `FALSE` for the value. For other conditions that not interacting with GLib, always prefer `bool` from `stdbool.h`.

When passing types as arguments, for example defining a signal, it is OK to use GLib types like `G_TYPE_INT`, we have no choice.

However, always prefer GLib variant of functions, because they add more checks to handle corner-cases than the standard version so we get more benefits. For example, use `g_malloc0`, `g_free` and `g_strcmp0` instead of `malloc`, `free` and `strcmp`.

Use `g_autofree`, `g_autoptr` and `g_auto` whenever is possible, because they reduce the burden of manually memory management.

When you are writing new functions, follow existing name and coding style. Use `static` for private methods, you don't need to add prefix for private methods, including underline, but like most GObject projects, add reasonable prefix for public methods. Put private methods at the top of a file, then GObject methods (`class_init` and `init`), then public methods. Use `this` as the first parameter name for methods and use `klass` as the first parameter name for class methods.

# Debugging

This program will load/link against libraries under installation prefix, so you may need to `meson install` them before running it, otherwise it may still load old files.

# Profiling

You could build it with `gprof` support by adding `-D c_args='-pg' -D c_link_args='-pg'` options to Meson.

# TODOs

The idea of clipboard text sync is inspired by qemu's `spice-vdagent` which also uses XDG autostart and GTK to implement it, `reframe-session` sets `GDK_BACKEND=x11` because Wayland does not allow normal clients to read/write clipboard without focus, it is not so good, but usable is the most important. We could add Wayland `data-control` implementation and (maybe) mutter implementation to make it better.

It might be possible to get virtual desktop size and monitor position in user session, however, I have no idea to match it with DRM connectors that `reframe-server` needs.
