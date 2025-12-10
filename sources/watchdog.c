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

void od_watchdog_worker(void *arg)
{
	od_instance_t *instance = arg;

	const char *locks_dir = instance->config.locks_dir == NULL ? ODYSSEY_DEFAULT_LOCK_DIR : instance->config.locks_dir;
	od_log(&instance->logger, "watchdog", NULL, NULL,
	       "watchdog worker started, locks_dir=%s", locks_dir);

	int fd_ctrl = od_get_control_lock(instance->config.locks_dir);
	if (fd_ctrl == -1) {
		od_error(
			&instance->logger, "watchdog", NULL, NULL,
			"failed to create ctrl lock file in %s (errno: %d) try to "
			"specify another locks dir or disable online restart feature",
			instance->config.locks_dir == NULL ?
				ODYSSEY_DEFAULT_LOCK_DIR :
				instance->config.locks_dir,
			errno);

		if (instance->config.graceful_die_on_errors) {
			kill(instance->pid.pid, OD_SIG_GRACEFUL_SHUTDOWN);
		} else {
			kill(instance->pid.pid, SIGKILL);
		}
		return;
	}
	int fd_exec = od_get_execution_lock(instance->config.locks_dir);
	if (fd_exec == -1) {
		od_log(&instance->logger, "watchdog", NULL, NULL,
		       "failed to create exec lock file in %s (errno: %d) try to "
		       "specify another locks dir or disable online restart feature",
		       instance->config.locks_dir == NULL ?
			       ODYSSEY_DEFAULT_LOCK_DIR :
			       instance->config.locks_dir,
		       errno);
		if (instance->config.graceful_die_on_errors) {
			kill(instance->pid.pid, OD_SIG_GRACEFUL_SHUTDOWN);
		} else {
			kill(instance->pid.pid, SIGKILL);
		}
		close(fd_ctrl);
		return;
	}

	/* Log the actual lock file paths for verification */
	char ctrl_lock_path[ODYSSEY_LOCK_MAXPATH];
	char exec_lock_path[ODYSSEY_LOCK_MAXPATH];
	snprintf(ctrl_lock_path, sizeof(ctrl_lock_path), "%s/%s:%d", 
	         locks_dir, ODYSSEY_LOCK_PREFIX, ODYSSEY_CTRL_LOCK_HASH);
	snprintf(exec_lock_path, sizeof(exec_lock_path), "%s/%s:%d", 
	         locks_dir, ODYSSEY_LOCK_PREFIX, ODYSSEY_EXEC_LOCK_HASH);
	
	od_log(&instance->logger, "watchdog", NULL, NULL,
	       "using lock files: ctrl='%s' exec='%s'", ctrl_lock_path, exec_lock_path);

	od_log(&instance->logger, "watchdog", NULL, NULL,
	       "attempting to acquire control lock (fd=%d)", fd_ctrl);
	od_dbg_printf_on_dvl_lvl(1, "try to acquire ctrl lock %d\n", fd_ctrl);
	
	int ctrl_wait_iterations = 0;
	for (;;) {
		if (flock(fd_ctrl, LOCK_EX | LOCK_NB) == 0) {
			od_log(&instance->logger, "watchdog", NULL, NULL,
			       "control lock '%s' acquired successfully after %d iterations", 
			       ctrl_lock_path, ctrl_wait_iterations);
			od_dbg_printf_on_dvl_lvl(1, "acquire ctrl lock ok %d\n",
						 fd_ctrl);
			break;
		}
		ctrl_wait_iterations++;
		if (ctrl_wait_iterations % 10 == 0) {
			od_log(&instance->logger, "watchdog", NULL, NULL,
			       "still waiting for control lock (waited %d iterations, errno=%d)", 
			       ctrl_wait_iterations, errno);
		}
		machine_sleep(ODYSSEY_WATCHDOG_ITER_INTERVAL);
	}

	od_log(&instance->logger, "watchdog", NULL, NULL,
	       "attempting to acquire execution lock (fd=%d)", fd_exec);
	od_dbg_printf_on_dvl_lvl(1, "try to acquire exec lock %d\n", fd_exec);
	
	int exec_wait_iterations = 0;
	for (;;) {
		if (flock(fd_exec, LOCK_EX | LOCK_NB) == 0) {
			od_log(&instance->logger, "watchdog", NULL, NULL,
			       "execution lock '%s' acquired successfully after %d iterations", 
			       exec_lock_path, exec_wait_iterations);
			od_dbg_printf_on_dvl_lvl(1, "acquire exec lock ok %d\n",
						 fd_exec);
			break;
		}
		exec_wait_iterations++;
		if (exec_wait_iterations % 10 == 0) {
			od_log(&instance->logger, "watchdog", NULL, NULL,
			       "still waiting for execution lock (waited %d iterations, errno=%d)", 
			       exec_wait_iterations, errno);
		}
		machine_sleep(ODYSSEY_WATCHDOG_ITER_INTERVAL);
	}

	/*
	 * Keep holding the control lock for one full iteration to ensure
	 * the old instance's monitoring loop detects our presence before we release it.
	 */
	od_log(&instance->logger, "watchdog", NULL, NULL,
	       "holding control lock '%s' for %dms to signal presence to old instance", 
	       ctrl_lock_path, ODYSSEY_WATCHDOG_ITER_INTERVAL);
	machine_sleep(ODYSSEY_WATCHDOG_ITER_INTERVAL);

	/*
	 * Release control lock and start monitoring for new instances.
	 * This allows the next instance to detect us and initiate a handoff.
	 * Keep holding exec lock to maintain exclusive execution rights.
	 */
	od_log(&instance->logger, "watchdog", NULL, NULL,
	       "releasing control lock '%s' to allow new instances to detect us", ctrl_lock_path);
	flock(fd_ctrl, LOCK_UN | LOCK_NB);
	
	/*
	 * After a grace period (30 seconds), release the exec lock to reset state.
	 * This ensures subsequent handoffs work identically to the first handoff,
	 * where the new instance must wait for the exec lock.
	 */
	const int monitoring_check_interval_ms = 100; /* Check 5x per iteration */
	const int exec_lock_grace_period_ms = 30000; /* 30 seconds */
	od_log(&instance->logger, "watchdog", NULL, NULL,
	       "will release execution lock after %dms grace period to reset handoff state", 
	       exec_lock_grace_period_ms);
	
	od_log(&instance->logger, "watchdog", NULL, NULL,
	       "now monitoring for new instances (checking every %dms)", 
	       monitoring_check_interval_ms);

	int monitoring_iterations = 0;
	bool exec_lock_released = false;
	
	for (;;) {
		monitoring_iterations++;
		
		/* After grace period, release exec lock to reset state */
		if (!exec_lock_released && 
		    (monitoring_iterations * monitoring_check_interval_ms >= exec_lock_grace_period_ms)) {
			od_log(&instance->logger, "watchdog", NULL, NULL,
			       "grace period elapsed, releasing execution lock '%s' to reset state", 
			       exec_lock_path);
			flock(fd_exec, LOCK_UN | LOCK_NB);
			exec_lock_released = true;
			od_log(&instance->logger, "watchdog", NULL, NULL,
			       "state reset complete - next handoff will behave identically to first handoff");
		}
		
		int lock_result = flock(fd_ctrl, LOCK_EX | LOCK_NB);
		if (lock_result == -1) {
			/* Another new instance has started, we need to gracefully shutdown */
			od_log(&instance->logger, "watchdog", NULL, NULL,
			       "detected new instance starting (ctrl lock '%s' held by new instance, errno=%d), initiating graceful shutdown", 
			       ctrl_lock_path, errno);
			od_dbg_printf_on_dvl_lvl(1, "failed to acquire ctrl lock (errno: %d), releasing exec lock %d\n",
						 errno, fd_exec);
			break;
		}

		/* We got the lock, which means no new instance is starting yet */
		od_dbg_printf_on_dvl_lvl(1, "ctrl lock acquired in monitoring loop (iteration %d)\n", 
					 monitoring_iterations);
		
		if (monitoring_iterations % 100 == 0) {
			od_log(&instance->logger, "watchdog", NULL, NULL,
			       "still monitoring, no new instance detected (%d checks performed)", 
			       monitoring_iterations);
		}
		
		/* Release immediately - we only need to check if we CAN acquire it */
		flock(fd_ctrl, LOCK_UN | LOCK_NB);

		od_dbg_printf_on_dvl_lvl(1, "watchdog worker sleep for %d ms\n",
					 monitoring_check_interval_ms);
		machine_sleep(monitoring_check_interval_ms);
	}
	
	/* Only release exec lock if we still hold it */
	if (!exec_lock_released) {
		od_log(&instance->logger, "watchdog", NULL, NULL,
		       "releasing execution lock '%s' for handoff to new instance", exec_lock_path);
	}
	flock(fd_exec, LOCK_UN | LOCK_NB);
	close(fd_exec);
	close(fd_ctrl);

	od_log(&instance->logger, "watchdog", NULL, NULL,
	       "watchdog terminating, requesting graceful shutdown of odyssey process");
	
	/* request our own process to shutdown  */
	kill(instance->pid.pid, OD_SIG_GRACEFUL_SHUTDOWN);
	return;
}

od_retcode_t od_watchdog_invoke(od_system_t *system)
{
	od_instance_t *instance = system->global->instance;

	od_log(&instance->logger, "watchdog", NULL, NULL,
	       "starting watchdog coroutine for online restart feature");

	int64_t id = machine_create("watchdog", od_watchdog_worker, instance);
	if (id == -1) {
		od_error(&instance->logger, "watchdog", NULL, NULL,
			 "failed to start watchdog coroutine");
		return NOT_OK_RESPONSE;
	}
	
	od_log(&instance->logger, "watchdog", NULL, NULL,
	       "watchdog coroutine created successfully (id=%ld)", id);
	
	return OK_RESPONSE;
}
