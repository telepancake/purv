/*
 * purva.h - the AOT evaluator's interface.
 *
 * purva implements purv.h's external shape (RiscvEmulatorInit + Loop over the public
 * state) but runs a program that was already transcoded -- it does no decoding or
 * transcoding itself. The host transcodes the code once (transcode.c) and installs
 * the result with RiscvEmulatorSetProgram before running RiscvEmulatorLoop.
 */
#ifndef PURVA_H_
#define PURVA_H_

#include "../purv.h"
#include "transcode.h"

/* Install the transcoded program the evaluator runs. Call once after transcode(),
 * before RiscvEmulatorLoop. The pointed-to Transcoded must outlive the run. */
void RiscvEmulatorSetProgram(const Transcoded *prog);

#endif /* PURVA_H_ */
