#include <msp430g2211.h>

#define AUDIO BIT1
#define RED BIT0
#define GRN BIT6

#define DOT BIT4
#define DASH BIT7

#define FASTERBUTTON BIT3
 
unsigned int flagwpm = 1;

unsigned int wpm = 5;/*15*/
unsigned int dotCount = 0, guardSpace = 0;

volatile unsigned int timer_count;

volatile int counter = 0;
volatile unsigned int countAdjust=0;
volatile int toneOn = 0;

volatile int dotKey = 0, dashKey = 0;

/* Key capacitance baseline values. */
unsigned int base_dash, base_dot;

/* State of the keyer.
 * idle: no keys pressed recently
 * dot: sent a dot
 * dash: sent a dash
 */
enum STATES { idle, dot, dash } state = idle;





/* This triggers when a pad has been charged or discharged.  When it returns,
 * timer_count will hold the elapsed count of charging or discharging time for
 * the key.  Setup for triggering this interrupt happens in
 * measure_key_capacitance(). */
void Port_1 (void) __attribute__((interrupt(PORT1_VECTOR)));
void Port_1 (void) {
    P1IFG = 0;
    timer_count = TAR - timer_count;
    __bic_SR_register_on_exit( LPM0_bits );
}




/* Returns a value the reflects the capacitance of one of two key pads
 * connected by a large value resistor.  Assumes key to be BIT7 or BIT4. */
unsigned int measure_key_capacitance( unsigned int key ) {
    static unsigned int sum;

    P1OUT &= ~(BIT7 + BIT4);    // Start with both keys low.

    /* charge key */
    P1OUT |= key;
    asm( "nop \n\t" "nop \n\t" "nop \n\t" );

    /* Set up interrupt to trigger on key. */
    P1IES |= key;       // Trigger on voltage drop.
    P1IE |= key;        // Interrupt on.
    P1DIR &= ~key;      // Float key and let voltage drop.

    timer_count = TAR;  // Get timer (to compare with in interrupt).
    __bis_SR_register( LPM0_bits + GIE );       // Sleep.

    P1IE &= ~key;       // Disable interrupts on key.
    P1OUT &= ~key;      // Discharge key by setting
    P1DIR |= key;       // active low.

    sum = timer_count;  // Save the count that was recorded in interrupt.

    /* Charge the complement line. */
    P1OUT |= (BIT7 + BIT4)^key;
    asm( "nop \n\t" "nop \n\t" "nop \n\t" );

    /* Set up interrupt to trigger on key. */
    P1IES &= ~key;      // Trigger on voltage rise.
    P1IE |= key;        // Interrupt on.
    P1DIR &= ~key;      // Float key and let voltage rise.

    timer_count = TAR;  // Get timer (to compare with in interrupt).
    __bis_SR_register( LPM0_bits + GIE );

    P1IE &= ~key;       // Disable interrupts on key.
    P1OUT &= ~(BIT7 + BIT4);    // Set both keys to
    P1DIR |=  (BIT7 + BIT4);    // active low.

    return sum + timer_count;   // Return the sum of both counts.
}









/* Computes a trimmed mean of 18 capacitance values, trimming the largest and
 * smallest samples. */
unsigned int sample_key( unsigned char key ) {
    long int total;
    int i, j, small, large, cap;

    total = 0;

    /* Measure once to initialize max and min values. */
    small = large = measure_key_capacitance( key );

    /* Seventeen more samples happen here. Each time we decide whether the new
     * sample is kept or if it replaces one of the extremes. */
    for( i = 1; i < 18; i++ ) {
        cap = measure_key_capacitance( key );
        if( cap < small ) {
            total += small;
            small = cap;
        } else if( cap > large ) {
            total += large;
            large = cap;
        } else {
            total += cap;
        }

        // Add some jitter here, for more effective sampling.
        for( j=0; j < (cap&0x0F); j++ ) asm( "nop \n\t" );
    }

    /* We average 16 values (not including the two extremes) */
    return total >> 4;
}

/* Returns 1 if the key is touched (sufficiently past the baseline
 * value) and 0 otherwise.  Sampling can take a fair bit of time, so
 * we adjust the counter for timerA. */
int key_touched( unsigned char key, unsigned int baseline ) {
    unsigned int timeElapsed=0;
    int touch;

    /* Time elapsed is computed to adjust the delay time. */
    timeElapsed = TAR;

    touch = ( 4*sample_key(key) > 5*baseline );

    /* Adjust the delay to account for key sampling time. */
    countAdjust += TAR - timeElapsed;

    return touch;
}




/* This interrupt counts the durations for playing dots and dashes,
 * as well as the durations for dot-spaces and dash-spaces. */
void TimerA0(void) __attribute__((interrupt(TIMERA0_VECTOR)));
void TimerA0(void) {
    CCR0 += 80;

    if( countAdjust >= 80 ) {
        counter -= countAdjust/80;
        countAdjust = countAdjust%80;
    }

    if( counter-- > 0 ) {
        if( toneOn ) {
            P1OUT ^= AUDIO;
        }
    } else {
        /* Wake up main program when count is finished. */
        __bic_SR_register_on_exit(LPM0_bits);
    }
}






void WDT(void) __attribute__((interrupt(WDT_VECTOR)));
void WDT(void) {
    /* wake up the main program. */
    __bic_SR_register_on_exit(LPM3_bits);
}






void do_space_with_polling( int guard, int minwait, int maxwait ) {
    /* Waits the specified amount of time with polling at 4 points, at
     * the beginning, at the end of the guard time, at the end of the minwait
     * and at the end of the maxwait.  Guard space keeps us from accidentally
     * adding another symbol, but a dash is fine after a dot, and a dot is fine
     * after a dash. Assumes guard <= minwait <= maxwait.*/

    if( guard > 0 ) {
        /* Immediately poll for a key, guarding against doubles. */
        if( state != dash ) {
            dashKey = key_touched(DASH, base_dash);
        }

        if( state != dot ) {
            dotKey = key_touched(DOT, base_dot);
        }

        if( dotKey || dashKey ) {
            maxwait = minwait;                  // Return as soon as possible.
        }

        // Sleep past the guard time.
        counter = guard;
        CCR0 = TAR+80;  // 80 * 4us = 320us -- for tone generation (for delay here)
        CCTL0 |= CCIE;
        __bis_SR_register(LPM0_bits + GIE);
        CCTL0 &= ~CCIE;
    }

    if( minwait > guard ) {
        /* Guard time has passed.  Poll for a key. */
        if( !( dotKey || dashKey )) {
            dashKey = key_touched(DASH, base_dash);
            dotKey = key_touched(DOT, base_dot);

            if( (state == dot) && dashKey ) {           // Make sure both keys alternates.
                dotKey = 0;
            }

            if( dotKey || dashKey ) {
                maxwait = minwait;                      // Return as soon as possible.
            }
        }

        // Sleep past the minwait time.
        counter = minwait - guard;
        CCR0 = TAR+80;  // 80 * 4us = 320us -- for tone generation (for delay here)
        CCTL0 |= CCIE;
        __bis_SR_register(LPM0_bits + GIE);
        CCTL0 &= ~CCIE;
    }

    if( maxwait > minwait ) {
        /* Minwait time has passed.  Poll for a key. */
        if( !( dotKey || dashKey )) {
            dashKey = key_touched(DASH, base_dash);
            dotKey = key_touched(DOT, base_dot);

            if( dotKey || dashKey ) {
                return;
            }
        }

        // Sleep to the maxwait time.
        counter = maxwait - minwait;
        CCR0 = TAR+80;  // 80 * 4us = 320us -- for tone generation (for delay here)
        CCTL0 |= CCIE;
        __bis_SR_register(LPM0_bits + GIE);
        CCTL0 &= ~CCIE;
    }

    /* Maxwait time has passed.  Poll for a key. */
    if( !( dotKey || dashKey )) {
        dashKey = key_touched(DASH, base_dash);
        dotKey = key_touched(DOT, base_dot);
    }
}







void play_welcome( char *str ) {
    CCR0 = TAR+80;
    CCTL0 |= CCIE;      // CCR0 interrupt enabled

    while( *str ) {
        toneOn = 1;
        counter = ( *str == '-' ) ? 3*dotCount : dotCount;
        __bis_SR_register(LPM0_bits + GIE);

        P1OUT &= ~AUDIO;

        toneOn = 0;
        counter = dotCount;
        __bis_SR_register(LPM0_bits + GIE);
        str++;
    }

    CCTL0 &= ~CCIE;
}



void delay_ms(unsigned int ms)
{
    unsigned int i;
    for (i = 0; i<= ms; i++)
    __delay_cycles(1000);
}





int main(void) {
    int i;
   // int flag1=0, flag2=0, int j;
    unsigned int samp1, samp2;

    /* Stop the watchdog timer so it doesn't reset our chip. */
    WDTCTL = WDTPW + WDTHOLD;
    /* Set ACLK to use VLO so we can sleep in LPM3 when idle. */
    BCSCTL3 |= LFXT1S_2;

    /* Set all output pins low. */
    P1OUT = 0x00;
    P1DIR = 0xFF; //(RED + GRN + AUDIO)

    /* Set clock speed at 1Mhz. */
    BCSCTL1 = CALBC1_1MHZ;
    DCOCTL = CALDCO_1MHZ;

    /* Both LEDs are output lines */
    P1DIR |= RED + GRN;
    P1OUT &= ~(RED + GRN);

    /* So is the Audio pin. */
    P1DIR |= AUDIO;
    P1OUT &= ~AUDIO;

 
  // P1DIR |= ~FASTERBUTTON;
  // P1OUT |= FASTERBUTTON;		//  set, else reset
    P1REN |= FASTERBUTTON;		//  internal pullup resistor
 //  P1IE &= ~FASTERBUTTON;		//  interrupt disabled
  //  P1IES |= FASTERBUTTON;		//  Hi/Lo edge
/*
       if (FASTERBUTTON & P1IN){
       wpm++;  
       P1OUT |= (RED + AUDIO);
       delay_ms(500); //flasheo indicando que se incremento el wpm
       P1OUT &= ~(RED + AUDIO);
       }



while (flagwpm)                                 // Test FASTERBUTTON
  {
    delay_ms(2000); //mejora debounce
    if ((FASTERBUTTON & P1IN) == FALSE){
       wpm++;  
       P1OUT |= (RED + AUDIO);
       delay_ms(500); //flasheo indicando que se incremento el wpm
       P1OUT &= ~(RED + AUDIO);
      }
    if (FASTERBUTTON & P1IN){
       delay_ms(2000); 
       if (FASTERBUTTON & P1IN){
          delay_ms(2000);
          flagwpm = 0;
          }
      
      }


  }


*/

    /* One word per minute is 50 dots, which makes each dot 6/5 seconds.  Since
     * there are 1e6/4 clock ticks per second (because of the clock divider),
     * and the counter is moved every 80 ticks, the length of a 1 wpm dot is
     * 1e6/4/80*(6/5) = 3750. */
    dotCount = 3750 / wpm;/*3750*/
    guardSpace = dotCount/2;    // guard space is 1/2 of a dot

    /* Setup for capacitive touch.  Timer A is in count up mode, driven by the
     * submain clock (1 Mhz) with a clock divider of 2^2=4. The pins for our
     * touch pads are set as output pins. Same settings used for tone generation. */
    TACTL = MC_2 + TASSEL_2 + ID_2 + TACLR;     // count up mode, SMCLK, /4

    play_welcome("-.-.");       // "C"

    /* Get maximum baseline values for each key.  In the main loop, more than
     * 125% of the baseline value will indicate a touch event. */
    for( i = 0; i < 32; i++ ) {
        samp1 = sample_key(DASH);
        if( samp1 > base_dash ) base_dash = samp1;

        samp2 = sample_key(DOT);
        if( samp2 > base_dot ) base_dot = samp2;
    }

    play_welcome("--.-");       // "Q"

    while( 1 ) {
        if( !( dotKey || dashKey )) {
            /* One paddle sends dashes. */
            dashKey = key_touched(DASH, base_dash);
            /* The other paddle sends dots. */
            dotKey = key_touched(DOT, base_dot);
        }


        if( dotKey ) {
            state = dot;

            counter = dotCount;
            toneOn = 1;         // Play tone.

            countAdjust = 0;    // no need to adjust during tone
            CCR0 = TAR+80;      // 80 * 4us = 320us -- for tone generation
            CCTL0 |= CCIE;      // CCR0 interrupt enabled
            __bis_SR_register(LPM0_bits + GIE);
            CCTL0 &= ~CCIE;     // CCR0 interrupt disabled
        } else if( dashKey ) {
            state = dash;

            counter = 3*dotCount;
            toneOn = 1;         // Play tone.

            countAdjust = 0;    // no need to adjust during tone
            CCR0 = TAR+80;  // 80 * 4us = 320us -- for tone generation
            CCTL0 |= CCIE;      // CCR0 interrupt enabled
            __bis_SR_register(LPM0_bits + GIE);
            CCTL0 &= ~CCIE;     // CCR0 interrupt disabled
        } else {
            /* Set watchdog timer interval to wake us up in 43.7 ms
             * (normally it would be 16 ms, but the VLO is slower). */
            WDTCTL = WDT_ADLY_16;
            IE1 |= WDTIE;
            /* Go to sleep to save power until awoken by timer. */
            __bis_SR_register(LPM3_bits + GIE);
            /* Hold the watchdog clock. */
            WDTCTL = WDTPW + WDTHOLD;
            continue;
        }

        dotKey = dashKey = 0;

        /* Need at least a dot-space now, maybe more. */
        toneOn = 0;
        P1OUT &= ~AUDIO;

        /* 0. Keys within this first space will add symbols to the current letter.
         * We'll guard for dots after a dot, and dashes after a dash.  We'll accept
         * (sloppy) keys up to half a dot late. */
        do_space_with_polling( guardSpace, dotCount, dotCount+dotCount/2 );

        /* If a key was pressed, go back to the top to process it. */
        if( dotKey || dashKey ) continue;

        state = idle;

        P1OUT |= RED;   // visually indicate dash space between letters

        /* 1.5 Now we finish the dash space, and start a new letter.  No guard time.
         * We'll accept (keys) up to half a dot late, for starting the next letter. */
        do_space_with_polling( 0, dotCount+dotCount/2, 2*dotCount );

        P1OUT &= ~RED;  // end of dash space

        /* If a key was pressed, go back to the top to process it. */
        if( dotKey || dashKey ) continue;

        /* 3.5 Another three and a half dots finishes the word space.  Start with
         * two and a half dots without polling. */

        P1OUT |= GRN;   // visually indicate word space

        counter = 2*dotCount+ dotCount/2;
        CCR0 = TAR+80;  // 80 * 4us = 320us -- for tone generation (for delay here)
        CCTL0 |= CCIE;      // CCR0 interrupt enabled
        __bis_SR_register(LPM0_bits + GIE);
        CCTL0 &= ~CCIE;     // CCR0 interrupt disabled

        P1OUT &= ~GRN;  // visually indicate ready again

        /* 7. A final dot space finishes the word-space. */
        do_space_with_polling( 0, dotCount, dotCount );

        /* Idle.  Loop back to the top. */
    }
}
