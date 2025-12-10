/*
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <odyssey.h>

#include <sys/file.h>
#include <unistd.h>

#include <machinarium/machinarium.h>

#include <watchdog.h>
#include <restart_sync.h>
#include <instance.h>
#include <system.h>
#include <global.h>
#include <od_memory.h>
#include <debugprintf.h>

/*
 * Watchdog Finite State Machine for Online Restart
 * =================================================
 * 
 * States:
 *   INIT              - Initial state, opening lock files
 *   ACQUIRING_CTRL    - Attempting to acquire control lock
 *   ACQUIRING_EXEC    - Attempting to acquire execution lock (holds ctrl lock)
 *   MONITORING        - Monitoring for new instances (holds exec lock only)
 *   SHUTDOWN_HANDOFF  - Releasing locks and shutting down
 *   ERROR             - Error state, shutting down
 *   
 * Transitions:
 *   INIT → ACQUIRING_CTRL               (lock files opened)
 *   INIT → ERROR                        (failed to open lock files)
 *   ACQUIRING_CTRL → ACQUIRING_EXEC     (ctrl lock acquired)
 *   ACQUIRING_EXEC → MONITORING         (exec lock acquired)
 *   MONITORING → SHUTDOWN_HANDOFF       (new instance detected)
 *   ERROR → (exit)                      (cleanup and exit)
 *   SHUTDOWN_HANDOFF → (exit)           (graceful shutdown triggered)
 */

typedef enum {
	OD_WATCHDOG_INIT,
	OD_WATCHDOG_ACQUIRING_CTRL,
	OD_WATCHDOG_ACQUIRING_EXEC,
	OD_WATCHDOG_MONITORING,
	OD_WATCHDOG_SHUTDOWN_HANDOFF,
	OD_WATCHDOG_ERROR
} od_watchdog_state_t;

static const char *od_watchdog_state_name(od_watchdog_state_t state)
{
	switch (state) {
	case OD_WATCHDOG_INIT:
		return "INIT";
	case OD_WATCHDOG_ACQUIRING_CTRL:
		return "ACQUIRING_CTRL";
	case OD_WATCHDOG_ACQUIRING_EXEC:
		return "ACQUIRING_EXEC";
	case OD_WATCHDOG_MONITORING:
		return "MONITORING";
	case OD_WATCHDOG_SHUTDOWN_HANDOFF:
		return "SHUTDOWN_HANDOFF";
	case OD_WATCHDOG_ERROR:
		return "ERROR";
	default:
		return "UNKNOWN";
	}
}

void od_watchdog_worker(void *arg)
{
	od_instance_t *instance = arg;
	od_watchdog_state_t state = OD_WATCHDOG_INIT;
	int fd_ctrl = -1;
	int fd_exec = -1;
	int iterations = 0;
	const char *locks_dir = instance->config.locks_dir == NULL ? 
	                        ODYSSEY_DEFAULT_LOCK_DIR : 
	                        instance->config.locks_dir;

	od_log(&instance->logger, "watchdog", NULL, NULL,
	       "watchdog FSM starting, locks_dir=%s", locks_dir);

	/* Main state machine loop */
	while (state != OD_WATCHDOG_SHUTDOWN_HANDOFF && state != OD_WATCHDOG_ERROR) {
		switch (state) {
		case OD_WATCHDOG_INIT:
			/* Open lock files */
			od_log(&instance->logger, "watchdog", NULL, NULL,
			       "FSM: %s", od_watchdog_state_name(state));
			
			fd_ctrl = od_get_control_lock(instance->config.locks_dir);
			if (fd_ctrl == -1) {
				od_error(&instance->logger, "watchdog", NULL, NULL,
				         "failed to create ctrl lock file in %s (errno: %d)",
				         locks_dir, errno);
				state = OD_WATCHDOG_ERROR;
				break;
			}

			fd_exec = od_get_execution_lock(instance->config.locks_dir);
			if (fd_exec == -1) {
				od_error(&instance->logger, "watchdog", NULL, NULL,
				         "failed to create exec lock file in %s (errno: %d)",
				         locks_dir, errno);
				close(fd_ctrl);
				state = OD_WATCHDOG_ERROR;
				break;
			}

			od_log(&instance->logger, "watchdog", NULL, NULL,
			       "lock files created: ctrl(fd=%d) exec(fd=%d)", fd_ctrl, fd_exec);
			
			state = OD_WATCHDOG_ACQUIRING_CTRL;
			iterations = 0;
			break;

		case OD_WATCHDOG_ACQUIRING_CTRL:
			/* Try to acquire control lock (non-blocking) */
			if (iterations == 0) {
				od_log(&instance->logger, "watchdog", NULL, NULL,
				       "FSM: %s → ACQUIRING_CTRL", od_watchdog_state_name(state));
			}
			
			if (flock(fd_ctrl, LOCK_EX | LOCK_NB) == 0) {
				od_log(&instance->logger, "watchdog", NULL, NULL,
				       "control lock acquired after %d iterations", iterations);
				state = OD_WATCHDOG_ACQUIRING_EXEC;
				iterations = 0;
			} else {
				iterations++;
				if (iterations % 10 == 0) {
					od_log(&instance->logger, "watchdog", NULL, NULL,
					       "waiting for control lock (iteration %d, errno=%d)",
					       iterations, errno);
				}
				machine_sleep(ODYSSEY_WATCHDOG_ITER_INTERVAL);
			}
			break;

		case OD_WATCHDOG_ACQUIRING_EXEC:
			/* Try to acquire execution lock (non-blocking) while holding ctrl lock */
			if (iterations == 0) {
				od_log(&instance->logger, "watchdog", NULL, NULL,
				       "FSM: ACQUIRING_CTRL → ACQUIRING_EXEC");
			}
			
			if (flock(fd_exec, LOCK_EX | LOCK_NB) == 0) {
				od_log(&instance->logger, "watchdog", NULL, NULL,
				       "execution lock acquired after %d iterations", iterations);
				od_log(&instance->logger, "watchdog", NULL, NULL,
				       "FSM: ACQUIRING_EXEC → MONITORING");
				
				/* Keep control lock held initially to give new instances time to start */
				state = OD_WATCHDOG_MONITORING;
				iterations = 0;
			} else {
				iterations++;
				if (iterations % 10 == 0) {
					od_log(&instance->logger, "watchdog", NULL, NULL,
					       "waiting for execution lock (iteration %d, errno=%d)",
					       iterations, errno);
				}
				machine_sleep(ODYSSEY_WATCHDOG_ITER_INTERVAL);
			}
			break;

		case OD_WATCHDOG_MONITORING:
			/* Monitor for new instances by trying to acquire control lock */
			iterations++;
			
			/* On first iteration, release ctrl lock to allow new instances to signal */
			if (iterations == 1) {
				flock(fd_ctrl, LOCK_UN | LOCK_NB);
				od_log(&instance->logger, "watchdog", NULL, NULL,
				       "control lock released, ready to detect new instances");
			}
			
			/* Try to acquire ctrl lock - if held by another instance, we detected them */
			if (flock(fd_ctrl, LOCK_EX | LOCK_NB) == -1) {
				/* Control lock is held by new instance - begin handoff */
				od_log(&instance->logger, "watchdog", NULL, NULL,
				       "new instance detected (ctrl lock held, errno=%d) after %d monitoring cycles",
				       errno, iterations);
				od_log(&instance->logger, "watchdog", NULL, NULL,
				       "FSM: MONITORING → SHUTDOWN_HANDOFF");
				state = OD_WATCHDOG_SHUTDOWN_HANDOFF;
			} else {
				/* No new instance yet, release lock and continue monitoring */
				flock(fd_ctrl, LOCK_UN | LOCK_NB);
				
				if (iterations % 20 == 0) {
					od_log(&instance->logger, "watchdog", NULL, NULL,
					       "monitoring: no new instance detected (%d checks)", iterations);
				}
				
				machine_sleep(ODYSSEY_WATCHDOG_ITER_INTERVAL);
			}
			break;

		case OD_WATCHDOG_SHUTDOWN_HANDOFF:
		case OD_WATCHDOG_ERROR:
			/* These states are handled outside the loop */
			break;
		}
	}

	/* Final state handling */
	if (state == OD_WATCHDOG_SHUTDOWN_HANDOFF) {
		od_log(&instance->logger, "watchdog", NULL, NULL,
		       "releasing execution lock for handoff");
		
		flock(fd_exec, LOCK_UN | LOCK_NB);
		close(fd_exec);
		close(fd_ctrl);

		od_log(&instance->logger, "watchdog", NULL, NULL,
		       "handoff complete, requesting graceful shutdown");
		kill(instance->pid.pid, OD_SIG_GRACEFUL_SHUTDOWN);
	} else if (state == OD_WATCHDOG_ERROR) {
		od_log(&instance->logger, "watchdog", NULL, NULL,
		       "FSM: ERROR - shutting down");
		
		if (instance->config.graceful_die_on_errors) {
			kill(instance->pid.pid, OD_SIG_GRACEFUL_SHUTDOWN);
		} else {
			kill(instance->pid.pid, SIGKILL);
		}
	}

	return;
}

od_retcode_t od_watchdog_invoke(od_system_t *system)
{
	od_instance_t *instance = system->global->instance;

	int64_t id = machine_create("watchdog", od_watchdog_worker, instance);
	if (id == -1) {
		od_error(&instance->logger, "cron", NULL, NULL,
			 "failed to start watchdog coroutine");
		return NOT_OK_RESPONSE;
	}
	return OK_RESPONSE;
}
