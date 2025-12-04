ReFrame Remote Desktop
======================

DRM/KMS based remote desktop for Linux that supports Wayland/NVIDIA/headless/login…
-----------------------------------------------------------------------------------

# Features

## What ReFrame Currently Support

- VNC
- Wayland/X11/TTY
- Intel/AMD/NVIDIA/[Enter more GPUs that can run a general Wayland compositor…]
- Pointer/Keyboard
- Remote Login
- Headless Setup
- Draw Remote Cursor

## What ReFrame May Support in Future

- RDP: There should be no difference between supporting VNC and RDP for ReFrame, but personally I use VNC, if you need RDP support and implement it by yourself, I'll be very grateful if you send a PR.

## What ReFrame Won't Support

- Sound: VNC has no sound support and I am not sure whether we can dump sound buffers in ALSA like what we do currently for graphics in DRM, and I think sound in display manager is not so useful, so maybe you can setup sound stream redirection for your session using PulseAudio/PipeWire-pulse, I think it already have network support.
- No GPU/connector/EGL/OpenGL ES/DRM/KMS: You probably cannot run a modern Linux desktop environment if you are lacking of those, then why you want a remote desktop? Don't you even have llvmpipe?
- Game Streaming: ReFrame does not handle network streaming directly but uses existing tools like VNC, and VNC might be not optimized for low-latency. You may use some game streaming optimized apps like [Sunshine](https://github.com/LizardByte/Sunshine/).
- DRM Overlay Plane: Currently only a few compositors support overlay plane as a experimental feature, typically they pass video frames to overlay plane so they don't need to decode and composite those frames and let hardware deal with those frames to reduce power comsumption. However handling such overlay planes in ReFrame only means moving such decoding and compositing operations from compositor to ReFrame (and due to natural limitations it is not reliable), thus you get no benefits. So you could just disable overlay plane support in your compositor.

# Requirements

To run ReFrame, your system must have a GPU that supports DRM/KMS and has proper EGL/OpenGL ES implementation and can output to monitors. Modern Linux desktop environments requires them (even NVIDIA supports DRM/KMS now), so it should not be too hard to meet requirements.

If you cannot meet those requirements (for example embedded devices without proper OpenGL ES implementation), you should try some old (but not good) remote desktop implementations.

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

**Security Warning**: VNC data streams are not encrypted even with password authenciation, so **never** expose this to public network directly! Connecting to it via VPN is a better idea.

If you happen to have only 1 connected monitor and you never rotate it, it should work out of the box without modify the example configuration. Run `systemctl start reframe-server@example.service` and try connecting to it via port `5933`.

If it cannot correctly find your monitor, you need to manually select monitor via DRM card and connector.

## Select Monitor via DRM Card and Connector

First copy the example configuration and modify it. This program handles only 1 monitor, so I suggest to use your selected monitor connector name as configuration name.

DRM connector name and card name can be found in `/sys/class/drm/`.

```
# cp /etc/reframe/example.conf /etc/reframe/DP-1.conf
```

Set connector name and card name to what your system uses for your selected monitor.

If you rotated your monitor, set the value of `rotation` to the angle.

By default it will try to resize the VNC client size to monitor size on start and follow the resize of VNC client, however, if you are using a VNC client that does not support resizing, or you don't want to resize the VNC client window manually every time, you can set values of `default-width` and `default-height`.

Then start the ReFrame Server systemd service so it will listen to VNC clients.

```
# systemctl start reframe-server@DP-1.service
```

ReFrame Server systemd service should automatically pulls ReFrame systemd socket, which will trigger the privileged ReFrame Streamer systemd service on demand. If not, start the ReFrame systemd socket manually.

```
# systemctl start reframe@DP-1.socket
```

## Multi-monitor

If you have more than 1 monitors, you need to set the size of the whole virtual desktop and the position offset of your selected monitor to make mouse input position mapping works.

Unfortunately there is no general way to get those values for all desktop environments. You could run a program to get the current cursor position, and then move the cursor to the right border of your right most monitor, the current x value is `desktop-width`, and then move the cursor to the bottom border of your bottom most monitor, the current y value is `desktop-height`, and then move the cursor to the top left corner of your selected monitor, the current x and y value is `monitor-x` and `monitor-y`.

You need to keep the same multi-monitors layout **both of user session and display manager session** to make remote login work correctly.

You need to disable automatic screen blank for **both of user session and display manager session**, otherwise the connector might be set to disconnected and we cannot get frames for it.

## Headless Setup

This program only works with connected monitors, however if you have no monitor connected ("headless"), you can still use it because Linux kernel could force enable a connector to pretend there is a monitor and the GPU driver will work.

First you need to choose a connector to enable, for example `DP-1`, and then add kernel parameter `video="DP-1:D"` to enable it.

Then you need to get a EDID binary, this is used to decide resolution, you can dump your real monitor's EDID from `/sys/class/drm/card*-*/edid` or download a virtual one, and put it to `/lib/firmware/edid/`, for example `/lib/firmware/edid/1280x720.bin`, and then add kernel parameter `drm.edid_firmware="DP-1:edid/1280x720.bin"` to use it. **If you have early KMS, please also add those EDID binaries to your initramfs.**

Don't forget to use your selected connector name to replace `DP-1`.

Then you can reboot your system, and come back to modify the configuration.

# Compare with Other Linux Remote Desktop Implementations

I only list pros of ReFrame, if you find cons of ReFrame, you can just use other implementations.

## [kmsvnc](https://github.com/isjerryxiao/kmsvnc/)

- ReFrame supports NVIDIA driver by decoding frames with EGL and OpenGL ES, kmsvnc uses VA-API and it cannot decode frames of NVIDIA driver correctly.
- ReFrame gets frame buffer on each frame, so it works correctly if your compositor uses double-buffers.
- ReFrame splits into privilieged DRM/uinput part and unprivilieged VNC server part, kmsvnc runs as a whole privilieged process.
- ReFrame supports resizing client window.

However, most of those improvements are ideas from [@isjerryxiao](https://github.com/isjerryxiao/), and ReFrame initially is designed as a better re-implementation of kmsvnc.

## [RustDesk](https://github.com/rustdesk/rustdesk/)

- ReFrame supports Remote Login on Wayland, [RustDesk does not support it](https://rustdesk.com/docs/en/client/linux/#login-screen).

## [GNOME Remote Desktop](https://gitlab.gnome.org/GNOME/gnome-remote-desktop/)

- ReFrame supports non-GNOME desktop environments.
- ReFrame supports Remote Login with VNC, GNOME Remote Desktop only supports Remote Login with RDP clients that implements Server Redirection (Microsoft's Windows App on macOS does NOT implement it and that's why I decide to write my own solution).

## [wayvnc](https://github.com/any1/wayvnc/)

- ReFrame supports non-wlroots desktop environments.

## [x0vncserver](https://tigervnc.org/)

- ReFrame supports Wayland.

# Special Thanks

I am very appreciate to [@isjerryxiao](https://github.com/isjerryxiao/) for creating [kmsvnc](https://github.com/isjerryxiao/kmsvnc/) and giving advice for where could be fixed or improved in it. ReFrame is heavily inspired by kmsvnc and I've learnt a lot in it.
