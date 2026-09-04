/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom tracing header pulled in by Zephyr when CONFIG_TRACING_CUSTOM=y.
 * It is included at the end of <zephyr/tracing/tracing.h>, before
 * tracing_hooks.h, which back-fills any undefined hook with a no-op.
 */

#ifndef ZEPHYR_CUSTOM_TRACING_H_
#define ZEPHYR_CUSTOM_TRACING_H_

/*
 * When CONFIG_TRACING_CUSTOM is selected, tracing.h does not take the default
 * branch that declares the sys_trace_isr_* / sys_trace_idle* entry points used
 * by the arch idle paths. tracing_none.c still provides their weak definitions,
 * so declare them here. This mirrors the declarations in tracing.h's default
 * branch.
 */
void sys_trace_isr_enter(void);
void sys_trace_isr_exit(void);
void sys_trace_isr_exit_to_scheduler(void);
void sys_trace_idle(void);
void sys_trace_idle_exit(void);

/*
 * The default branch of tracing.h defines these system-init hooks as no-op
 * macros; the CONFIG_TRACING_CUSTOM branch does not. Define them here so the
 * kernel init code compiles unchanged.
 */
#define sys_trace_sys_init_enter(entry, level)
#define sys_trace_sys_init_exit(entry, level, result)

/*
 * Define a custom trace hook for the "silly" audio demonstrations. A tracing
 * backend can override it, or it can be used to hook into custom tools. The
 * #ifndef behavior in tracing_hooks.h ensures our definition wins.
 */
#ifndef sys_port_trace_silly_audio_sample
#define sys_port_trace_silly_audio_sample(sample_index, value) \
	do { \
		(void)(sample_index); \
		(void)(value); \
	} while (0)
#endif

#endif /* ZEPHYR_CUSTOM_TRACING_H_ */
