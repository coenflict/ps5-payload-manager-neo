#ifndef BOOT_GUARD_H
#define BOOT_GUARD_H

/* Crash guard for freshly updated payloads.
 *
 * A kernel-level payload that breaks after an update usually takes the whole
 * console down, so the evidence is the next boot: the guard file still says an
 * updated payload was being launched (or the sequence was still in its
 * probation window). Startup then restores the previous version of each
 * updated payload launched in that boot and skips the version that failed.
 * Unchanged payloads are never touched, and nothing is written unless an
 * updated payload is launched. */

#ifndef BOOT_GUARD_PROBATION_SECONDS
#define BOOT_GUARD_PROBATION_SECONDS 90
#endif

/* Startup, before autoload. Pass 1 when a running manager instance was just
 * replaced: the previous boot did not crash, so its record is discarded.
 * Returns the number of payloads rolled back. */
int boot_guard_evaluate(int replaced_running_instance);
void boot_guard_updated(const char *slot_key, const char *version);
void boot_guard_launching(const char *slot_key);
void boot_guard_sequence_done(void);
void boot_guard_tick(void); /* main loop: ends the probation window */

#endif
