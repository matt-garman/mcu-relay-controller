#ifndef BYPASS_OUTPUT_CD4053_WITH_MUTE_H__
#define BYPASS_OUTPUT_CD4053_WITH_MUTE_H__

#include <avr/io.h>


// two control pins to the CD4053/TMUX4053, to enable mute-before-switch
// capability
#define CD4053_CTL1 (PB2)
#define CD4053_CTL2 (PB3)

// how long to mute the effect before switching between effect/bypass
#define CD4053_MUTE_DELAY_MS (5)


#endif // BYPASS_OUTPUT_CD4053_WITH_MUTE_H__

