#ifndef SoftSerial_h
#define SoftSerial_h
#if defined(ARDUINO_ARCH_STM32F1)
#define BMAP
#elif defined(ARDUINO_ARCH_STM32)
#define BHAL
#else
#error Core not supported
#endif

/******************************************************************************
* SoftSerial.cpp
* Multi-instance Software Serial Library for STM32Duino
* Using Timers and Interrupts enabling use of any GPIO pins for RX/TX
* 
* Copyright 2015 Ron Curry, InSyte Technologies
* 
 
Features:
- Fully interrupt driven - no delay routines.
- Any GPIO pin may be used for tx or rx (hence the AP libray name suffix)
- Circular buffers on both send and receive.
- Works up to 115,200 baud.
- Member functions compatible with Hardware Serial and Arduino NewSoftSerial Libs
- Extensions for non-blocking read and transmit control
- Supports up to 4 ports (one timer used per port) without modification.
- Easily modified for more ports or different timers on chips with more timers.
- Can do full duplex under certain circumstances at lower baud rates.
- Can send/receive simultaneously with other ports/instantiatious at some baud
  rates and certain circumstances.

Notes:

Performance
- More than two ports use has not been extensively tested. High ISR latencies in the 
low-level Maple timer ISR code, C++ ISR wranglings, and the STM32 sharing interrupt 
architecture (not in that order) restrict simultaneous port use and speeds. Two ports
sending simultaniously at 115,200. As well, two ports simultansiously receiving at 57,600
simultaneously have been successfully tested. Various other situations have been 
tested. Results are dependent on various factors - you'll need to experiment.

Reliability
- Because of the way STM32 shares interrupts and the way STM32Arduino low level ISRs
processes them latencies can be very high for interrupts on a given timer. I won't go
into all the details of why but some interrupts can be essentially locked out. Causing
extremely delayed interrupt servicing. Some of this could be alleviated with more
sophisticated low-level ISRs that make sure that all shared interrupt sources get
serviced on an equal basis but that's not been done yet. 
This impacts the ability to do full duplex on a single port even at medium bit rates.
I've done some experimentation with two ports/instantiations with RX on one port
and TX on the other with good results for full-duplex. In any case, to be sure
that the serial data streams are not corrupted at higher data rates it's best to
write the application software such that rx and tx are not sending/receiving
simultaneously in each port and that other ports are not operating simultaneously
as well. This is, effectively, how the existing NewSoftSerial library on Arduino
operates so not a new concept. Again, experiment and find what works in your
application.

Improvements
No doubt the code can be improved upon. If you use the code PLEASE give back by
providing soure code for improvements or modifications you've made!
- Specific improvements that come to mind and I'd like to explore are:
  o Replacing the STM32/Maple timer interrupt handlers with something more streamlined
    and lower latency and overhead.
  o A better way to implement the high level C++ ISR's to reduce latency/overhead
  o Minor improvements that can save cycles in the C++ ISR's such as using bit-banding
  o Possibly a way to coordinate RX/TX to increase full-duplex capability.

License
* Permission is hereby granted, free of charge, to any person
* obtaining a copy of this software and associated documentation
* files (the "Software"), to deal in the Software without
* restriction, including without limitation the rights to use, copy,
* modify, merge, publish, distribute, sublicense, and/or sell copies
* of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
* BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
* ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
* CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
 *****************************************************************************/

#include <Arduino.h>

#ifdef BMAP
#include <libmaple/libmaple.h>
#include <ext_interrupts.h>
#endif

#include <HardwareTimer.h>

/******************************************************************************
* Definitions
******************************************************************************/
#define DEBUG_DELAY 0
#define DEBUG_PIN 17
#define DEBUG_PIN1 18
#ifdef BHAL
typedef void (*voidFuncPtr) (void);
typedef void (*htFuncPtr) (HardwareTimer *);
#else
#define htFuncPtr voidFuncPtr
#endif

#define  SSI_RX_BUFF_SIZE 128
#define  SSI_TX_BUFF_SIZE 128
#define SS_MAX_RX_BUFF (SSI_RX_BUFF_SIZE - 1)	// RX buffer size
#define SS_MAX_TX_BUFF (SSI_TX_BUFF_SIZE - 1)	// TX buffer size
#define _SSI_VERSION   1.2	// Library Version

/******************************************************************************
* Class Definition
******************************************************************************/
class SoftSerial:public Stream
{
private:

  // Per object data
  uint8_t transmitPin;
#ifdef BMAP
  gpio_dev *txport;
  exti_num gpioBit;
  HardwareTimer timerSerial;
  timer_dev *timerSerialDEV;
#else
  GPIO_TypeDef *txport;
  uint8_t gpioBit;
  HardwareTimer *timerSerial;
  TIM_TypeDef *timerSerialDEV;
#endif
  uint8_t txbit;
  uint8_t receivePin;
  uint8_t rxtxTimer;
  bool activeRX;
  bool activeTX;
  uint8_t lS = LOW;
  uint8_t _rx_channel, _tx_channel;
  uint8_t TX_TIMER_CHANNEL, TX_TIMER_MASK, TX_TIMER_PENDING;
  uint8_t RX_TIMER_CHANNEL, RX_TIMER_MASK, RX_TIMER_PENDING;
#ifdef BHAL
  const uint16_t CHAN[4] =
    { TIM_CHANNEL_1, TIM_CHANNEL_2, TIM_CHANNEL_3, TIM_CHANNEL_4 };
  uint32_t exprio = ~(0UL), curprio = ~(0UL);
#endif

#if DEBUG_DELAY
  volatile uint8_t overFlowTail;
  volatile uint8_t overFlowHead;
#endif

  uint16_t bitPeriod;
  uint16_t startBitPeriod;
  volatile uint16_t rxTimingCount;
  volatile uint16_t txTimingCount;
  volatile uint8_t bufferOverflow;

  volatile int8_t rxBitCount;
  volatile uint16_t receiveBufferWrite;
  volatile uint16_t receiveBufferRead;
  volatile uint8_t receiveBuffer[SS_MAX_RX_BUFF];

  volatile uint16_t transmitBufferWrite = 0;
  volatile uint16_t transmitBufferRead = 0;
  volatile uint8_t transmitBuffer[SS_MAX_TX_BUFF];

  // Static Data
  static SoftSerial *interruptObject1;	// This looks inefficient
  // but it reduces
  static SoftSerial *interruptObject2;	// interrupt latency a
  // small amount
  static SoftSerial *interruptObject3;
  static SoftSerial *interruptObject4;

#define STT static

  STT voidFuncPtr handleRXEdgeInterruptP[4];
  STT htFuncPtr handleRXBitInterruptP[4];
  STT htFuncPtr handleTXBitInterruptP[4];


  // Static Methods
  // Better way to do this?
#ifdef BHAL
#define HTIM HardwareTimer *
#else
#define HTIM void
#endif

  STT inline void handleRXBitInterrupt1 (HTIM)
    __attribute__ ((__always_inline__));
  STT inline void handleTXBitInterrupt1 (HTIM)
    __attribute__ ((__always_inline__));

  STT inline void handleRXBitInterrupt2 (HTIM)
    __attribute__ ((__always_inline__));
  STT inline void handleTXBitInterrupt2 (HTIM)
    __attribute__ ((__always_inline__));

  STT inline void handleRXBitInterrupt3 (HTIM)
    __attribute__ ((__always_inline__));
  STT inline void handleTXBitInterrupt3 (HTIM)
    __attribute__ ((__always_inline__));

  STT inline void handleRXBitInterrupt4 (HTIM)
    __attribute__ ((__always_inline__));
  STT inline void handleTXBitInterrupt4 (HTIM)
    __attribute__ ((__always_inline__));

#undef HTIM

  STT inline void handleRXEdgeInterrupt1 ()
    __attribute__ ((__always_inline__));
  STT inline void handleRXEdgeInterrupt2 ()
    __attribute__ ((__always_inline__));
  STT inline void handleRXEdgeInterrupt3 ()
    __attribute__ ((__always_inline__));
  STT inline void handleRXEdgeInterrupt4 ()
    __attribute__ ((__always_inline__));

  // Private Methods
  inline void noTXInterrupts () __attribute__ ((__always_inline__));
  inline void txInterrupts () __attribute__ ((__always_inline__));
  inline void txInterruptsClr () __attribute__ ((__always_inline__));
  inline void noRXStartInterrupts () __attribute__ ((__always_inline__));
  inline void rxStartInterrupts () __attribute__ ((__always_inline__));
  inline void noRXInterrupts () __attribute__ ((__always_inline__));
  inline void rxInterrupts () __attribute__ ((__always_inline__));
  inline void rxInterruptsClr () __attribute__ ((__always_inline__));
  inline void onRXPinChange (void) __attribute__ ((__always_inline__));
  inline void rxNextBit (void) __attribute__ ((__always_inline__));
  inline void txNextBit (void) __attribute__ ((__always_inline__));
  inline uint16_t isTXInterruptEnabled () __attribute__ ((__always_inline__));
  inline void init_timer_values (uint8_t TX_CHANNEL, uint8_t RX_CHANNEL);

  void setInterruptObject (uint8_t timerNumber);

public:
  // Public Methods
  SoftSerial (int receivePinT, int transmitPinT, uint8_t rxtxTimerT	/* , 
									 * bool 
									 * inverseLogic 
									 */ );

  SoftSerial (int receivePinT, int transmitPinT, uint8_t rxtxTimerT, uint8_t tx_channel, uint8_t rx_channel	/* , 
														 * bool 
														 * inverseLogic 
														 */ );

  ~SoftSerial ();

  uint32_t rxedgec, rxbitc, txbitc;
  uint32_t tt = millis ();
  String s_dbg = "";
  void dbg (String p)
  {
    s_dbg += "[" + String (millis ()) + "] " + p + "\n";
  }
  void p_dbg (HardwareSerial * S)
  {
    S->print (s_dbg);
    s_dbg = "";
  }
  static int library_version ()
  {
    return _SSI_VERSION;
  }
  void print_counters (HardwareSerial * S);
  void begin (uint32_t tBaud);
  bool listen ();
  bool isListening ()
  {
    return activeRX;
  }
  bool stopListening ();
  bool talk ();
  bool isTalkingT ()
  {
    return activeTX;
  }
  bool stopTalking ();
  void end ();
  bool overflow ()
  {
    bool ret = bufferOverflow;
    if (ret)
      bufferOverflow = false;
    return ret;
  }
  int readnb ();		// Non-blocking read
  volatile int8_t txBitCount;

  virtual int peek ();
  virtual size_t write (uint8_t byte);
  virtual int read ();
  virtual int available ();
  virtual void flush ();
  operator            bool ()
  {
    return true;
  }

  // Debug use only
#if DEBUG_DELAY
  uint16_t getBitPeriod ()
  {
    return bitPeriod;
  }
  void setBitPeriod (uint16_t period)
  {
    bitPeriod = period;
  }
  uint16_t getBitCentering ()
  {
    return startBitPeriod;
  }
  void setBitCentering (uint16_t period)
  {
    startBitPeriod = period;
  }
  uint8_t getOverFlowTail ()
  {
    return overFlowTail;
  }
  uint8_t getOverFlowHead ()
  {
    return overFlowHead;
  }
  uint8_t getTXHead ()
  {
    return transmitBufferRead;
  }
  uint8_t getTXTail ()
  {
    return transmitBufferWrite;
  }
  uint8_t getRXHead ()
  {
    return receiveBufferRead;
  }
  uint8_t getRXTail ()
  {
    return receiveBufferWrite;
  }
  uint8_t getrxtxTimer ()
  {
    return rxtxTimer;
  }
#endif

  using Print::write;

};

#endif
