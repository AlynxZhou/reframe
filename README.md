ReFrame Remote Desktop
======================

DRM/KMS based remote desktop for Linux that supports Wayland/NVIDIA/headless/login…
-----------------------------------------------------------------------------------

# Features

## What ReFrame Currently Support

- **VNC**
- **Wayland**/X11/**TTY**
- Intel/AMD/**NVIDIA**/[Enter more GPUs that can run a general Wayland compositor…]
- Pointer/Keyboard
- **Remote Login**
- Headless Setup
- Draw Remote Cursor

## What ReFrame May Support in Future

- **RDP**: PR is always welcome.

## What ReFrame Won't Support

- **Sound**: VNC has no sound support, and dumping sound is hard.
- **No GPU/connector/EGL/OpenGL ES/DRM/KMS**: Even llvmpipe should work.
- **Game Streaming**: VNC might be not optimized for low-latency. Consider game streaming optimized solutions like [Sunshine](https://github.com/LizardByte/Sunshine/).
- **DRM Overlay Plane**: Currently only a few compositors support overlay plane as a experimental feature, mostly for passing video frames to overlay plane so they don't need to decode and composite those frames and let hardware deal with those frames to reduce power comsumption. However handling such overlay planes in ReFrame only means moving such decoding and compositing operations from compositor to ReFrame (and due to natural limitations it is not reliable), thus you get no benefits. So you could just disable overlay plane support in your compositor.

# Requirements

A GPU that:

- supports **DRM/KMS**.
- has proper **EGL/OpenGL ES** implementation.
- is able to output to monitors.

If you can run a modern Linux desktop environment, it is likely you already meet those requirements.

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
- systemd (optional & recommanded)
- meson
- ninja
- gcc

### Build

```
$ git clone https://github.com/AlynxZhou/reframe.git
$ cd reframe
$ mkdir build && cd build && meson setup --prefix=/usr . .. && meson compile
# meson install
```

# Usage

> **Security Suggestion**: VNC data streams are not encrypted even with password authenciation, so **NEVER** expose this to public network directly! Connecting to it via VPN is recommended.

1. Disable automatic screen blank for **both user session and display manager session**: Otherwise the connector might be set to disconnected and we cannot get frames for it.
2. Run `systemctl start reframe-server@example.service` and try connecting to it via port `5933`.

If you have only 1 connected monitor and you never rotate it, it should work out of the box without modifying the example configuration. If it cannot find your monitor, you need to manually select monitor via DRM card and connector.

## Select Monitor via DRM Card and Connector

1. Find your DRM card name (e.g., `card0`) and connector name (e.g., `DP-1`) in `/sys/class/drm/`.
2. **Copy and modify the example configuration**: This program handles only 1 monitor, so I suggest to use your selected monitor connector name as configuration name.
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

ReFrame Server systemd service should automatically pulls ReFrame systemd socket, which will trigger the privileged ReFrame Streamer systemd service on demand. If not, start the ReFrame systemd socket manually.

```
# systemctl start reframe@DP-1.socket
```

## Multi-monitor

If you have more than 1 monitors, you need to set the size of the whole virtual desktop and the position offset of your selected monitor to make mouse position mapping works.

Unfortunately there is no general way to get those values for all desktop environments. You could run a program to get the current cursor position, and then move the cursor to get the following values.

- `desktop-width`: The x coordinate of right border of your right most monitor.
- `desktop-height`: The y coordinate of bottom border of your bottom most monitor.
- `monitor-x` and `monitor-y`: The x and y coordinate of top left corner of your selected monitor.

You need to keep the same multi-monitors layout for **both user session and display manager session** to make remote login work correctly.

## Headless Setup

This program only works with connected monitors, however if you have no monitor connected ("headless"), you can still use it because Linux kernel could force enable a connector to pretend there is a monitor.

1. **Choose a unused connector**: For example add kernel parameter `video="DP-1:D"` to enable `DP-1`.
2. **Set resolution via EDID override**: You can dump your real monitor's EDID from `/sys/class/drm/card*-*/edid` or download a virtual one, put it to `/lib/firmware/edid/`, for example `/lib/firmware/edid/1280x720.bin`, and then add kernel parameter `drm.edid_firmware="DP-1:edid/1280x720.bin"` to use it. **If you have early KMS, also add those EDID binaries to your initramfs.**

Then you can reboot your system, and come back to modify the configuration.

# Comparison with Other Linux Remote Desktop

- [kmsvnc](https://github.com/isjerryxiao/kmsvnc/)
	+ ReFrame supports NVIDIA driver by decoding frames with EGL and OpenGL ES, kmsvnc uses VA-API and it cannot decode frames of NVIDIA driver correctly.
	+ ReFrame gets frame buffer on each frame, so it works correctly if your compositor uses double-buffers.
	+ ReFrame splits into privilieged DRM/uinput part and unprivilieged VNC server part, kmsvnc runs as a whole privilieged process.
	+ ReFrame supports resizing client window.
- [RustDesk](https://github.com/rustdesk/rustdesk/)
	+ ReFrame supports Remote Login on Wayland, [RustDesk does not support it](https://rustdesk.com/docs/en/client/linux/#login-screen).
- [GNOME Remote Desktop](https://gitlab.gnome.org/GNOME/gnome-remote-desktop/)
	+ ReFrame supports non-GNOME desktop environments.
	+ ReFrame supports Remote Login with VNC, GNOME Remote Desktop only supports Remote Login with RDP clients that implements Server Redirection (Microsoft's Windows App on macOS does NOT implement it and that's why I decide to write my own solution).
- [wayvnc](https://github.com/any1/wayvnc/)
	+ ReFrame supports non-wlroots desktop environments.
- [x0vncserver](https://tigervnc.org/)
	+ ReFrame supports Wayland.

# Special Thanks

I am very appreciate to [@isjerryxiao](https://github.com/isjerryxiao/) for creating [kmsvnc](https://github.com/isjerryxiao/kmsvnc/) and giving advice for where could be fixed or improved in it. ReFrame is heavily inspired by kmsvnc and I've learnt a lot in it.
