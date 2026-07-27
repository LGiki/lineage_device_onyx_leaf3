# Leaf3 E-Ink Optimization Roadmap

This roadmap orders future E-Ink work by device risk. Each phase must remain
independently reversible and pass its hardware gate before work proceeds to
the next phase. Stability takes priority over matching every stock-ROM feature
at once.

## Current baseline

The production display path uses periodic `ScreenshotClient` capture and one
rate-limited EBC update rectangle per changed frame. The following safe
optimizations are already present:

- 32-pixel tile damage detection with sparse copies into the persistent EBC
  buffer and previous-frame cache.
- Touch-slop scroll detection, with row-hash detection as a fallback, and A2
  updates only for sufficiently large scrolling regions.
- Regional post-scroll GC16 cleanup after the display becomes quiet.
- Blocking display-off waits and property-value caching.
- Disabled wallpaper, doze/AOD drawing, automatic brightness, touch ripples,
  edge glow, and navigation-bar scrims.
- Optional global grayscale and user-selected waveform profiles.
- A 100 ms minimum interval between EBC ioctls.

Virtual-display capture and content-aware direct-EBC waveform selection remain
quarantined because both caused crashes under composition or scrolling load.

## Phases

| Phase | Status | Goal | Risk |
| --- | --- | --- | --- |
| 0 | Complete | Stabilize and optimize the screenshot/EBC bridge | Low |
| 1 | Validation | Wake screenshot capture from a SurfaceFlinger frame notification | Low to medium |
| 2 | Planned | Deliver compositor damage with the notification | Medium |
| 3 | Planned | Add conservative automatic application profiles | Low |
| 4 | Planned | Make grayscale the default for new installations | Low |
| 5 | Planned | Add update-count-based regional ghost cleanup | Medium |
| 6 | Research | Port the stock composer-native EPDC protocol | High |
| 7 | Research | Expose stock-style per-surface E-Ink controls | High |

### Phase 1: SurfaceFlinger frame notification

The opt-in notification implementation is present. It remains in validation
status until it passes the hardware gate below; polling is still the default.

Add a small, LineageOS 18.1-specific SurfaceFlinger integration that signals a
nonblocking `eventfd` after a physical-display frame is committed. The bridge
will wait for that notification, capture only the latest primary frame, and
retain the existing 100 ms EBC pacing.

This phase does not create a virtual display, modify the vendor composer, or
send additional EBC ioctls. Registration failure, unsupported builds, or a
stalled notifier must automatically fall back to the current polling path.
The notification path should initially require an explicit opt-in property.

Success criteria:

- Idle screenshot captures and comparisons fall close to zero.
- Touch, fling, keyboard, rotation, wake, and animation changes are not lost.
- Notification bursts coalesce without building a queue of stale frames.
- A missing or failed notifier returns to polling without losing the panel.
- Settings and browser scrolling pass the hardware stress gate.

### Phase 2: SurfaceFlinger damage delivery

Extend the frame notification to include the dirty display region or a bounded
set of dirty regions. Use that information to limit screenshot comparison and
copy work.

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

Track fast regional updates and schedule a regional GC16 cleanup after a
configurable threshold, in addition to the existing quiet-period cleanup.
Reset the counter only for the cleaned region, and merge overlapping dirty
regions conservatively.

Automatic ghost management must never request a full-screen cleanup. Manual
cleanup policy must continue to disable automatic cleanup, and every cleanup
must obey the global EBC ioctl interval.

### Phase 6: Stock composer-native EPDC protocol

Reverse engineer and implement the ONYX `CommitEpdc` command used between
SurfaceFlinger and the preserved Qualcomm composer service. Confirm the exact
command opcode, header and batch layout, coordinate convention, waveform
values, and queue lifetime before sending any command on hardware.

This work requires coordinated framework, `libgui`, HWC2 command-writer, and
graphics-common changes. It must be feature-gated, match the preserved vendor
ABI exactly, and never run simultaneously with bridge-driven EBC submission.
A boot property must provide an immediate rollback to the known-good bridge.

Only this composer-native path should reconsider:

- Multiple panel update rectangles in one vendor-supported batch.
- Content-aware per-region waveform and dithering.
- Eliminating screenshot readback entirely.
- Vendor-native update scheduling and synchronization.

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
   minimum submit spacing against the previous phase.

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
