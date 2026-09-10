# ROCKNIX RK3566-Specific PC environment

This optional environment consumes the owner-supplied official
`ROCKNIX-RK3566.aarch64-20260801-Specific.img.gz` image. The source contract
pins the compressed archive, raw disk, validated GPT, FAT32 system partition,
the contiguous `SYSTEM` SquashFS and selected files from its AArch64 rootfs.
No firmware payload is stored in Git or redistributed by the framework.

The image contains an SDL2/Wayland/Sway and Mesa/Panfrost/EGL/GLES userspace.
Its own `/etc/profile.d/050-sway.conf` declares `WAYLAND_DISPLAY=wayland-1` and
`SDL_VIDEODRIVER=wayland`; the Sway service declares
`WLR_BACKENDS=drm,libinput`. These are image facts, not a NextOS policy keyed
by the ROCKNIX name, an RG device brand or an inferred Mali model.

Prepare the environment outside the repository:

    python3 framework/tests/device-environments/rocknix/prepare.py \
      --archive /path/to/ROCKNIX-RK3566.aarch64-20260801-Specific.img.gz \
      --output /path/to/firmware-environments/rocknix-rk3566-20260801

Verify the prepared environment:

    python3 framework/tests/device-environments/rocknix/verify.py \
      --environment /path/to/firmware-environments/rocknix-rk3566-20260801

The focused gate queries the target AArch64 SDL under QEMU for its compiled
video-driver list and late-opens the target EGL/GLES dispatch libraries only to
resolve symbols. It never initializes SDL video, EGL or GLES and never accesses
DRM, framebuffer, Wayland sockets, input or audio devices. It then exercises
the real nxgl sanitizer with the measured driver list: an absent hint remains
absent, Wayland is preserved while available, and is removed without selecting
a replacement when unavailable. A missing provider returns `no-action` without
assigning `SDL_VIDEO_EGL_DRIVER`, `SDL_VIDEO_GL_DRIVER` or changing
`LD_PRELOAD`.

## Evidence boundary

The prepared image can prove file identity, ELF class/machine, dependency and
symbol inventory, and which video drivers the target SDL compiled. It cannot
prove that Wayland was selected, Sway was running, the kernel bound Panfrost to
a GPU, EGL initialized, a GLES context existed, or a frame reached the panel.
The `panfrost_dri.so` link, `panfrost.ko` module and Vulkan provider are static
inventory only. Physical graphics claims require a separately authorized
device run and receipt.
