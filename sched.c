#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/monitor.h>
#include <kern/sched.h>


// Choose a user environment to run and run it.
void
sched_yield(void)
{
#if defined (PRIORITY_SCHED)

	priority_sched_yield();

#else

	// 
        // Implement simple round-robin scheduling.
	// Search through 'envs' for a runnable environment,
	// in circular fashion starting after the previously running env,
	// and switch to the first such environment found.
	// It's OK to choose the previously running env if no other env
	// is runnable.
	// But never choose envs[0], the idle environment,
	// unless NOTHING else is runnable.
        //

	int i, env;
	
	if (!curenv) {
            //
            // We come here in 2 cases
            //
            // 1. When the kernel starts up and calls sched_yield to run the 
            //    first available environment. In this case, we have to run
            //    the idle process.
            // 2. When one environment exits gracefully and we have to run the
            //    next available environment. In this case, we have to loop
            //    through the available environments and pick the next
            //    available one.
            //
	    if (prev_curenv_id == 0) {
		goto end;
	    } else {
		env = ENVX(prev_curenv_id);
	    }
	} else {
            //
            // We come here when one environment yields the CPU. Here, we loop
            // through the available environments and pick the next one
            //
	    env = ENVX(curenv->env_id);
	}

        // Loop through the available environments in a circular fashion
	for (i = (env + 1) % NENV; i < NENV; i = (i + 1) % NENV) {

	    if (i == env) {
                //
                // If we come back to the same environment we started with,
                // it means there are no other runnable environments. Here,
                // we run the same environment again if its runnable. We run
                // the idle process otherwise.
                //
		if (curenv && curenv->env_status == ENV_RUNNABLE) {
		    env_run(curenv);
		} else {
		    goto end;
		}
	    } else if (i == 0) {
                //
                // We don't run the idle process until we are sure there are
                // no other runnable environments
                //
		continue;
	    } else {
                // See if this environment is runnable and run it
		if (envs[i].env_status == ENV_RUNNABLE) {
		    env_run(&envs[i]);
		}
	    }
	}

end:
	// Run the special idle environment when nothing else is runnable.
	if (envs[0].env_status == ENV_RUNNABLE)
		env_run(&envs[0]);
	else {
		cprintf("Destroyed all environments - nothing more to do!\n");
		while (1)
			monitor(NULL);
	}

#endif // PRIORITY_SCHED
}

void
priority_sched_yield(void)
{
	int i, env, min_priority, min_priority_index;
	
	//
	// This function implements a fixed priority scheduling policy in the
	// kernel. Whenever the kernel has to choose a user environment to
	// run, it goes through the list of available environments and choose
	// the one with the maximum priority. Here, lower value indicates
	// higher priority.
	//
	// To test this function, we have modified Env struct to store the
	// priority associated with an environment in the field 'priority'.
	//
	// Here we are setting the priority of the environment in a static
	// fashion. Ideally, ENV_CREATE has to be modified to pass an
	// additional priority field to env_create. But as of now, the
	// priority of an environment is hardcoded in the env_alloc() routine
	// and its equal to the index of that environment in the envs array
	// i.e. priority(envs[5]) = 5.
	//
	// 3 new files have been added to the user directory to test this:
	// - sched_prio1.c
	// - sched_prio2.c
	// - sched_prio3.c
	//

	if (!curenv) {
            //
            // We come here in 2 cases
            //
            // 1. When the kernel starts up and calls sched_yield to run the 
            //    first available environment. In this case, we have to run
            //    the idle process.
            // 2. When one environment exits gracefully and we have to run the
            //    next available environment. In this case, we have to loop
            //    through the available environments and pick the next
            //    available one.
            //
	    if (prev_curenv_id == 0) {
		goto end;
	    } else {
		env = ENVX(prev_curenv_id);
	    }
	} else {
            //
            // We come here when one environment yields the CPU. Here, we loop
            // through the available environments and pick the next one
            //
	    env = ENVX(curenv->env_id);
	}

	// To keep track of environment with maximum priority
	min_priority = 10000;
	min_priority_index = env;

        // Loop through the available environments in a circular fashion
	for (i = (env + 1) % NENV; i < NENV; i = (i + 1) % NENV) {

	    if (i == env) {
                //
                // If we come back to the same environment we started with,
                // it means we have scanned all the environments. If we have
		// another environment with maximum priority, we run it. Else,
		// we run the same environment which called yield, if its
		// runnable or run the idle environment.
                //
		if (min_priority_index != env) {
		    break;
		} else {
		    if (curenv && curenv->env_status == ENV_RUNNABLE) {
			break;
		    } else {
			goto end;
		    }
		}
	    } else if (i == 0) {
                //
                // We don't run the idle process until we are sure there are
                // no other runnable environments
                //
		continue;
	    } else {
                // See if this environment is runnable
		if (envs[i].env_status == ENV_RUNNABLE) {
		    if (envs[i].priority < min_priority) {
			//
			// Check if this has the maximum priority. If so,
			// remember it.
			//
			min_priority = envs[i].priority;
			min_priority_index = i;
		    }
		}
	    }
	}

	// Run the environment with the maximum priority
	if (envs[min_priority_index].env_status == ENV_RUNNABLE) {
	    env_run(&envs[min_priority_index]);
	}

end:
	// Run the special idle environment when nothing else is runnable.
	if (envs[0].env_status == ENV_RUNNABLE)
		env_run(&envs[0]);
	else {
		cprintf("Destroyed all environments - nothing more to do!\n");
		while (1)
			monitor(NULL);
	}
}

