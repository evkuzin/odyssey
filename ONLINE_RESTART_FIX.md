# Online Restart Bug Fix

## Issue

The online restart feature had a critical bug that prevented multiple consecutive restarts from working correctly.

### Original Behavior

1. Instance A starts, acquires control and execution locks
2. Instance B starts, waits for execution lock
3. Instance A detects B, shuts down gracefully
4. Instance B acquires execution lock BUT never releases it
5. **Instance A restarts → waits forever for execution lock**
6. **Instance B never detects A's restart**

### Root Cause

The watchdog logic had a **one-way handoff** problem:

- The new instance would acquire the execution lock after the old instance released it
- But it would **never release the execution lock** during normal operation
- It only monitored the control lock
- When the old instance restarted, it couldn't acquire the execution lock
- The current instance had no mechanism to detect this

## The Fix

Modified `sources/watchdog.c` to properly implement a **bidirectional handoff**:

### New Behavior

1. Instance A starts
   - Acquires control lock (blocking)
   - Acquires execution lock (blocking)
   - **Releases control lock**
   - Enters monitoring loop

2. Instance B starts
   - Waits for control lock
   - A's monitoring loop releases control briefly
   - B acquires control lock → **A fails to reacquire → A initiates shutdown**
   - B waits for execution lock
   - A releases execution lock on shutdown
   - **B acquires execution lock**
   - **B releases control lock ← NEW**
   - **B enters monitoring loop ← NEW**

3. Instance A restarts
   - Waits for control lock
   - **B's monitoring loop releases control briefly**
   - **A acquires control lock → B fails to reacquire → B initiates shutdown**
   - A waits for execution lock
   - **B releases execution lock on shutdown**
   - **A acquires execution lock**
   - **A releases control lock and enters monitoring loop**

### Key Changes

```c
// After acquiring execution lock, release control lock and enter monitoring
flock(fd_ctrl, LOCK_UN | LOCK_NB);

od_log(&instance->logger, "watchdog", NULL, NULL,
       "acquired execution lock, now monitoring for new instances");

// Monitor for new instances (same loop as initial instance)
for (;;) {
    if (flock(fd_ctrl, LOCK_EX | LOCK_NB) == -1) {
        od_log(&instance->logger, "watchdog", NULL, NULL,
               "detected new instance starting, initiating graceful shutdown");
        break;
    }
    flock(fd_ctrl, LOCK_UN | LOCK_NB);
    machine_sleep(ODYSSEY_WATCHDOG_ITER_INTERVAL);
}

// Clean up locks properly
flock(fd_exec, LOCK_UN | LOCK_NB);
close(fd_exec);
close(fd_ctrl);
```

## Testing the Fix

### Setup with Two systemd Services

Both services can now have `Restart=always`:

```ini
# /etc/systemd/system/odyssey.service
[Service]
User=odyssey
ExecStart=/usr/bin/odyssey /etc/odyssey/odyssey.conf
Restart=always
RestartSec=30

# /etc/systemd/system/odyssey_new.service  
[Service]
User=odyssey
ExecStart=/usr/bin/odyssey /etc/odyssey/odyssey_new.conf
Restart=always
RestartSec=30
```

### Expected Log Flow

**Instance A starts:**
```
watchdog: acquired execution lock, now monitoring for new instances
```

**Instance B starts:**
```
watchdog: acquired execution lock, now monitoring for new instances
```

**Instance A logs:**
```
watchdog: detected new instance starting, initiating graceful shutdown
system: SIG_GRACEFUL_SHUTDOWN received
config: stop to accepting new connections
```

**Instance A exits, systemd waits 30s, then restarts Instance A:**

**Instance A logs:**
```
watchdog: acquired execution lock, now monitoring for new instances
```

**Instance B logs:**
```
watchdog: detected new instance starting, initiating graceful shutdown
system: SIG_GRACEFUL_SHUTDOWN received
config: stop to accepting new connections
```

**Instance B exits, systemd waits 30s, then restarts Instance B:**

**→ Cycle repeats indefinitely ✓**

## Configuration Requirements

For online restart to work correctly, you need:

```conf
locks_dir "/run/odyssey"
enable_online_restart yes
bindwith_reuseport yes
graceful_shutdown_timeout_ms 30000

online_restart_drop_options {
    drop_enabled yes  # or no, depending on your needs
}
```

**Important:** Both instances must:
- Use the **same `locks_dir`**
- Have `bindwith_reuseport yes`
- Have `enable_online_restart yes`
- Bind to the **same TCP port** (SO_REUSEPORT allows this)

## Additional Logging

The fix adds two new log messages:

1. When an instance successfully becomes the current instance:
```
watchdog: acquired execution lock, now monitoring for new instances
```

2. When an instance detects a new instance starting:
```
watchdog: detected new instance starting, initiating graceful shutdown
```

These make it clear what's happening during the handoff process.

## Build and Test

```bash
# Rebuild odyssey
cd /usr/scratch/evgeny/local/odyssey
make clean
make

# Install
sudo make install

# Test with two systemd services
systemctl start odyssey
sleep 5
systemctl start odyssey_new

# Watch logs
journalctl -f -u odyssey -u odyssey_new

# You should see proper handoffs every 30 seconds
```
