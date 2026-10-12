/*

 This application reads audio data from an Knowles I2S microphone (SPH0645LM4H-B on AdaFruit breakout board).

 SPH0645LM4H-B Datasheet:
 https://cdn-shop.adafruit.com/product-files/3421/i2S+Datasheet.PDF

 DSP is done using the CMSIS-DSP library for ARM:
 https://arm-software.github.io/CMSIS_5/DSP/html/group__RealFFT.html

 Theory on calculating DB(a) using FFT can be found here (Sound Level by Meter Douglas R. Lanman):
 http://alumni.media.mit.edu/~dlanman/courses/decibel_meter.pdf

*/

#include <I2S.h>
#include "arm_math.h"
#include "arm_const_structs.h"
#include "SLMSettings.h"
#include "Stats.h"
#include "RunningAvg.h"
#include "EQ.h"

#if defined(ARDUINO_ARCH_RP2040)
// Arduino-Pico exposes I2S as an instance-based PIO peripheral rather than
// the global SAMD I2S object used by the original Feather M0 sketch.
static I2S slmI2S(INPUT);
  #define SLM_I2S slmI2S
#else
  #define SLM_I2S I2S
#endif

#define SLM_I2S_READ_WORDS 256

// Audio buffer indexes and counters
static uint32_t buffer_write_index = 0;
static int32_t buffer[BUFFER_SIZE];
static volatile uint32_t samples_read = 0;
static uint32_t frames_done = 0;
static uint8_t i2s_channel_index = 0;

/// Buffer for reading from I2S
static int32_t chunk[SLM_I2S_READ_WORDS];

// Buffers for FFT are in q31 format. Fixed point values range from [-1,1] mapped to [-2^31,2^32].
// The DSP library uses fixed point (q31) as input. But the output of the FFT can be in different format (values bigger than 1).
// For the FFT the output depends on the FFT size (bigger FFT, bigger values, see CMSIS-DSP documention).
// N.B. FFT size is defined as 2 x number of bins.
// For example (documentation) the output size for q31 FFT with size 2048 (1024 real input samples) is q11.21.
// That means we have to divide output values by 2^21.
static q31_t hann[DATA_SIZE];
static arm_rfft_instance_q31 slmFft;

// Test tone generation, in order to test the dBA algorithm test tones can be generated instead of using audio input from the microphone
uint32_t test_tone_index = 0;
uint32_t test_tone_step = INT32_MAX / (FS / TEST_TONE_FREQ);

//
static double normal_hann_scale_factor = (1.63 * sqrt(2.0))/((double)DATA_SIZE);                                                                                                        //normalize and double because we use half of the bins, multiply to correct hann window
static uint8_t fracbits = (FRACBITS + FRACBITS + SCALING_TABLE_FRACBITS);



class SLM
{
  Stream& serial;
  bool paused = true;
  bool capture_started = false;
  int bands;
  RunningAvg avg = RunningAvg();
public:
  Stats statsRMS = Stats();
  Stats statsDB = Stats();
#if CALC_BANDS == 1
  Stats band_stats[12] = { Stats(), Stats(), Stats(), Stats(), Stats(), Stats(), Stats(), Stats(), Stats(), Stats(), Stats(), Stats() }; //1 per band, 12 bands (octave) is probably enough (depends on sample rate and start bin)
#endif


  // Copy microphone samples from the I2S DMA buffer to the circular audio
  // buffer. The Arduino-Pico callback runs in interrupt context, where its
  // documentation prohibits reading I2S, so RP2040 builds call this from the
  // main loop instead. The legacy SAMD core still reads from its callback.
  static void readI2S()
  {
    size_t bytes = SLM_I2S.available();
    if (bytes > sizeof(chunk))
    {
      bytes = sizeof(chunk);
    }
    // Arduino-Pico I2S reads whole 32-bit words. A read may end between the
    // left and right channel words, so preserve channel position across calls.
    bytes -= bytes % 4;
    if (bytes == 0)
    {
      return;
    }

    // Cast to bytes to select the bulk-buffer overload. Without this cast,
    // the compiler selects read(int32_t *, bool), which returns only 0/1.
    size_t bytes_read = SLM_I2S.read((uint8_t *)chunk, bytes);
    bytes_read -= bytes_read % 4;
    // Copy the selected channel from I2S or fill it with the sine test tone.
    int32_t val;
    for (unsigned int i = 0; i < bytes_read / 4; i++)
    {
      if (i2s_channel_index == 1) //right channel; SEL should be tied to 3V
      {
        val = chunk[i] & 0xFFFFC000;  // mask with 14 zero's
#if (TEST == 1)
        buffer[buffer_write_index] = arm_sin_q31(test_tone_index); // expects [0,1>
        test_tone_index += test_tone_step;
        if (test_tone_index > INT32_MAX) // INT32_MAX is Q31_MAX
        {
          test_tone_index -= INT32_MAX;
        }
#else
        buffer[buffer_write_index] = val;
#endif
        buffer_write_index++;
        samples_read++;
        if (buffer_write_index == BUFFER_SIZE)
        {
          buffer_write_index = 0;
        }
      }
      i2s_channel_index ^= 1;
    }
  }

#if !defined(ARDUINO_ARCH_RP2040)
  static void onI2SReceive()
  {
    readI2S();
  }
#endif


  SLM(Stream &s) : serial(s)
  {
  }

  void start()
  {
    // End any previous capture before clearing state so a new measurement
    // window cannot retain samples, channel position or averages from it.
    if (capture_started)
    {
      stop();
    }
    buffer_write_index = 0;
    samples_read = 0;
    frames_done = 0;
    i2s_channel_index = 0;
    test_tone_index = 0;
    memset(buffer, 0, sizeof(buffer));
    memset(chunk, 0, sizeof(chunk));
    reset();

    // Start I2S at 48 kHz with 32-bits per block (containes 24-bit sample), allowed rates for ics43432 8khz, 16khz, 24khz, 48khz
#if defined(ARDUINO_ARCH_RP2040)
    SLM_I2S.setBCLK(SLM_I2S_BCLK_PIN);
    SLM_I2S.setDATA(SLM_I2S_DATA_PIN);
    SLM_I2S.setBitsPerSample(32);
    SLM_I2S.setBuffers(SLM_I2S_DMA_BUFFERS, SLM_I2S_DMA_WORDS);
    bool i2s_started = SLM_I2S.begin(FS);
#else
    bool i2s_started = SLM_I2S.begin(I2S_PHILIPS_MODE, FS, 32);
#endif
    if (!i2s_started)
    {
      if (SLM_MODE == SLM_MODE_DEBUG)
      {
        serial.println(F("Failed to initialize I2S!"));
      }

      while (1)       // do nothing
      {
      }
    }
    capture_started = true;
    paused = false;
    // Number of bands is highest band index + 1
    bands = (31 - __builtin_clz((DATA_SIZE / 2) / START_BIN)) + 1;
  }

  // End audio stream from mic
  void stop()
  {
    paused = true;
    if (capture_started)
    {
      SLM_I2S.end();
      capture_started = false;
    }
  }

  // Bypass processing while capture remains active.
  void pause()
  {
    paused = true;
  }

  // Reset statistics 
  void reset()
  {
    statsDB.Reset();
    statsRMS.Reset();
    avg.Reset();
#if CALC_BANDS == 1
    for (unsigned int i = 0; i < sizeof(band_stats) / sizeof(band_stats[0]); i++)
    {
      band_stats[i].Reset();
    }
#endif
  }

  // Resume with a fresh audio window, including a new I2S DMA capture.
  void resume()
  {
    start();
  }

  // One time stuff
  bool setup()
  {
    // Configure the fixed transform once, rather than 32 times per second.
    // The dedicated initializer also exposes the required size to the linker.
#if DATA_SIZE == 1024
    if (arm_rfft_init_1024_q31(&slmFft, 0, 1) != ARM_MATH_SUCCESS) return false;
#else
    if (arm_rfft_init_q31(&slmFft, DATA_SIZE, 0, 1) != ARM_MATH_SUCCESS) return false;
#endif
    // Make a q31 hann window for scaling FFT real input buffer
    double hann_s = 1.0 / DATA_SIZE;
    double hann_i = 0;
    for (unsigned int i = 0; i < DATA_SIZE; i++)
    {
      double v = sin(hann_i * PI);
      hann[i] = v * v * 2147483647.0;       //to q31
      hann_i += hann_s;
    }
#if !defined(ARDUINO_ARCH_RP2040)
    // The SAMD I2S implementation calls this as audio data arrives.
    SLM_I2S.onReceive(SLM::onI2SReceive);
#endif
    return true;
  }

   
  // Stream audio to serial port. Make sure to enable and write nothing else then this audio.
  // Listen: cat /dev/cu.usbmodem14111 | mplayer -rawaudio rate=48000:channels=1:samplesize=4 -demuxer rawaudio -cache 1024 -
  // Or write to raw file: cat /dev/cu.usbmodem14111 > audio.raw
  void streamAudio()
  {
    uint32_t frames_ought_to_be_done = samples_read / DATA_SIZE;
    if ((SLM_MODE == SLM_MODE_STREAM_AUDIO) && (frames_done < frames_ought_to_be_done))
    {
      static q31_t windowed[DATA_SIZE];
      // Copy latest window of samples possibly wrapped over circular buffer
      uint32_t start = (BUFFER_SIZE + buffer_write_index - DATA_SIZE) % BUFFER_SIZE;
      if (start <= (BUFFER_SIZE - DATA_SIZE))
      {
        // Without wrapping
        memcpy((uint8_t *)windowed, (uint8_t *)(buffer + start), DATA_SIZE * sizeof(int32_t));
      }
      else
      {
        // With wrapping in two parts
        memcpy((uint8_t *)windowed, (uint8_t *)(buffer + start), (BUFFER_SIZE - start) * sizeof(int32_t));
        memcpy((uint8_t *)(windowed + (BUFFER_SIZE - start)), (uint8_t *)buffer, (DATA_SIZE - (BUFFER_SIZE - start)) * sizeof(int32_t));
      }
      serial.write((uint8_t *)windowed, DATA_SIZE * sizeof(int32_t));
      frames_done++;
    }
  }

  // Main function call as often as possible. Sticks to the frame rate configured
  void update()
  {
    if (paused)
    {
      return;
    }

#if defined(ARDUINO_ARCH_RP2040)
    // Arduino-Pico's onReceive callback is interrupt-driven and must not read
    // the peripheral. Drain its DMA data here in foreground context.
    readI2S();
#endif

    //
    if (SLM_MODE == SLM_MODE_STREAM_AUDIO)
    {
      streamAudio();
      return;
    }

    //
    if (SLM_MODE == SLM_MODE_OUTPUT)
    {
      // Reset stats on user input
      if (serial.available() > 0)
      {
        byte b = serial.read();
        if (b == 0x00)
        {
          statsDB.Reset();
          statsRMS.Reset();
        }
      }
    }

    // All modes except for SLM_MODE_STREAM_AUDIO
    uint32_t frames_ought_to_be_done = samples_read / (FS / FPS);
    if (frames_done < frames_ought_to_be_done)
    {
      // Input and output buffer
      // Keep the 12 KiB DSP workspace out of the RP2040's 8 KiB stack.
      static q31_t windowed[DATA_SIZE];
      static q31_t out_q[DATA_SIZE * 2];

      // Measure, all calcs take about 27ms with Q nums, 50ms with floats
      int m = millis();

      // Skip the first buffers, seem to contain noise
      if (frames_ought_to_be_done < 10)
      {
        frames_done++;
        return;
      }

      // Multipy latest window of samples possibly wrapped over buffer
      uint32_t start = (BUFFER_SIZE + buffer_write_index - DATA_SIZE) % BUFFER_SIZE;

      // Apply Hann window to newest samples in buffer, correct later with scaling. 
      // Take samples from circular buffer with or without wrap depending on current write index. 
      // Following operation must be done within (BUFFER_HEAD / FS) seconds, it should take about max 2ms
      if (start <= (BUFFER_SIZE - DATA_SIZE))
      {
        // Without wrapping
        arm_mult_q31(buffer + start, hann, windowed, DATA_SIZE);
      }
      else
      {
        // With wrapping in two parts
        arm_mult_q31(buffer + start, hann, windowed, BUFFER_SIZE - start);
        arm_mult_q31(buffer, hann + (BUFFER_SIZE - start), windowed + (BUFFER_SIZE - start), DATA_SIZE - (BUFFER_SIZE - start));
      }

      // Transform with the instance initialized once in setup().
      arm_rfft_q31(&slmFft, windowed, out_q);

      // Stream FFT data out in binary format
      if (SLM_MODE == SLM_MODE_STREAM_FFT)
      {
        serial.write((uint8_t *)out_q, DATA_SIZE * 2 * sizeof(int32_t));
        frames_done++;
        return;
      }

      // When we remove lower bins, things get much better... this seems to have to do with DC offset.
      // Seems to be a lot of noise below 50hz while the mic cannot even measure that, as such we ignore te first bins.
      // At 1024 real input size (FFT size 2048) first bins 0 & 1 are respectively 0Hz and 43Hz.
      // Floating point version: normalizing and scaling the q (fixed point) number is done after the loop to save floating point calulcations.
#if USE_FLOATS == 1
      double total = 0;       // RMS total
      for (unsigned int i = START_BIN; i < (DATA_SIZE / 2); i++)
      {
        q31_t re = out_q[i * 2];
        q31_t im = out_q[i * 2 + 1];
        double re_f = ((double)re);
        double im_f = ((double)im);
        // Because magnitude is squared here we need to multiply with A-weighting amplitude scaling squared (which is power scaling)
        float scale = CORRECTED_DBS_FLOAT[i];
        double mag_squared = (re_f * re_f + im_f * im_f) * scale;
        total += mag_squared;
      }

      double RMSa = 1.63 * q2double(sqrt(2.0 * total / (DATA_SIZE * DATA_SIZE)), FRACBITS); //this is amplitude RNS
      double dbA = 120 + db_full_scale_amp(RMSa);
#else
      // Fixed point version, take in account that numbers keep fitting in the fixed point numbers. Adapted the qmath lib to give 64 bit room!
      // When normalizing in the loop a lot of resolution (data) is lost, so we want to postpone scaling back as long as possible.
      // The fp version was found to be 5 times quicker, however with really high levels (0db) we might saturate the fp number.
      // For example the RMS total (before normalizing) with an unsigned q14.50, the maximum value is 2^14. As such the maximum level is only -7.8dB with 1024 real input size.
      // Ruby code : 20 * Math::log10(1.63 * Math::sqrt(2.0 * (2**14) / (1024 ** 2)) * Math::sqrt(2))
      // The testtone does not reach 120 for this reason!
      // To partly solve this problem we calculate a total per band and add all the bands as floating point after the heavy calculation.
      // When using a test tone of 0db fixed point numbers can still saturate because all energy is in one band.
      // Comments for code assumes using 2048 FFT size
      uint64_t band_totals[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };       //totals per band, amount of bands varies with FFT_SIZE and START_BIN, however 16 is more than enough
      int64_t re;
      int64_t im;
      uint64_t q_squared;
      uint64_t q_squared_scaled;
      uint8_t band_index;
      unsigned int o;
      for (unsigned int i = START_BIN; i < (DATA_SIZE / 2); i++)
      {
        o = i * 2;
        re = out_q[o];                                                                  //get the q number, fractional bit depends on FFT SIZE, q11.21 for FFT size 2048
        im = out_q[o + 1];                                                              //get the q number, fractional bit depends on FFT SIZE, q11.21 for FFT size 2048
        q_squared = re * re + im * im;                                                  //output q22.42, from here we know the value is positive we can save one bit for more resolution
        // Because magnitude is squared here we need to multiply with A-weighting amplitude scaling squared (which is power scaling)
        q_squared_scaled = q_squared * (uint64_t)CORRECTED_DBS_FIXED_POINT[i];          //output q14.50
 #if CALC_BANDS == 0       
        band_totals[0] += q_squared_scaled;
 #else
        band_index = (31 - __builtin_clz(i / START_BIN));                               //from start bin, band increases with 1 on every doubling (octave)
        band_totals[band_index] += q_squared_scaled;
 #endif
      }

      // After heavy calculation is done we go back to floating point numbers
      double bands_total = 0;
      double band_total;
      double RMSa_band;
      double RMSp_band;
      double dbA_band;

#if CALC_BANDS == 0
      bands_total = q2double(band_totals[0], fracbits);
#else
      // should we do this without A-weighting?
      for (unsigned int i = 0; i < bands; i++) //
      {
        band_total = q2double(band_totals[i], fracbits);
        bands_total += band_total;
        RMSa_band = normal_hann_scale_factor * sqrt(band_total);
        RMSp_band = RMSa_band * RMSa_band;
        band_stats[i].UpdateMean(RMSp_band);
      }
#endif 
      // Averaging RMSs instead of DBs !
      double RMSa = normal_hann_scale_factor * sqrt(bands_total); //overall energy
      double RMSp = RMSa * RMSa; //RMS amp to power
      double dbA = 120 + db_full_scale_pow(RMSp); //one frame, very short period
#endif //USE_FLOATS == 0
      double avgShort = avg.Update(RMSp); //short period running avg 
      double dbLEQ = 120 + db_full_scale_pow(avgShort); //short period db
      statsRMS.Update(RMSp); //long period LEQ
      statsDB.Update(dbLEQ); //stats on short period LEQ

      frames_done++;

      if (SLM_MODE == SLM_MODE_OUTPUT)
      {
        serial.write((uint8_t *)&dbLEQ, sizeof(double));
        double dbMin = statsDB.Min();
        double dbMax = statsDB.Max();
        double dbMean = 120 + db_full_scale_pow(statsRMS.Mean());
        serial.write((uint8_t *)&dbMin, sizeof(double));
        serial.write((uint8_t *)&dbMax, sizeof(double));
        serial.write((uint8_t *)&dbMean, sizeof(double));
      }

      if (SLM_MODE == SLM_MODE_DEBUG)
      {
        serial.print(dbA); //single frame
        serial.print(F(","));
        serial.print(dbLEQ); //running avg
        serial.print(F(","));
        serial.print(statsDB.Min()); 
        serial.print(F(","));
        serial.print(statsDB.Max());
        serial.print(F(","));
        serial.print(120 + db_full_scale_pow(statsRMS.Mean()));
        serial.print(F(","));
        serial.println(statsDB.StdDev(true));
#if CALC_BANDS == 1
        for (unsigned int i = 0; i < bands; i++) //all band dbs add together to 'overall' mean (checked)
        {
          serial.print(i);
          serial.print(F(","));
          serial.println(120 + db_full_scale_pow(band_stats[i].Mean()));
        }
#endif
      }

      if (SLM_MODE == SLM_MODE_PROFILE)
      {
        serial.printf("%ms %i\r\n", millis() - m);
        serial.printf("missed frames: %i\n", frames_ought_to_be_done - frames_done);
      }
    }
  }
};
