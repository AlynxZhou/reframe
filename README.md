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

## What ReFrame May Support in Future

- RDP
- Draw Remote Cursor: Cursor are typically handled in another plane, I currently have no idea on how to handle it.

## What ReFrame Won't Support

- Sound: VNC has no sound support and I am not sure whether we can dump sound buffers in ALSA like what we do currently for graphics in DRM, and I think sound in display manager is not so useful, so maybe you can setup sound stream redirection for your session using PulseAudio/PipeWire-pulse, I think it already have network support.
- No GPU/connector/EGL/OpenGL ES/DRM/KMS: You probably cannot run a modern Linux desktop environment if you are lacking of those, then why you want a remote desktop? Don't you even have llvmpipe?
- Game Streaming: ReFrame does not handle network streaming directly but uses existing tools like VNC, and VNC might be not optimized for low-latency. You may use some game streaming optimized apps like [Sunshine](https://github.com/LizardByte/Sunshine/).

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

### Usage

**Security Warning**: VNC data streams are not encrypted even with password authenciation, so **never** expose this to public network directly! Connecting to it via VPN is a better idea.

First copy the example configuration and modify it according to your monitors.

This program handles only 1 monitor, so I suggest to use your selected monitor connector name as configuration name.

DRM connector name and card name can be found in `/sys/class/drm/`.

```
# cp /etc/reframe/example.conf /etc/reframe/DP-1.conf
```

Set connector name and card name to what your system uses for your selected monitor.

If you have multi-GPU (likely you are using a laptop with a integrated GPU and a discrete GPU), you need to select EGL device by setting device ID. EGL device must match DRM card, which is the one that outputs via selected connector (generally it is the integrated GPU). IDs can be found by running `eglinfo -B`.

If you have more than 1 monitors, you need to set the size of the whole virtual desktop, and the position offset of your selected monitor.

Unfortunately there are no general way to get those values for all desktop environments. You could run a program to get the current cursor position, and then move the cursor to the right border of your right most monitor, the current x value is `desktop-width`, and then move the cursor to the bottom border of your bottom most monitor, the current y value is `desktop-height`, and then move the cursor to the top left corner of your selected monitor, the current x and y value is `monitor-x` and `monitor-y`.

You need to keep the same multi-monitors layout **both of user session and display manager session** to make remote login work correctly.

You need to disable automatic screen blank for **both of user session and display manager session**, otherwise the connector might be set to disconnected and we cannot get frames for it.

Then start the ReFrame Server systemd service so it will listen to VNC clients.

```
# systemctl start reframe-server@DP-1.service
```

ReFrame Server systemd service should automatically pulls ReFrame systemd socket, which will trigger the privileged ReFrame Streamer systemd service on demand. If not, start the ReFrame systemd socket manually.

```
# systemctl start reframe@DP-1.socket
```

### Headless Setup

This program only works with connected monitors, however if you have no monitor connected ("headless"), you can still use it because Linux kernel could force enable a connector to pretend there is a monitor and the GPU driver will work.

First you need to choose a connector to enable, for example `DP-1`, and then add kernel parameter `video="DP-1:D"` to enable it.

Then you need to get a EDID binary, this is used to decide resolution, you can dump your real monitor's EDID from `/sys/class/drm/card*-*/edid` or download a virtual one, and put it to `/lib/firmware/edid/`, for example `/lib/firmware/edid/1280x720.bin`, and then add kernel parameter `drm.edid_firmware="DP-1:edid/1280x720.bin"` to use it. **If you have early KMS, please also add those EDID binaries to your initramfs.**

Don't forget to use your selected connector name to replace `DP-1`.

Then you can reboot your system, and continue to modify the configuration.

# Compare with Other Linux Remote Desktop Implementations

I only list pros of ReFrame, if you find cons of ReFrame, you can just use other implementations.

## [kmsvnc](https://github.com/isjerryxiao/kmsvnc/)

- ReFrame supports NVIDIA driver by decoding frames with EGL and OpenGL ES, kmsvnc uses VA-API and it cannot decode frames of NVIDIA driver correctly.
- ReFrame gets frame buffer on each frame, so it works correctly if your compositor uses double-buffers.
- ReFrame splits into privilieged DRM/uinput part and unprivilieged VNC server part, kmsvnc runs the whole process as privilieged.
- ReFrame supports resizing client window.

However, most of those cons are ideas from [@isjerryxiao](https://github.com/isjerryxiao/), and ReFrame initially is designed to improve kmsvnc.

## [RustDesk](https://github.com/rustdesk/rustdesk/)

- ReFrame supports Remote Login on Wayland, [RustDesk does not support it](https://rustdesk.com/docs/en/client/linux/#login-screen).

## [GNOME Remote Desktop](https://gitlab.gnome.org/GNOME/gnome-remote-desktop/)

- ReFrame supports non-GNOME desktop environments.
- ReFrame supports Remote Login with VNC, GNOME Remote Desktop only supports Remote Login with RDP clients that implements Server Redirection (Microsoft's Windows App on macOS does NOT implement it).

## [wayvnc](https://github.com/any1/wayvnc/)

- ReFrame supports non-wlroots desktop environments.

## [x0vncserver](https://tigervnc.org/)

- ReFrame supports Wayland.

# Special Thanks

I am very appreciate to [@isjerryxiao](https://github.com/isjerryxiao/) for creating [kmsvnc](https://github.com/isjerryxiao/kmsvnc/) and giving advice for where could be fixed or improved in it. ReFrame is heavily inspired by kmsvnc and I've learnt a lot in it.

# How to Debug

```
$ mkdir build && cd build && meson setup --prefix=/usr --buildtype=debug . .. && meson compile
# setcap cap_sys_admin+ep reframe-streamer/reframe-streamer
$ G_MESSAGES_DEBUG=ReFrame ./reframe-streamer/reframe-streamer --config=/etc/reframe/DP-1.conf --keep-listen
$ EGL_PLATFORM=surfaceless G_MESSAGES_DEBUG=ReFrame ./reframe-server/reframe-server --config=/etc/reframe/DP-1.conf
```
