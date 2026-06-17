// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#ifndef BYPASS_OUTPUT_CD4053_WITH_MUTE_H__
#define BYPASS_OUTPUT_CD4053_WITH_MUTE_H__

#if defined(__AVR__)
#  include <avr/io.h>
#endif


// two control pins to the CD4053/TMUX4053, to enable mute-before-switch
// capability
#define CD4053_CTL1 (PB2)
#define CD4053_CTL2 (PB3)

// how long to mute the effect before switching between effect/bypass
#define CD4053_MUTE_DELAY_MS (5U)


#endif // BYPASS_OUTPUT_CD4053_WITH_MUTE_H__

