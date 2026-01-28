ReFrame Remote Desktop
======================

DRM/KMS based remote desktop for Linux that supports Wayland/NVIDIA/headless/login…
-----------------------------------------------------------------------------------

# Features

If you are interested in contribution, you may read [HACKING.md](./HACKING.md) first which should be helpful.

## What ReFrame Currently Supports

- **VNC**
- **Wayland**/X11/**TTY**
- Intel/AMD/**NVIDIA**/[Even **Raspberry Pi**!](https://reframe.alynx.one/#rpi)/[Enter more GPUs that can run a general Wayland compositor…]
- Pointer/Keyboard
- **Remote Login**
- Headless Setup
- Draw Remote Cursor

## What ReFrame May Support in Future

- **RDP**: PRs are always welcome.

## What ReFrame Won't Support

- **Sound**: VNC does not have sound support.
- **No GPU/connector/EGL/OpenGL ES/DRM/KMS**: Even llvmpipe should work.
- **Game Streaming**: VNC is not optimized for low-latency. Consider game streaming optimized solutions like [Sunshine](https://github.com/LizardByte/Sunshine/).
- **DRM Overlay Plane**: Currently only a few compositors support overlay plane as an experimental feature, mostly for passing video frames to overlay plane so they don't need to decode and composite those frames and let hardware deal with those frames to reduce power consumption. However handling such overlay planes in ReFrame only means moving such decoding and compositing operations from compositor to ReFrame (and due to natural limitations it is not reliable), thus you get no benefits. So you could just disable overlay plane support in your compositor.

# Requirements

A GPU that:

- supports **DRM/KMS**.
- has a proper **EGL/OpenGL ES** implementation.
- is able to output to monitors.

If you can run a modern Linux desktop environment, it is likely you already meet these requirements.

# Install

## Distribution Package (Recommended)

### Arch Linux

#### Install From [AUR](https://aur.archlinux.org/packages/reframe/)

```
$ paru reframe
```

Or use other AUR helpers.

#### Install From `archlinuxcn`

First [add archlinuxcn repo to your system](https://www.archlinuxcn.org/archlinux-cn-repo-and-mirror/).

```
# pacman -S reframe
```

### openSUSE

#### Install from OBS

Packages can be found in [my OBS project](https://build.opensuse.org/package/show/home:AZhou/reframe/).

```
# zypper ar https://download.opensuse.org/repositories/home:/AZhou/openSUSE_Tumbleweed/home:AZhou.repo
# zypper in reframe
```

Leap users please replace URL for Tumbleweed with URL for your Leap version.

## Build from Source

### Dependencies

- glib2
- libepoxy
- libvncserver
- libxkbcommon
- libdrm
- systemd (optional & recommended)
- meson
- ninja
- gcc

If you want to build the experimental neatvnc implementation (`-D neatvnc=true`), you will also need the following dependencies that neatvnc requires:

- neatvnc
- aml
- pixman
- zlib
- ffmpeg

### Build

```
$ git clone --recurse-submodules https://github.com/AlynxZhou/reframe.git
$ cd reframe
$ mkdir build && cd build && meson setup --prefix=/usr . .. && meson compile
# meson install
```

# Usage

> **Security Suggestion**: VNC data streams are not encrypted even with password authentication, so **NEVER** expose this to public network directly! Connecting to it via VPN is recommended.

1. Run `systemctl start reframe-server@example.service`.
2. Try connecting to it with a VNC client via port `5933`.

If you have only 1 connected monitor and you never rotate it, it should work out of the box without modifying the example configuration. If it cannot find your monitor, you need to manually select monitor via DRM card and connector. If you can see screen content, but cannot control it, you need to manually load `uinput` kernel module (`modprobe uinput`).

## Select Monitor via DRM Card and Connector

1. Find your DRM card name (e.g., `card0`) and connector name (e.g., `DP-1`) in `/sys/class/drm/`.
2. Copy and modify the example configuration.
	```
	# cp /etc/reframe/example.conf /etc/reframe/DP-1.conf
	```
3. Set `card` and `connector` values.
4. Set the value of `rotation` to the angle you rotate your monitor.

If you cannot resize your VNC client window, or you don't want to resize the VNC client window manually every time, you can set values of `default-width` and `default-height`.

Then start the ReFrame Server systemd service so it will listen to VNC clients.

```
# systemctl start reframe-server@DP-1.service
```

ReFrame Server systemd service should automatically pull ReFrame systemd socket, which will trigger the privileged ReFrame Streamer systemd service on demand. If not, start the ReFrame systemd socket manually.

```
# systemctl start reframe@DP-1.socket
```

## Multi-monitor

If you have more than 1 monitors, you need to set the size of the whole virtual desktop and the position offset of your selected monitor to make mouse position mapping work.

Unfortunately there is no general way to get those values for all desktop environments. You could run a program to get the current cursor position, and then move the cursor to get the following values.

- `desktop-width`: The x coordinate of right border of your rightmost monitor.
- `desktop-height`: The y coordinate of bottom border of your bottommost monitor.
- `monitor-x` and `monitor-y`: The x and y coordinates of top-left corner of your selected monitor.

You need to keep the same multi-monitor layout for **both user session and display manager session** to make remote login work correctly.

## Headless Setup

This program only works with connected monitors, however if you have no monitor ("headless"), you can still use it, because Linux kernel could force enable a connector to pretend there is a monitor.

1. **Choose an unused connector**: For example add kernel parameter `video="DP-1:D"` to enable `DP-1`.
2. **Set resolution via EDID override**: You can dump your real monitor's EDID from `/sys/class/drm/card*-*/edid` or download a virtual one, put it to `/lib/firmware/edid/`, for example `/lib/firmware/edid/1280x720.bin`, and then add kernel parameter `drm.edid_firmware="DP-1:edid/1280x720.bin"` to use it. **If you have early KMS, also add those EDID binaries to your initramfs.**

Then you can reboot your system, and come back to modify the configuration.

## Automatic Wakeup

Desktop environments may turn off monitors if automatic screen blank is enabled, and ReFrame by default will try to wake system up by moving pointer a little bit so it could get monitor content.

This will introduce a ~2s delay before you can see monitor content, because your system needs some time to process the input event.

If you don't want this, you can disable automatic screen blank for **both user session and display manager**, then set `wakeup=false` in your configuration file.

## Clipboard Text Sync

You need to add your user to `reframe` group to use clipboard text sync.

```
# gpasswd -a USER reframe
```

If you set other username than `reframe` while building, the group name should be changed accordingly.

This is implemented by putting a desktop file to XDG autostart dir to start `reframe-session` with your session and handle your clipboard. `reframe-session` talks with `reframe-server` via sockets in directory `/run/reframe-session`, if you are not using systemd, or you changed the directory via `reframe-server`'s argument, don't forget to modify the XDG autostart file to change argument for `reframe-session`

# Comparison with Other Linux Remote Desktop

- [kmsvnc](https://github.com/isjerryxiao/kmsvnc/)
	+ ReFrame supports NVIDIA driver by decoding frames with EGL and OpenGL ES, kmsvnc uses VA-API and it cannot decode frames of NVIDIA driver correctly.
	+ ReFrame gets frame buffer on each frame, so it works correctly if your compositor uses double-buffers.
	+ ReFrame contains privilieged DRM/uinput process and unprivilieged VNC server process, kmsvnc runs as a whole privilieged process.
	+ ReFrame supports resizing client window.
- [RustDesk](https://github.com/rustdesk/rustdesk/)
	+ ReFrame supports Remote Login on Wayland, [RustDesk does not support it](https://rustdesk.com/docs/en/client/linux/#login-screen).
- [GNOME Remote Desktop](https://gitlab.gnome.org/GNOME/gnome-remote-desktop/)
	+ ReFrame supports non-GNOME desktop environments.
	+ ReFrame supports Remote Login with VNC, GNOME Remote Desktop only supports Remote Login with RDP clients that implements Server Redirection (Microsoft's Windows App on macOS does NOT implement it and that's why I decides to write my own solution).
- [wayvnc](https://github.com/any1/wayvnc/)
	+ ReFrame supports non-wlroots desktop environments.
- [w0vncserver](https://tigervnc.org/)
	+ ReFrame supports Remote Login, [w0vncserver uses per-session XDG desktop portal](https://github.com/TigerVNC/tigervnc/pull/1947), which means it cannot support Remote Login.
- [x0vncserver](https://tigervnc.org/)
	+ ReFrame supports Wayland.

# Special Thanks

I am very grateful to [@isjerryxiao](https://github.com/isjerryxiao/) for creating [kmsvnc](https://github.com/isjerryxiao/kmsvnc/) and for giving advice for what could be fixed or improved in it. ReFrame is heavily inspired by kmsvnc and I've learnt a lot from it.
