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

# Coding

There is a `.clang-format`, so you could run `clang-format -i *.c *.h` in each source directories, it is suggested but not a must, because if you forget it, I'll do it.

# TODOs

The idea of clipboard text sync is inspired by qemu's `spice-vdagent` which also uses XDG autostart and GTK to implement it, `reframe-session` sets `GDK_BACKEND=x11` because Wayland does not allow normal clients to read/write clipboard without focus, it is not so good, but usable is the most important. We could add Wayland `data-control` implementation and (maybe) mutter implementation to make it better.

It might be possible to get virtual desktop size and monitor position in user session, however, I have no idea to match it with DRM connectors that `reframe-server` needs.
