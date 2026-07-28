# Leaf3 E-Ink Optimization Roadmap

This roadmap orders future E-Ink work by device risk. Each phase must remain
independently reversible and pass its hardware gate before work proceeds to
the next phase. Stability takes priority over matching every stock-ROM feature
at once.

## Current baseline

The production display path uses SurfaceFlinger notifications, damage-cropped
`ScreenshotClient` capture, and one rate-limited EBC update rectangle per
changed frame. Periodic full capture remains the automatic fallback. The
following safe optimizations are already present:

- Compositor damage delivery with tile-aligned cropped capture, 32-pixel tile
  verification, and sparse copies into the persistent EBC buffer and
  previous-frame cache.
- Touch-slop scroll detection, with row-hash detection as a fallback, and A2
  updates only for sufficiently large scrolling regions.
- Per-tile fast-update aging and bounded regional GC16 cleanup after the
  display becomes quiet.
- Pre-capture EBC pacing, so compositor notifications coalesce to the newest
  frame while the driver interval is busy instead of queueing a stale
  screenshot.
- A stock-inspired Reader profile with DU page turns, independent ANIM
  scrolling, and configurable GC16 intervals of 1, 3, 5, 10, 30, 50 pages or
  never.
- An optional settled Regal pass: large page changes appear first through AUTO
  and receive REGAL only after 180 ms of compositor quiet.
- Bridge-side grayscale tone controls for contrast, gamma, and EBC dithering.
- Latency telemetry for the pacing gate, notification-to-capture,
  notification-to-submit, and the EBC ioctl itself.
- Blocking display-off waits and property-value caching.
- Disabled wallpaper, doze/AOD drawing, automatic brightness, touch ripples,
  edge glow, and navigation-bar scrims.
- Optional global grayscale and user-selected waveform profiles.
- A 100 ms minimum interval between EBC ioctls.
- An opt-in composer-native EPDC backend that attaches damage and waveform
  metadata to the matching HWC frame without screenshot readback. The
  direct-EBC bridge remains the default and automatic failure fallback.

Virtual-display capture and content-aware direct-EBC waveform selection remain
quarantined because both caused crashes under composition or scrolling load.

## Phases

| Phase | Status | Goal | Risk |
| --- | --- | --- | --- |
| 0 | Complete | Stabilize and optimize the screenshot/EBC bridge | Low |
| 1 | Validation, default | Wake screenshot capture from a SurfaceFlinger frame notification | Low to medium |
| 2 | Validation | Deliver compositor damage with the notification | Medium |
| 3 | Planned | Add conservative automatic application profiles | Low |
| 4 | Planned | Make grayscale the default for new installations | Low |
| 5 | Validation | Add per-tile update-aged regional ghost cleanup | Medium |
| 5A | Implemented, hardware validation required | Coalesce to the newest frame before EBC submission | Low to medium |
| 5B | Implemented, hardware validation required | Port stock reader refresh policy and tone controls | Low to medium |
| 6 | Implemented, hardware validation required | Port the stock composer-native EPDC protocol | High |
| 7 | Research | Expose stock-style per-surface E-Ink controls | High |

### Phase 1: SurfaceFlinger frame notification

Notification is the default on new installations. It remains in validation
status until it passes the hardware gate below; explicit `poll` selection and
automatic failure fallback remain available.

Add a small, LineageOS 18.1-specific SurfaceFlinger integration that signals a
nonblocking `eventfd` after a physical-display frame is committed. The bridge
will wait for that notification, capture only the latest primary frame, and
retain the existing 100 ms EBC pacing.

This phase does not create a virtual display, modify the vendor composer, or
send additional EBC ioctls. Registration failure, unsupported builds, or a
stalled notifier must automatically fall back to the current polling path.
The notification path is selected by the default property and remains
independently reversible with `leaf3-refresh capture poll`.

Success criteria:

- Idle screenshot captures and comparisons fall close to zero.
- Touch, fling, keyboard, rotation, wake, and animation changes are not lost.
- Notification bursts coalesce without building a queue of stale frames.
- A missing or failed notifier returns to polling without losing the panel.
- Settings and browser scrolling pass the hardware stress gate.

### Phase 2: SurfaceFlinger damage delivery

The versioned frame-notifier transaction now accumulates the default display's
dirty region and lets the bridge atomically consume its bounding rectangle.
The bridge aligns it to its 32-pixel grid, captures only that crop, updates a
persistent assembled frame, and compares only intersecting tiles.

Initially, continue to coalesce damage into one safe EBC rectangle and one
ioctl. The purpose of this phase is to reduce CPU time and memory bandwidth,
not to reintroduce unsafe multi-update batches. Keep tile comparison as a
fallback when compositor damage is absent, invalid, or larger than the frame.

Success criteria:

- Damage never excludes pixels that visibly changed.
- `capture_us`, `compare_us`, and copied-byte metrics improve on common UI
  workloads.
- Widely separated changes do not trigger additional EBC ioctls.
- The polling and full-comparison fallbacks remain functional.

### Phase 3: Conservative automatic application profiles

Add built-in defaults based on the foreground application:

- Settings and launchers: Balanced.
- Browsers: Balanced with touch-driven A2 scrolling.
- Readers: Regal or Normal.
- Terminals: Speed or Regal.
- Video and games: A2.

User-created profiles must override built-in defaults, and unknown
applications must use the selected global mode. Profile changes must not force
a full-screen refresh.

### Phase 4: Default grayscale for new installations

Enable SurfaceFlinger's global grayscale color matrix by default on fresh
installs while retaining the Leaf3 Controls switch. Existing installations
must preserve their current choice. Validate screenshots, accessibility color
features, WebView, and media before making the default permanent.

### Phase 5: Update-count-based ghost management

Track a saturating age for every tile touched by a fast update. After the
existing quiet period, begin at the oldest tile and grow through adjacent
dirty tiles without exceeding one third of the panel. Reset only tiles covered
by the GC16 region; disconnected tiles remain queued for a separately paced
cleanup.

Automatic ghost management must never request a full-screen cleanup. Manual
cleanup policy must continue to disable automatic cleanup, and every cleanup
must obey the global EBC ioctl interval.

### Phase 5A: Latest-frame scheduling

The EBC safety interval is now observed before a notified screenshot is
captured. While the interval is busy, SurfaceFlinger continues accumulating
damage and the notifier collapses additional commits. The bridge therefore
captures the newest available composition at the deadline instead of sleeping
with an older screenshot already staged.

The ioctl path retains its own final spacing check as a safety backstop for
manual, cleanup, suspend, and polling submissions. New statistics separate
time spent at the EBC gate from capture, comparison, submission, and ioctl
time.

Hardware validation must establish:

- Page-turn bursts increase the dropped/coalesced notification count without
  displaying intermediate pages.
- No EBC ioctl is issued less than 100 ms after the previous one.
- Notification-to-submit latency improves without notifier-health regressions.
- Manual cleanup, tone changes, screen-off clearing, and polling fallback
  continue to work.

### Phase 5B: Stock reader and image policy

The stock reader uses a persistent fast application scope and performs a
quality cleanup only after a user-selected number of page changes. The bridge
now reproduces that policy without private framework APIs:

- `Reader` is an explicit global and per-app profile.
- Large static Reader changes use DU; page counting is separate from
  touch-slop/row-hash scrolling, which still uses ANIM.
- The foreground-package token resets the page counter when switching between
  reader applications, even when both apps use the same Reader profile.
- The cleanup interval accepts the stock choices 1, 3, 5, 10, 30, 50, and
  disabled. A due cleanup applies regional GC16 to the new page and resets the
  counter. Reader page turns do not arm the ordinary quiet-time cleanup.
  Manual cleanup remains authoritative and disables the page counter too.
- Large Regal changes use AUTO for the immediately visible frame and,
  optionally, one REGAL pass after 180 ms without a replacement frame.
- Contrast and gamma use a cached 256-entry luminance table while staging
  changed EBC rows. Default values preserve the prior memcpy path. Dithering
  remains a separately configurable EBC update flag.

These controls are available in Leaf3 Controls and `leaf3-refresh`. Tone
changes request one non-flashing AUTO redraw of the accumulated frame.

Hardware validation must compare Reader against Speed and Regal using text,
mixed text/images, PDF pages, physical page keys, touch page turns, continuous
scrolling, and rapid multi-page navigation. Test every cleanup interval,
contrast/gamma extremes, and dithering both enabled and disabled.

### Phase 6: Stock composer-native EPDC protocol

The stock ABI has been recovered and implemented in a strict LineageOS 18.1
framework patch. The preserved QTI composer client is used only when its HIDL
interface chain contains
`vendor.qti.hardware.display.composer@3.0::IQtiComposerClient`.

`CommitEpdc` uses command opcode `0x08020000`. Its 16-bit command length is
`1 + 5 * update_count` words. The payload is a 32-bit update count followed by
one to eight records of:

```text
left, top, right, bottom, mode
```

Coordinates use exclusive right and bottom edges. The implemented stock mode
values are DU `1`, GC16 `2`, ANIM `4`, AUTO `5`, REGAL `6`, full-update flag
`0x20`, and dither flag `0x100`. Host tests lock the single-record and maximum
eight-record serialization, including the encoded header lengths. They also
compile and execute the production policy header to cover timer-only presents,
racing real damage, live cleanup-policy deadlines, and private-command
classification.

CompositionEngine preserves the physical display's dirty region before it is
cleared. HWC2 queues the EPDC batch immediately before `PRESENT_DISPLAY`, so
the private command and the corresponding frame execute in one composer
command buffer. Damage with at most eight rectangles remains a native batch;
more complex damage safely collapses to its bounding rectangle. Virtual
displays never receive the command.

The backend is boot-latched with `persist.sys.leaf3.epdc_backend`. `bridge`
remains the shipping default. `composer` removes screenshot capture, pixel
comparison, EBC buffer copies, and direct EBC ioctls from the display path.
Native submission and its timer worker remain inactive until the backend is
requested, the QTI capability is confirmed, and SurfaceFlinger has installed
its refresh callback. Immediate timer deadlines remain fixed after arming so
partial startup cannot spin while holding the controller mutex.
The bridge service stays alive only for frontlight and fallback coordination.
If the QTI descriptor is absent, command execution fails, or presentation
fails after a native command is queued, SurfaceFlinger permanently blocks
native submission for that boot and the service starts the known-good
direct-EBC bridge. `leaf3-refresh backend bridge` also blocks new native
submission immediately, drains any admitted present, and observes the 100 ms
EPDC safety interval before direct EBC starts. Selecting composer requires a
reboot so both writers can never run concurrently.

Native policy currently supports Balanced/Normal AUTO, Speed DU, A2 ANIM,
Regal with an optional settled pass, Reader DU with interval GC16, dithering,
and bounded per-tile regional cleanup. Screenshot-only behavior—automatic
touch/row-hash scrolling, contrast/gamma staging, idle capture policy, and a
white clear-on-sleep frame—is deliberately unavailable and is marked disabled
in Leaf3 Controls. Native Reader therefore uses DU during continuous scrolling
until Phase 7 provides per-surface motion hints.

Hardware validation must confirm:

- The vendor accepts single and eight-record batches without HWC validation,
  command-parser, or presentation errors.
- Damage coordinates cover every changed pixel in rotation and both
  composition paths.
- Rapid page turns display the newest committed frame without a duplicate
  direct-EBC submission.
- Unsupported capability and injected command failure switch to the bridge
  once and keep the panel usable.
- Manual GC16, interval cleanup, settled Regal, suspend/wake, and the
  `backend bridge` emergency rollback remain reliable.

### Phase 7: Stock-style application and view controls

After the composer-native protocol is stable, expose framework APIs equivalent
to the stock ROM's per-surface E-Ink metadata. Candidate controls include
region waveform selection, transient scrolling mode, dithering and text
enhancement, new-surface lifecycle hints, and cleanup intervals.

Add an application optimization database only after those controls work
reliably. Unknown applications must remain on conservative defaults, and
application hints must not bypass driver pacing or safety limits.

## Hardware gate for every phase

Complete each phase as a separate focused change, then:

1. Run the repository static checks and a full LineageOS build.
2. Flash the matching logical images and regenerated `vbmeta.img`.
3. Stress-scroll Settings and a browser for at least 30 minutes.
4. Exercise tap, fling, keyboard, rotation, screen off/on, suspend, and wake.
5. Confirm ADB stays connected and the device does not fall into
   `QUSB_BULK`/EDL or reboot.
6. Inspect all logcat buffers and the kernel log for crashes, EBC failures,
   SurfaceFlinger failures, and SELinux denials.
7. Compare capture count, comparison time, updated area, cleanup count, and
   minimum submit spacing against the previous phase. For phases 5A and later,
   also compare EBC-gate wait, notification-to-capture,
   notification-to-submit, ioctl time, page-turn count, interval cleanups, and
   settled-Regal updates.

Do not begin the next phase until the current phase passes this gate on real
hardware.

## Safety boundaries

The following approaches are excluded from the production bridge unless new
evidence and a separately gated implementation prove them safe:

- Active virtual-display mirroring alongside direct `/dev/ebc` submission.
- Multiple raw `EBC_SEND_UPDATE` ioctls for one frame.
- Content-aware quality passes on the direct-EBC path.
- Unverified SurfaceFlinger latch or buffer-count overrides.
- Guessed grayscale EBC buffer formats.
- Replacing preserved vendor display components with unmatched stock
  binaries.

The stock vendor partition remains preserved. Stock optimizations should be
ported by reproducing understood protocols in matching LineageOS source, not
by copying binary framework components.
