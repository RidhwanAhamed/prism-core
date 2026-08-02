/* Pure C11 translation unit: proves include/prism/prism_core.h is genuinely a C header
 * (ffigen and OEM C hosts consume it as C, not C++). Exercises the no-I/O lifecycle. */

#include "prism/prism_core.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  /* Deliberately hard-coded: this is a tripwire for an UNINTENDED ABI version change,
   * so it must be updated by hand in the same commit that moves prism_version(). */
  if (strcmp(prism_version(), "0.2.0") != 0) {
    return 1;
  }
  for (int r = PRISM_OK; r <= PRISM_ERROR_OUT_OF_MEMORY; ++r) {
    if (prism_result_description((prism_result)r) == NULL) {
      return 2;
    }
  }

  prism_core* core = NULL;
  if (prism_create(NULL, &core) != PRISM_OK || core == NULL) {
    return 3; /* NULL config means defaults */
  }
  if (prism_start(core) != PRISM_ERROR_INVALID_STATE) {
    return 4; /* no scene loaded yet */
  }
  if (prism_sample_rate(core) != 0) {
    return 5;
  }
  prism_psv psv;
  if (prism_get_psv(core, &psv) != PRISM_ERROR_INVALID_STATE) {
    return 6; /* nothing emitted before start */
  }
  float frames[64];
  if (prism_render(core, frames, 64) != PRISM_ERROR_INVALID_STATE) {
    return 7; /* fills silence + reports the state */
  }
  for (int i = 0; i < 64; ++i) {
    if (frames[i] != 0.0f) {
      return 8;
    }
  }
  /* A negative length cast to size_t must come back as an error code — never as a C++
   * exception aborting a C host (adversarial review reproduced the SIGABRT). */
  prism_task_deadline one = {0, 1};
  if (prism_report_task_deadlines(core, &one, (size_t)-1) != PRISM_ERROR_INVALID_ARGUMENT) {
    return 9;
  }

  prism_destroy(core);
  prism_destroy(NULL); /* documented no-op */

  if (prism_create(NULL, NULL) != PRISM_ERROR_INVALID_ARGUMENT) {
    return 10;
  }
  printf("C ABI smoke OK\n");
  return 0;
}
