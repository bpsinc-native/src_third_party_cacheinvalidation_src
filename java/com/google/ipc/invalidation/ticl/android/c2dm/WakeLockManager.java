/*
 * Copyright 2011 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.google.ipc.invalidation.ticl.android.c2dm;

import com.google.common.base.Preconditions;

import android.content.Context;
import android.os.PowerManager;
import android.os.PowerManager.WakeLock;
import android.util.Log;

import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;

/**
 * Singleton that manages wake locks identified by a key. Wake locks are refcounted so if they are
 * acquired multiple times with the same key they will not unlocked until they are released an
 * equivalent number of times.
 */
public class WakeLockManager {
  /** Logging tag. */
  private static final String TAG = "WakeLockMgr";

  /** Lock over all state. Must be acquired by all non-private methods. */
  private static final Object LOCK = new Object();

  /** Singleton instance. */
  private static WakeLockManager theManager;

  /** Wake locks by key. */
  private final Map<Object, PowerManager.WakeLock> wakeLocks =
      new HashMap<Object, PowerManager.WakeLock>();

  private final PowerManager powerManager;

  private final Context applicationContext;

  private WakeLockManager(Context context) {
    powerManager = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
    applicationContext = Preconditions.checkNotNull(context);
  }

  /** Returns the wake lock manager. */
  public static WakeLockManager getInstance(Context context) {
    synchronized (LOCK) {
      if (theManager == null) {
        theManager = new WakeLockManager(context.getApplicationContext());
      } else {
        Preconditions.checkState(theManager.applicationContext == context.getApplicationContext(),
            "Provided context %s does not match stored context %s",
            context.getApplicationContext(), theManager.applicationContext);
      }
      return theManager;
    }
  }

  /**
   * Acquires a wake lock identified by the {@code key}.
   */
  public void acquire(Object key) {
    synchronized (LOCK) {
      cleanup();
      Preconditions.checkNotNull(key, "Key can not be null");
      log(key, "acquiring");
      getWakeLock(key).acquire();
    }
  }

  /**
   * Acquires a wake lock identified by the {@code key} that will be automatically released after at
   * most {@code timeoutMs}.
   */
  public void acquire(Object key, int timeoutMs) {
    synchronized (LOCK) {
      cleanup();
      Preconditions.checkNotNull(key, "Key can not be null");
      log(key, "acquiring with timeout " + timeoutMs);
      getWakeLock(key).acquire(timeoutMs);
    }
  }

  /**
   * Releases the wake lock identified by the {@code key} if it is currently held.
   */
  public void release(Object key) {
    synchronized (LOCK) {
      cleanup();
      Preconditions.checkNotNull(key, "Key can not be null");
      PowerManager.WakeLock wakelock = getWakeLock(key);

      // If the lock is not held, this is a bogus release.
      if (!wakelock.isHeld()) {
        Log.w(TAG, "Over-release of wakelock: " + key);
        return;
      }
      // We hold the lock, so we can safely release it.
      wakelock.release();
      log(key, "released");

      // Now if the lock is not held, that means we were the last holder, so we should remove it
      // from the map.
      if (!wakelock.isHeld()) {
        wakeLocks.remove(key);
        log(key, "freed");
      }
    }
  }

  /**
   * Returns whether there is currently a wake lock held for the provided {@code key}.
   */
  public boolean isHeld(Object key) {
    synchronized (LOCK) {
      cleanup();
      Preconditions.checkNotNull(key, "Key can not be null");
      if (!wakeLocks.containsKey(key)) {
        return false;
      }
      return getWakeLock(key).isHeld();
    }
  }

  /** Returns whether the manager has any active (held) wakelocks. */
  
  public boolean hasWakeLocks() {
    synchronized (LOCK) {
      cleanup();
      return !wakeLocks.isEmpty();
    }
  }

  /** Discards (without releasing) all wakelocks. */
  
  public void resetForTest() {
    synchronized (LOCK) {
      cleanup();
      wakeLocks.clear();
    }
  }

  /**
   * Returns a wakelock to use for {@code key}. If a lock is already present in the map,
   * returns that lock. Else, creates a new lock, installs it in the map, and returns it.
   * <p>
   * REQUIRES: caller must hold {@link #LOCK}.
   */
  private PowerManager.WakeLock getWakeLock(Object key) {
    if (key == null) {
      throw new IllegalArgumentException("Key can not be null");
    }
    PowerManager.WakeLock wakeLock = wakeLocks.get(key);
    if (wakeLock == null) {
      wakeLock = powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, key.toString());
      wakeLocks.put(key, wakeLock);
    }
    return wakeLock;
  }

  /**
   * Removes any non-held wakelocks from {@link #wakeLocks}. Such locks may be present when a
   * wakelock acquired with a timeout is not released before the timeout expires. We only explicitly
   * remove wakelocks from the map when {@link #release} is called, so a timeout results in a
   * non-held wakelock in the map.
   * <p>
   * Must be called as the first line of all non-private methods.
   * <p>
   * REQUIRES: caller must hold {@link #LOCK}.
   */
  private void cleanup() {
    Iterator<Map.Entry<Object, WakeLock>> wakeLockIter = wakeLocks.entrySet().iterator();

    // Check each map entry.
    while (wakeLockIter.hasNext()) {
      Map.Entry<Object, WakeLock> wakeLockEntry = wakeLockIter.next();
      if (!wakeLockEntry.getValue().isHeld()) {
        // Warn and remove the entry from the map if the lock is not held.
        Log.w(TAG, "Found un-held wakelock '" + wakeLockEntry.getKey() + "' -- timed-out?");
        wakeLockIter.remove();
      }
    }
  }

  /** Logs a debug message that {@code action} has occurred for {@code key}. */
  private static void log(Object key, String action) {
    // TODO:
    Log.d(TAG, "WakeLock " + action + " for key: {" + key + "}");
  }
}
