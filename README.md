Build & launch

  Runtime layers (every binary inherits LD_PRELOAD=spi_shim.so)

  ┌── Constructor entry (spi_shim.c) ─────────────────────────────────────────┐
  │  shim_init_dirs        → mkdir _emulation/captures/audio + /tmp/cache    │
  │  shim_install_crash    → SIGSEGV/BUS/ABRT/TERM/INT/USR1/USR2/TRAP        │
  │                           handlers; shim_bridge_start, shim_frame_start, │
  │                           shim_audio_start, shim_inject_start            │
  └──────────────────────────────────────────────────────────────────────────┘

  Filesystem & device interception

  File: shim_fs.c
  Owns: remap_path (/dev /proc /run /sys (≠ firmware) /home /root /var skipped — everything else gets $SYSROOT prefix;
    /tmp carve-out commented out → falls through to host); open/openat/open64/openat64/fopen/fopen64; access/faccessat
    (libc-level because glibc INLINE_SYSCALLs bypass faccessat hook); stat family; mkdir/mkdirat (treats EEXIST as
    success); GPIO sysfs fake-fd pool (16 slots @ FAKE_FD_GPIO_BASE=1000); libgpiod chardev stubs (output-only)
  ────────────────────────────────────────
  File: spi_shim.c
  Owns: intercept_device(path) dispatcher: /dev/spidev4.0→FAKE_FD_SPI, /dev/i2c-10→FAKE_FD_I2C, /dev/dri/card1→real
    socketpair, /dev/shim-v4l2-*→fake fds, /dev/boot_display→fake fd, /dev/ttyAMA{2,5}→real socketpair
  ────────────────────────────────────────
  File: shim_io.c
  Owns: close/read/lseek/write + LFS variants (lseek64, pread64, pwrite64, pread, pwrite) + mmap/mmap64/munmap
    (file-backed only) observation hooks. UART read passes through real socketpair; UART write logs hex + calls
    synthesize_bridge_response. GPIO sysfs synthetic reads ("0\n" for value, "out\n" for direction etc., one-shot latch
    reset by lseek(0))
  ────────────────────────────────────────
  File: shim_ioctl.c
  Owns: UART termios ioctls (TCGETS/TCSETS/TIOCMGET/FIONREAD/TIOCOUTQ); SPI MESSAGE_1 with per-cmd env-var overrides
    SPIKE3_SPI_RX_XX=hexbytes (default rx[2]=0x04); DRM (1 CRTC, 2 connectors @ HDMI-A 1920×1080@60, MODE_PAGE_FLIP
    synthesizes drm_event_vblank on the shim socketpair side + calls shim_frame_capture_at_page_flip); V4L2 returns
    ENOTTY; I2C log-only

  Bridges & IPC (all AF_UNIX abstract namespace)

  shim_bridges.c — three independent listeners, each with accept thread + (for state) poll thread:

  ┌────────────────────────┬───────────────────────┬─────────────────────────────────────────────────────────────────┐
  │          Name          │       Env gate        │                           Wire format                           │
  ├────────────────────────┼───────────────────────┼─────────────────────────────────────────────────────────────────┤
  │ @pokemon_spike3_bridge │ SPIKE3_BRIDGE=1       │ newline-delimited JSON cmode deltas, 10 Hz poll of cmode +0x308 │
  ├────────────────────────┼───────────────────────┼─────────────────────────────────────────────────────────────────┤
  │                        │                       │ 16B header (magic 0xF00FBABA, seq, w16, h16, fmt, flags) +      │
  │ @pokemon_spike3_frame  │ SPIKE3_FRAME_BRIDGE=1 │ W·H·4 RGBA. Captured every 2nd MODE_PAGE_FLIP via               │
  │                        │                       │ glReadPixels(0,0,1920,1080) from shim_last_draw_fbo (or current │
  │                        │                       │  READ_FRAMEBUFFER)                                              │
  ├────────────────────────┼───────────────────────┼─────────────────────────────────────────────────────────────────┤
  │ @pokemon_spike3_audio  │ SPIKE3_AUDIO_BRIDGE=1 │ raw S16_LE 2ch 44.1 kHz, no header. Tee'd from snd_pcm_writei   │
  └────────────────────────┴───────────────────────┴─────────────────────────────────────────────────────────────────┘

  Netbridge UART protocol (shim_nodebus.c)

  Frame format [STX 0x02][CMD][ID][payload][CKSUM=−Σ][ETX 0x03] with byte-stuffing escape 0x1b NN→NN&0x7f. Synthesized
  responses:

  - 0x10 → cmd 0x10, 7-byte pwrdist version 01 00 00 00 00 00 00 (puts game on "has Spike 3 pwrdist" path)
  - 0x11, 0x1c → silent ACK
  - 0x15, 0x18, 0x1d → 1-byte 0x00 ACK
  - 0x16 → 01 00 02 00 (status-poll exit signature)
  - 0x20 → cmd 0x10, 4 zero bytes (hw version)
  - 0x27 → alternating 0x06/0x02 (paired loop bit-2 toggle)
  - 0x30 → cmd 0x20, NETBRIDGE_CRC32=0x357f5327
  - 0x40 → log only, no response (intentional timeout = progress per Step 45)
  - 0x41 → encrypted node-bus channel: log decoded inner_cmd + payload. NODEBUS_SYNTH_FE=1 enables fe_probe synthesis —
  emits "node not present, default chip" response to inner_cmd 0xFE/0xFA. Memory warning: NODEBUS_SYNTH_FE=1 long stable
   runs don't initialize Godot — not a healthy baseline.

  Cipher keys (nodebus_cipher_keys[32][16]) stay zero; game's per-slot status flag means encryption loop is skipped on
  the wire so we treat as plaintext. nodebus_cipher_decrypt exists but isn't currently used (Step 69 retracted decode).

  GL / EGL / GBM (shim_gl.c)

  Two modes selected by SPIKE3_REAL_GL (default 1):

  - Stub mode: every GLES/EGL/GBM call returns plausible defaults. Shaders "compile". Draws no-op.
  - Real-GL mode: FWD_RET / FWD_VOID macros tail-call dlsym(RTLD_NEXT,...) into Mesa overlay
  (mesa_sw_overlay/pkg/usr/lib/aarch64-linux-gnu/). Translation glue:
    - eglGetDisplay/PlatformDisplay[EXT] → drops game's GBM_KHR query, asks Mesa for EGL_PLATFORM_SURFACELESS_MESA
    - eglChooseConfig → patches EGL_SURFACE_TYPE: WINDOW_BIT→PBUFFER_BIT (Mesa surfaceless has no window configs)
    - eglCreateWindowSurface → redirects to eglCreatePbufferSurface(1920×1080)
    - eglMakeCurrent(draw=NULL) → fabricates fallback pbuffer from shim_last_real_config
    - eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID) → forces XR24 fourcc
    - glFramebufferTexture2D/Layer → swap GL_TEXTURE_EXTERNAL_OES → GL_TEXTURE_2D
    - eglCreateImageKHR → never forwards to Mesa (would deref fake GBM); registers {w,h,fourcc} in side-table, returns
  sentinel. glEGLImageTargetTexture2DOES → looks up + does real glTexImage2D(0, RGBA8, w, h, ..., NULL) to give the
  bound texture storage; tracks in shim_frametex for capture targeting.
  - dlopen/dlsym interception (in spi_shim.c): tracks libEGL/libGLES/libgbm handles, substitutes our 3 EGLImage
  overrides on dlsym(handle, "eglCreateImageKHR"/"eglDestroyImageKHR"/"glEGLImageTargetTexture2DOES").
  - eglGetProcAddress returns table of our stubs (never forwards — would hand fake GBM to real Mesa).

  Diagnostics (shim_diagnostics.c)

  - shim_resolve_game_base() — atomic-cached, walks backtrace+dladdr for first frame inside ./game.
  - shim_cmode_table[42] — BN VAs + expected mode_id/type_flags + name_hint. External TUs read this (the state bridge
  poll thread, the event sweep, the cmode-poke thread, the switch-fire snapshotter).
  - shim_dump_sound_globals — full /proc/self/maps + g_asset_registry hashtable walk (writes
  _emulation/captures/asset_registry.bin with header, raw nodes).
  - shim_dump_cmodes — read each cmode +0x08 (mode_id) +0x0c (type_flags) +0x58 (scene_ptr) +0x308 (init_flag); hexdump
  scene_data of base_00, story_charmander_1, battle_mountain.
  - shim_dump_dispatch_tables — registry @ data_4767b30 (42×8B), event handlers @ data_485ea80 (207 slots × 8B
  linked-list head, walk +0x10), update types @ data_48c52c0 (5×8B).
  - SIGSEGV → backtrace + sound + cmode + dispatch dumps, then SIG_DFL re-raise. Detects "in event sweep" and
  siglongjmps back instead.
  - SIGABRT → backtrace with ELF VAs for game frames (paste-able into BN), AudioServer singleton dump (data_451ac48
  vtable slot enumeration), then SIG_DFL re-raise.
  - SIGTERM/INT → dump and _exit(0). SIGUSR1 → dump and continue. SIGUSR2 → run event sweep.

  Injection layer (shim_inject.c)

  - 105-switch manifest from Pokémon LE manual.
  - shim_fire_event(id, data) — trampoline to game's fire_event @ ELF VA 0x9c12a0.
  - shim_fire_switch(id) — flips byte at game_base + 0x4480220 + id*0x30 + 0x28. Note: no event dispatch; game's
  translator hasn't been hooked, so closures may go unobserved until poll cycle.
  - shim_event_sweep — fires events 0x00..0xce with alarm(1) per-event timeout, siglongjmp recovery on timeout (code 2)
  or SEGV (code 1). Diffs cmode +0x308/+0x60 snapshots between events.
  - 6 env-gated worker threads, all spawned at constructor:
    - SPIKE3_SWEEP_AFTER=<sec> (+ SPIKE3_SWEEP_EVENTS=1) → run sweep
    - SPIKE3_FIRE_SWITCH=<id1:id2:...> (+ SPIKE3_SWITCH_DELAY) → fire switch IDs
    - SPIKE3_FORCE_GSTREAMER=<sec> → trampoline into game's gstreamer_init @ 0xcc1ec0
    - SPIKE3_POKE_CMODE=<name>:<hex>:<delay> → write hex to cmode +0x308
    - SPIKE3_POKE_STATE_FLAG=<mask>:<delay_ms> → atomic OR mask into system_state_flags @ 0x4482388
    - SPIKE3_PATCH_SPI_VIDEO=1 → shim_vp_patch_thread calls all 6 patches after 2s settle

  Runtime patches (shim_patches.c)

  Other hooks (in spi_shim.c + shim_net.c)

  Boundary contract (single header spi_shim.h)

  The TUs share a deliberately narrow interface:
  - SHIM_LOG, FWD_RET/VOID(_V), shim_unix_abstract_addr, shim_format_log_timestamp
  - Display: FAKE_MODE_HDISPLAY/VDISPLAY = 1920/1080
  - Cmode table externs (read by shim_bridges.c poll, shim_inject.c sweep)
  - 5 signal handlers + shim_event_sweep, shim_event_sweep_jmp, shim_event_fire_in_progress, shim_current_sweep_event_id
   (set by inject, read by diagnostics)
  - synthesize_bridge_response (called from io.c write hook)
  - Fake fds (FAKE_FD_SPI=1023, I2C=1022, V4L2_MEDIA=1021, V4L2_VIDEO=1020, BOOT_DISPLAY=1019, GPIO_BASE=1000)
  - 11 device-state externs + 5 fd predicates + 5 real-libc fn pointers
  - GPIO pool externs
  - 5 patch functions + 1 brk runtime addr
  - Bridge/inject lifecycle entry points
  - is_real_gl() cache
  - shim_last_draw_fbo (set in shim_gl.c, read in shim_bridges.c)

  Notable assumptions / load-bearing details

  1. Sysroot detection: _emulation lives at games/pokemon_le/_emulation, so ../../.. is the sysroot root. Both
  run_game.sh and Python helpers depend on this layout.
  2. BN-rebased addresses: every constant in shim_patches.c / shim_diagnostics.c / shim_inject.c is an ELF VA = current
  BN VA (BN database rebased to base 0x0). Runtime address = shim_resolve_game_base() + ELF_VA.
  3. PIE base resolved via backtrace+dladdr: requires at least one frame inside ./game on the calling thread's stack.
  Sweep timer and poke threads have no game frames, so they wait until ioctl/etc fires the cache from a guest thread.
  4. Stub mkdir at constructor for /tmp/cache: under remap rules /tmp/ is currently NOT remapped (carve-out commented
  out in shim_fs.c:86–102). The constructor's mkdir("/tmp/cache", 0755) therefore lands on the host /tmp, not
  $SYSROOT/tmp/cache — MESA_SHADER_CACHE_DIR env var passed in run_game.sh is the actual mechanism keeping shader cache
  in-sysroot.
  5. AF_UNIX abstract namespace: WSL2's 9P mount can't bind AF_UNIX on /mnt/c/.... All three bridges use abstract names
  (\0pokemon_spike3_*); legacy filesystem socket reaping in run_game.sh + nuke_emulation.sh is harmless backward-compat.
  6. DRM page-flip presentation: game uses MODE_PAGE_FLIP, not eglSwapBuffers. Frame capture trigger is in
  shim_frame_capture_at_page_flip() called from the DRM ioctl handler. Captures every 2nd flip.
  7. Per-cmd-byte SPI override (SPIKE3_SPI_RX_XX=) is loaded once on first MESSAGE_1 ioctl. To change values at runtime
  you'd have to restart the game.
  8. Switch closures via shim_fire_switch flip the state byte but DON'T currently fire game events — the netbridge cmd
  0x27 → state-write → fire_event translator is unhooked (per the in-source comment + memory). So switches fire silently
   until the game's own poll observes the byte.

  What's still missing or fragile
  - 11 device-state externs + 5 fd predicates + 5 real-libc fn pointers
  - GPIO pool externs
  - 5 patch functions + 1 brk runtime addr
  - Bridge/inject lifecycle entry points
  - is_real_gl() cache
  - shim_last_draw_fbo (set in shim_gl.c, read in shim_bridges.c)

  Notable assumptions / load-bearing details

  1. Sysroot detection: _emulation lives at games/pokemon_le/_emulation, so ../../.. is the sysroot root. Both
  run_game.sh and Python helpers depend on this layout.
  2. BN-rebased addresses: every constant in shim_patches.c / shim_diagnostics.c / shim_inject.c is an ELF VA = current
  BN VA (BN database rebased to base 0x0). Runtime address = shim_resolve_game_base() + ELF_VA.
  3. PIE base resolved via backtrace+dladdr: requires at least one frame inside ./game on the calling thread's stack.
  Sweep timer and poke threads have no game frames, so they wait until ioctl/etc fires the cache from a guest thread.
  4. Stub mkdir at constructor for /tmp/cache: under remap rules /tmp/ is currently NOT remapped (carve-out commented
  out in shim_fs.c:86–102). The constructor's mkdir("/tmp/cache", 0755) therefore lands on the host /tmp, not
  $SYSROOT/tmp/cache — MESA_SHADER_CACHE_DIR env var passed in run_game.sh is the actual mechanism keeping shader cache
  in-sysroot.
  5. AF_UNIX abstract namespace: WSL2's 9P mount can't bind AF_UNIX on /mnt/c/.... All three bridges use abstract names
  (\0pokemon_spike3_*); legacy filesystem socket reaping in run_game.sh + nuke_emulation.sh is harmless backward-compat.
  6. DRM page-flip presentation: game uses MODE_PAGE_FLIP, not eglSwapBuffers. Frame capture trigger is in
  shim_frame_capture_at_page_flip() called from the DRM ioctl handler. Captures every 2nd flip.
  7. Per-cmd-byte SPI override (SPIKE3_SPI_RX_XX=) is loaded once on first MESSAGE_1 ioctl. To change values at runtime
  you'd have to restart the game.
  8. Switch closures via shim_fire_switch flip the state byte but DON'T currently fire game events — the netbridge cmd
  0x27 → state-write → fire_event translator is unhooked (per the in-source comment + memory). So switches fire silently
   until the game's own poll observes the byte.

  What's still missing or fragile

  - Path remap for /tmp commented out — shim_fs.c lines 86–102. Without it, /tmp/godot_logs and /tmp/cache go to host
  /tmp. Memory says Godot logs end up at /tmp/godot_log(s) — so they're hitting host /tmp.
  - NV persistence stubs (per memory): EEPROM and FS NV writes vanish. No SIGTERM flush.
  - Switch translator unhooked — switch fires don't propagate.
  - Cipher keys never refreshed — fe_probe synthesis uses zero keys; if game ever fetches non-zero keys via inner-cmd
  0xfc, decryption diverges.
  - Cmd 0x40 / 0x41 mostly log-only — game tolerates timeout (verified Step 45 lesson).
  - VideoPlayerSpi is the current blocker (memory): brk probe + tbz NOP + Play vtable patch all in flight; play() loop
  times out and destructor pure-virtual abort still observed.
  - boot_display fd (FAKE_FD_BOOT_DISPLAY) is inert — ioctls return 0, writes swallowed. If real boot_display is doing
  anything beyond log probes, we don't see it.