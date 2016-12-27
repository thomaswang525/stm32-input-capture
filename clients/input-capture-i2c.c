#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "i2c.h"
#include "timespec.h"

#define I2C_ADDR 0x4
#define EXPECTED_FREQ 48000000
#define AVERAGING_CYCLES 1025
#define PPM_INVALID -1000000.0

struct i2c_registers_type {
  uint32_t milliseconds_now;
  uint32_t milliseconds_irq_ch1;
  uint16_t tim3_at_irq[3];
  uint16_t tim1_at_irq[3];
  uint16_t tim3_at_cap[3];
  uint16_t source_HZ_ch1;
};

void print_ppm(float ppm) {
  if(ppm < 500 && ppm > -500) {
    printf("%1.3f ", ppm);
  } else {
    printf("- ");
  }
}

void write_tcxo_ppm(float ppm) {
  FILE *tcxo;

  // TODO: allow the average ppm to be detected or configured
  ppm = 23.777 - ppm;

  ppm = ppm * -0.5; // reduce tempcomp effect by 50%

  if(ppm > 10 || ppm < -10) {
    printf("- ");
    return;
  }

  tcxo = fopen("/run/.tcxo","w");
  if(tcxo == NULL) {
    perror("fopen /run/.tcxo");
    exit(1);
  }
  fprintf(tcxo, "%1.3f\n", ppm);
  fclose(tcxo);
  rename("/run/.tcxo","/run/tcxo");
  printf("%1.3f ", ppm);
}

void add_offset_cycles(double added_offset_ns, struct timespec *cycles, uint16_t *first_cycle, uint16_t *last_cycle) {
  struct timespec *previous_cycle = NULL;
  uint16_t this_cycle_i;

  this_cycle_i = (*last_cycle + 1) % AVERAGING_CYCLES;
  if(*first_cycle != *last_cycle) {
    previous_cycle = &cycles[*last_cycle];
    if(*first_cycle == this_cycle_i) { // we've wrapped around, move the first pointer forward
      *first_cycle = (*first_cycle + 1) % AVERAGING_CYCLES;
    }
  }

  cycles[this_cycle_i].tv_sec = 0;
  cycles[this_cycle_i].tv_nsec = added_offset_ns;
  if(previous_cycle != NULL) {
    add_timespecs(&cycles[this_cycle_i], previous_cycle);
  }

  *last_cycle = this_cycle_i;
}

float calc_ppm(struct timespec *end, struct timespec *start, uint16_t seconds) {
  double diff_s;
  struct timespec diff;
  diff.tv_sec = end->tv_sec;
  diff.tv_nsec = end->tv_nsec;

  sub_timespecs(&diff, start);
  diff_s = diff.tv_sec + diff.tv_nsec / 1000000000.0;

  float ppm = diff_s * 1000000.0 / seconds;
  return ppm;
}

uint8_t cycles_wrap(uint32_t *this_cycles, uint32_t previous_cycles, int32_t *diff, const struct i2c_registers_type *i2c_registers) {
  uint8_t wrap = 0;
  *diff = *this_cycles - previous_cycles - EXPECTED_FREQ;

  if(i2c_registers->tim3_at_cap[0] > i2c_registers->tim3_at_irq[0]) {
    if(*diff > 41535) { // allow for +/-500ppm
      wrap = 1;
      *this_cycles -= 65536;
      *diff -= 65536;
    }
  } else if(i2c_registers->tim3_at_cap[0] > 65300) { // check for wrap if it's close
    if(*diff > 41535) { // allow for +/-500ppm
      wrap = 3;
      *this_cycles -= 65536;
      *diff -= 65536;
    }
  }

  return wrap;
}

uint16_t wrap_add(int16_t a, int16_t b, uint16_t modulus) {
  a = a + b;
  if(a < 0) {
    a += modulus;
  }
  return a;
}

uint16_t wrap_sub(int16_t a, int16_t b, uint16_t modulus) {
  return wrap_add(a, -1 * b, modulus);
}

float show_ppm(int16_t number_points, uint16_t last_cycle_index, uint16_t seconds, struct timespec *cycles) {
  float ppm = PPM_INVALID; // default: invalid PPM

  if(number_points >= seconds) {
    uint16_t start_index = wrap_sub(last_cycle_index, seconds, AVERAGING_CYCLES);
    ppm = calc_ppm(&cycles[last_cycle_index], &cycles[start_index], seconds);
    print_ppm(ppm);
  } else {
    printf("- ");
  }

  return ppm;
}

uint32_t calculate_sleep_ms(uint32_t milliseconds_now, uint32_t milliseconds_irq) {
  uint32_t sleep_ms = 1005 - (milliseconds_now - milliseconds_irq);
  if(sleep_ms > 1005) {
    sleep_ms = 1005;
  } else if(sleep_ms < 1) {
    sleep_ms = 1;
  }
  return sleep_ms;
}

int main() {
  int fd;
  struct timespec cycles[AVERAGING_CYCLES];
  uint16_t first_cycle_index = 0, last_cycle_index = 0;
  uint32_t last_timestamp = 0;
  uint32_t previous_cycles = 0;
  struct i2c_registers_type i2c_registers;

  memset(cycles, '\0', sizeof(cycles));
 
  fd = open_i2c(I2C_ADDR); 

  printf("ts delay wrap cycles #pts t.offset 16s_ppm 512s_ppm 1024s_ppm tempcomp\n");
  while(1) {
    double added_offset_ns;
    uint32_t sleep_ms, this_cycles;
    uint8_t wrap = 0;
    int16_t number_points;

    read_i2c(fd, &i2c_registers, sizeof(i2c_registers));

    // was there no new data?
    if(i2c_registers.milliseconds_irq_ch1 == last_timestamp) {
      printf("no new data\n");
      first_cycle_index = last_cycle_index = 0; // reset because we missed a cycle
      usleep(995000);
      continue;
    }
    last_timestamp = i2c_registers.milliseconds_irq_ch1;
    sleep_ms = calculate_sleep_ms(i2c_registers.milliseconds_now, i2c_registers.milliseconds_irq_ch1);

    // combine tim1 & tim3
    this_cycles = ((uint32_t)i2c_registers.tim1_at_irq[0]) << 16;
    this_cycles += i2c_registers.tim3_at_cap[0];

    // check for tim3 wrap if we have a previous count
    if((last_cycle_index != first_cycle_index) || (previous_cycles > 0)) {
      int32_t diff;
      wrap = cycles_wrap(&this_cycles, previous_cycles, &diff, &i2c_registers);
      added_offset_ns = diff * 1000000000.0 / EXPECTED_FREQ;
      add_offset_cycles(added_offset_ns, cycles, &first_cycle_index, &last_cycle_index);
    } else {
      // TODO: dealing with wraps on the very first sample
      previous_cycles = this_cycles;
      usleep(sleep_ms * 1000);
      continue;
    }
    previous_cycles = this_cycles;
    number_points = wrap_sub(last_cycle_index, first_cycle_index, AVERAGING_CYCLES);

    // aim for 5ms after the event
    printf("%lu %u %u %u %u ", time(NULL),
       i2c_registers.milliseconds_now - i2c_registers.milliseconds_irq_ch1,
       wrap, this_cycles, number_points
       );
    print_timespec(&cycles[last_cycle_index]);
    printf(" ");

    show_ppm(number_points, last_cycle_index, 16, cycles);
    show_ppm(number_points, last_cycle_index, 512, cycles);
    float ppm = show_ppm(number_points, last_cycle_index, AVERAGING_CYCLES-1, cycles);
    write_tcxo_ppm(ppm);

    printf("\n");
    fflush(stdout);

    usleep(sleep_ms * 1000);
  }
}