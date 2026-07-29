/*
 * SPDX-License-Identifier: Apache-2.0
 */

package android.view;

import android.app.ActivityThread;
import android.graphics.Rect;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.os.SystemProperties;

import java.lang.ref.WeakReference;

/**
 * Device-private E-Ink policy hooks installed into the Leaf3 framework.
 *
 * @hide
 */
public final class Leaf3EinkHelper {
    private static final String ACTIVE_PACKAGE = "sys.leaf3.active_package";
    private static final String ACTIVE_UID = "sys.leaf3.active_uid";
    private static final String FILTER_ANIMATIONS =
            "sys.leaf3.active_animation_filter";
    private static final String SCROLL_DETECT =
            "persist.sys.leaf3.scroll_detect";
    private static final String SURFACE_FLINGER = "SurfaceFlinger";
    private static final String SURFACE_COMPOSER_TOKEN =
            "android.ui.ISurfaceComposer";
    private static final int TRANSIENT_HINT_TRANSACTION = 1038;
    private static final int TRANSIENT_HINT_VERSION = 1;
    private static final int TRANSIENT_HINT_CLEAR = 0;
    private static final int TRANSIENT_HINT_SET = 1;
    private static final int TRANSIENT_HINT_DURATION_MS = 700;
    private static final long FLING_HINT_WINDOW_MS = 5000;
    private static final long FLING_RENEW_INTERVAL_MS = 250;
    private static final long FLING_QUIET_TIMEOUT_MS = 350;
    private static final long POLICY_CACHE_MS = 200;
    private static final int PENDING_HINT_NONE = 0;
    private static final int PENDING_HINT_CLEAR = 1;
    private static final int PENDING_HINT_SET = 2;

    private static float sDownRawX;
    private static float sDownRawY;
    private static long sPolicyCheckedAt;
    private static boolean sForegroundProcess;
    private static boolean sAnimationsFiltered;
    private static boolean sTransientHintsEnabled;
    private static IBinder sSurfaceFlinger;
    private static final Rect sPendingTransientHint = new Rect();
    private static int sPendingHintCommand = PENDING_HINT_NONE;
    private static WeakReference<View> sPendingHintView =
            new WeakReference<>(null);
    private static WeakReference<View> sLastHintView =
            new WeakReference<>(null);
    private static boolean sHintFrameScheduled;
    private static boolean sGestureHinted;
    private static int sFlingGeneration;
    private static long sFlingDeadline;
    private static long sLastFlingRenewal;
    private static long sLastFlingMotion;
    private static long sFlingMotionSignature;
    private static WeakReference<View> sFlingView = new WeakReference<>(null);
    private static ViewTreeObserver.OnPreDrawListener sFlingDrawListener;

    private static final Choreographer.FrameCallback HINT_FRAME_CALLBACK =
            frameTimeNanos -> dispatchPendingHint();

    private Leaf3EinkHelper() {}

    public static boolean animationsFilteredForProcess() {
        refreshPolicy();
        return sAnimationsFiltered;
    }

    private static void refreshPolicy() {
        final long now = SystemClock.uptimeMillis();
        if (now - sPolicyCheckedAt < POLICY_CACHE_MS) {
            return;
        }
        sPolicyCheckedAt = now;
        final String packageName = ActivityThread.currentPackageName();
        sForegroundProcess = packageName != null &&
                Integer.toHexString(packageName.hashCode()).equals(
                        SystemProperties.get(ACTIVE_PACKAGE, "")) &&
                Process.myUid() == SystemProperties.getInt(ACTIVE_UID, -1);
        sAnimationsFiltered = sForegroundProcess &&
                SystemProperties.getBoolean(FILTER_ANIMATIONS, false);
        sTransientHintsEnabled = sForegroundProcess &&
                SystemProperties.getBoolean(SCROLL_DETECT, true);
    }

    public static void noteTouchDispatch(View view, MotionEvent event,
            boolean handled) {
        refreshPolicy();
        if (!sTransientHintsEnabled || view == null || event == null) {
            return;
        }
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                sDownRawX = event.getRawX();
                sDownRawY = event.getRawY();
                sGestureHinted = false;
                stopFlingRenewal();
                queueTransientHintClear();
                break;
            case MotionEvent.ACTION_MOVE:
                if (!handled) {
                    break;
                }
                final int slop = ViewConfiguration.get(view.getContext())
                        .getScaledTouchSlop();
                if (Math.abs(event.getRawX() - sDownRawX) < slop &&
                        Math.abs(event.getRawY() - sDownRawY) < slop) {
                    break;
                }
                final Rect visible = new Rect();
                if (view.getGlobalVisibleRect(visible) && !visible.isEmpty()) {
                    sGestureHinted = true;
                    queueTransientHint(view, visible);
                }
                break;
            case MotionEvent.ACTION_CANCEL:
                sGestureHinted = false;
                queueTransientHintClear();
                break;
            case MotionEvent.ACTION_UP:
                if (sGestureHinted) {
                    startFlingRenewal();
                }
                sGestureHinted = false;
                break;
            default:
                break;
        }
    }

    private static void queueTransientHintClear() {
        sPendingHintCommand = PENDING_HINT_CLEAR;
        sPendingTransientHint.setEmpty();
        sPendingHintView.clear();
        sLastHintView.clear();
        scheduleHintFrame();
    }

    private static void queueTransientHint(View view, Rect region) {
        if (sPendingHintCommand != PENDING_HINT_SET) {
            sPendingTransientHint.set(region);
            sPendingHintView = new WeakReference<>(view);
            sPendingHintCommand = PENDING_HINT_SET;
        } else if (Rect.intersects(sPendingTransientHint, region) &&
                area(region) < area(sPendingTransientHint)) {
            // Dispatch unwinds from the handled child through its ancestors.
            // Keep the smallest overlapping view for this input event.
            sPendingTransientHint.set(region);
            sPendingHintView = new WeakReference<>(view);
        } else if (!Rect.intersects(sPendingTransientHint, region)) {
            // A distinct dispatch later in the same frame supersedes the old
            // candidate instead of merging unrelated view regions.
            sPendingTransientHint.set(region);
            sPendingHintView = new WeakReference<>(view);
        }
        scheduleHintFrame();
    }

    private static long area(Rect region) {
        return (long) region.width() * region.height();
    }

    private static void scheduleHintFrame() {
        if (sHintFrameScheduled) {
            return;
        }
        sHintFrameScheduled = true;
        Choreographer.getInstance().postFrameCallback(HINT_FRAME_CALLBACK);
    }

    private static void dispatchPendingHint() {
        sHintFrameScheduled = false;
        if (sPendingHintCommand == PENDING_HINT_NONE) {
            return;
        }
        if (sPendingHintCommand == PENDING_HINT_CLEAR) {
            sLastHintView.clear();
            transactTransientHint(null);
        } else {
            final Rect region = new Rect(sPendingTransientHint);
            final View view = sPendingHintView.get();
            if (view != null) {
                sLastHintView = new WeakReference<>(view);
            }
            transactTransientHint(region);
        }
        sPendingHintCommand = PENDING_HINT_NONE;
        sPendingTransientHint.setEmpty();
        sPendingHintView.clear();
    }

    private static void startFlingRenewal() {
        stopFlingRenewal();
        final View view = sPendingHintCommand == PENDING_HINT_SET
                ? sPendingHintView.get() : sLastHintView.get();
        if (view == null || !view.isAttachedToWindow()) {
            return;
        }

        final int generation = ++sFlingGeneration;
        final long startNow = SystemClock.uptimeMillis();
        sFlingDeadline = startNow + FLING_HINT_WINDOW_MS;
        sLastFlingRenewal = 0;
        sLastFlingMotion = startNow;
        sFlingMotionSignature = viewMotionSignature(view);
        sFlingView = new WeakReference<>(view);
        sFlingDrawListener = () -> {
            if (generation != sFlingGeneration) {
                return true;
            }
            refreshPolicy();
            if (!sTransientHintsEnabled) {
                stopFlingRenewal();
                return true;
            }
            final long now = SystemClock.uptimeMillis();
            final View flingView = sFlingView.get();
            if (flingView == null || !flingView.isAttachedToWindow() ||
                    now >= sFlingDeadline) {
                stopFlingRenewal();
                return true;
            }
            final long signature = viewMotionSignature(flingView);
            if (signature == sFlingMotionSignature) {
                if (now - sLastFlingMotion >= FLING_QUIET_TIMEOUT_MS) {
                    stopFlingRenewal();
                }
                return true;
            }
            sFlingMotionSignature = signature;
            sLastFlingMotion = now;
            if (now - sLastFlingRenewal >= FLING_RENEW_INTERVAL_MS) {
                final Rect visible = new Rect();
                if (flingView.getGlobalVisibleRect(visible) &&
                        !visible.isEmpty()) {
                    sLastFlingRenewal = now;
                    queueTransientHint(flingView, visible);
                }
            }
            return true;
        };
        view.getViewTreeObserver().addOnPreDrawListener(sFlingDrawListener);
        view.postDelayed(() -> {
            if (generation == sFlingGeneration) {
                stopFlingRenewal();
            }
        }, FLING_HINT_WINDOW_MS);
    }

    private static long viewMotionSignature(View view) {
        long signature = 17;
        signature = signature * 31 + view.getScrollX();
        signature = signature * 31 + view.getScrollY();
        if (!(view instanceof ViewGroup)) {
            return signature;
        }
        final ViewGroup group = (ViewGroup) view;
        signature = signature * 31 + group.getChildCount();
        for (int index = 0; index < group.getChildCount(); ++index) {
            final View child = group.getChildAt(index);
            signature = signature * 31 + System.identityHashCode(child);
            signature = signature * 31 + child.getLeft();
            signature = signature * 31 + child.getTop();
            signature = signature * 31 + child.getRight();
            signature = signature * 31 + child.getBottom();
            signature = signature * 31 +
                    Float.floatToIntBits(child.getTranslationX());
            signature = signature * 31 +
                    Float.floatToIntBits(child.getTranslationY());
        }
        return signature;
    }

    private static void stopFlingRenewal() {
        ++sFlingGeneration;
        final View view = sFlingView.get();
        if (view != null && sFlingDrawListener != null) {
            final ViewTreeObserver observer = view.getViewTreeObserver();
            if (observer.isAlive()) {
                observer.removeOnPreDrawListener(sFlingDrawListener);
            }
        }
        sFlingDrawListener = null;
        sFlingView.clear();
        sFlingDeadline = 0;
        sLastFlingRenewal = 0;
        sLastFlingMotion = 0;
        sFlingMotionSignature = 0;
    }

    private static void transactTransientHint(Rect region) {
        IBinder surfaceFlinger = sSurfaceFlinger;
        if (surfaceFlinger == null || !surfaceFlinger.isBinderAlive()) {
            surfaceFlinger = ServiceManager.checkService(SURFACE_FLINGER);
            sSurfaceFlinger = surfaceFlinger;
        }
        if (surfaceFlinger == null) {
            return;
        }

        final Parcel data = Parcel.obtain();
        try {
            data.writeInterfaceToken(SURFACE_COMPOSER_TOKEN);
            data.writeInt(TRANSIENT_HINT_VERSION);
            data.writeInt(region == null ? TRANSIENT_HINT_CLEAR
                                         : TRANSIENT_HINT_SET);
            if (region != null) {
                data.writeInt(region.left);
                data.writeInt(region.top);
                data.writeInt(region.right);
                data.writeInt(region.bottom);
                data.writeInt(TRANSIENT_HINT_DURATION_MS);
            }
            surfaceFlinger.transact(TRANSIENT_HINT_TRANSACTION, data, null,
                    IBinder.FLAG_ONEWAY);
        } catch (RemoteException exception) {
            sSurfaceFlinger = null;
        } finally {
            data.recycle();
        }
    }
}
