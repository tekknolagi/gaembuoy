#include <stdio.h>
#include "gb.h"

static void gb_spu_frequency_reload(struct gb_spu_divider *f) {
     f->counter = 2 * (0x800U - f->offset);
}

void gb_spu_reset(struct gb *gb) {
     struct gb_spu *spu = &gb->spu;

     spu->enable = true;

     /* NR3 reset */
     spu->nr3.enable = false;
     spu->nr3.running = false;
     spu->nr3.duration.enable = false;
     spu->nr3.volume_shift = 0;
     spu->nr3.divider.offset = 0;
     spu->nr3.t1 = 0;
     spu->nr3.index = 0;

     spu->nr3.divider.offset = 0;
     gb_spu_frequency_reload(&spu->nr3.divider);

     spu->nr3.duration.enable = false;
     spu->nr3.t1 = 0;
}

void gb_spu_duration_reload(struct gb_spu_duration *d,
                            unsigned duration_max,
                            uint8_t t1) {
     d->counter = (duration_max + 1 - t1) * 0x4000U;
}

/* Run the duration counter if it's enabled. Returns true if the counter reached
 * zero and the channel should be disabled. */
static bool gb_spu_duration_update(struct gb_spu_duration *d,
                                   unsigned duration_max,
                                   unsigned cycles) {
     bool elapsed = false;

     if (!d->enable) {
          return false;
     }

     while (cycles) {
          if (d->counter > cycles) {
               d->counter -= cycles;
               cycles = 0;
          } else {
               /* Counter reached 0 */
               elapsed = true;
               cycles -= d->counter;
               /* I'm not entirely sure about this but apparently when the
                * counter elapses it's reloaded with the max possible value
                * (maybe because it wraps around? */
               gb_spu_duration_reload(d, duration_max, 0);
          }
     }

     return elapsed;
}

/* Update the frequency counter and return the number of times it ran out */
static unsigned gb_spu_frequency_update(struct gb_spu_divider *f,
                                        unsigned cycles) {
     unsigned count = 0;

     while (cycles) {
          if (f->counter > cycles) {
               f->counter -= cycles;
               cycles = 0;
          } else {
               count++;
               cycles -= f->counter;
               /* Reload counter */
               gb_spu_frequency_reload(f);
          }
     }

     return count;
}

static uint8_t gb_spu_next_nr3_sample(struct gb *gb, unsigned cycles) {
     struct gb_spu *spu = &gb->spu;
     uint8_t sample;
     unsigned sound_cycles;

     /* The duration counter runs even if the sound itself is not running */
     if (gb_spu_duration_update(&spu->nr3.duration,
                                GB_SPU_NR3_T1_MAX,
                                cycles)) {
          spu->nr3.running = false;
     }

     if (!spu->nr3.running) {
          return 0;
     }

     sound_cycles = gb_spu_frequency_update(&spu->nr3.divider, cycles);

     spu->nr3.index = (spu->nr3.index + sound_cycles) % (GB_NR3_RAM_SIZE * 2);

     if (spu->nr3.volume_shift == 0) {
          /* Sound is muted */
          return 0;
     }

     /* We pack two samples per byte */
     sample = spu->nr3.ram[spu->nr3.index / 2];

     if (spu->nr3.index & 1) {
          sample &= 0xf;
     } else {
          sample >>= 4;
     }

     return sample >> (spu->nr3.volume_shift - 1);
}

/* Send a pair of left/right samples to the frontend */
static void gb_spu_send_sample_to_frontend(struct gb *gb,
                                           int16_t sample_l, int16_t sample_r) {
     struct gb_spu *spu = &gb->spu;
     struct gb_spu_sample_buffer *buf;

     buf = &spu->buffers[spu->buffer_index];

     if (spu->sample_index == 0) {
          /* We're about to fill the first sample, make sure that the
           * buffer is free. If it's not this will pause the thread until
           * the frontend frees it, effectively synchronizing us with audio
           */
          sem_wait(&buf->free);
     }

     buf->samples[spu->sample_index][0] = sample_l;
     buf->samples[spu->sample_index][1] = sample_r;

     spu->sample_index++;
     if (spu->sample_index == GB_SPU_SAMPLE_BUFFER_LENGTH) {
          /* We're done with this buffer */
          sem_post(&buf->ready);
          /* Move on to the next one */
          spu->buffer_index = (spu->buffer_index + 1)
               % GB_SPU_SAMPLE_BUFFER_COUNT;
          spu->sample_index = 0;
     }
}

void gb_spu_sync(struct gb *gb) {
     struct gb_spu *spu = &gb->spu;
     int32_t elapsed = gb_sync_resync(gb, GB_SYNC_SPU);
     int32_t frac;
     int32_t nsamples;
     int32_t next_sync;

     frac = spu->sample_period_frac;
     elapsed += frac;

     nsamples = elapsed / GB_SPU_SAMPLE_RATE_DIVISOR;

     while (nsamples--) {
          uint8_t sample =
               gb_spu_next_nr3_sample(gb, GB_SPU_SAMPLE_RATE_DIVISOR - frac);

          /* XXX TODO: mix properly */
          int16_t sample_l = sample << 11;
          int16_t sample_r = sample << 11;
          gb_spu_send_sample_to_frontend(gb, sample_l, sample_r);

          frac = 0;
     }

     /* See if we have any leftover fractional sample */
     frac = elapsed % GB_SPU_SAMPLE_RATE_DIVISOR;

     /* Advance the SPU state even if we don't want the sample yet in order to
      * have the correct value for the `running` flags */
     gb_spu_next_nr3_sample(gb, frac);

     spu->sample_period_frac = frac;

     /* Schedule a sync to fill the current buffer */
     next_sync = (GB_SPU_SAMPLE_BUFFER_LENGTH - spu->sample_index) *
          GB_SPU_SAMPLE_RATE_DIVISOR;
     next_sync -= frac;
     gb_sync_next(gb, GB_SYNC_SPU, next_sync);
}

void gb_spu_nr3_start(struct gb *gb) {
     struct gb_spu *spu = &gb->spu;

     if (!spu->nr3.enable) {
          /* We can't start if we're not enabled */
          return;
     }

     spu->nr3.index = 0;
     spu->nr3.running = true;
     gb_spu_frequency_reload(&spu->nr3.divider);
}
