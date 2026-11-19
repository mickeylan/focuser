/*
 * main_recovered.c - reconstructed main() for the focus controller.
 *
 * The original main was FUN_08007b58 (@0x08007b58) and ended in an empty
 * infinite loop: every unit of work ran from an interrupt (timer step pulses,
 * USART3 reception, USB CDC reception). The init sequence below mirrors the
 * recovered call order; see docs/RECONSTRUCTION.md for the mapping.
 */
#include "board_stm32f1.h"

int main(void)
{
    focus_board_init();

    for (;;) {
        focus_board_tick();
    }
}
