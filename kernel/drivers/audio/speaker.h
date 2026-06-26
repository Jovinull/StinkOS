/* PC speaker tone generator (PIT channel 2 gated to the speaker). */
#ifndef SPEAKER_H
#define SPEAKER_H

/* Starts a square-wave tone at 'freq' Hz on the PC speaker. A frequency of 0
 * silences the speaker. Non-blocking: the caller controls how long it sounds. */
void speaker_play(unsigned int freq);

#endif
