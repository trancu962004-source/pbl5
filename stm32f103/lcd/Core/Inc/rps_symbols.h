#ifndef RPS_SYMBOLS_H
#define RPS_SYMBOLS_H

#include <stdint.h>

#define RPS_KEO      0
#define RPS_BUA      1
#define RPS_BAO      2
#define RPS_UNKNOWN  3
#define RPS_EMPTY    4
#define RPS_WIN      5
#define RPS_LOSE     6
#define RPS_DRAW     7

#define RPS_SCISSORS RPS_KEO
#define RPS_ROCK     RPS_BUA
#define RPS_PAPER    RPS_BAO

extern uint8_t RPS_keo[8];
extern uint8_t RPS_bua[8];
extern uint8_t RPS_bao[8];
extern uint8_t RPS_unknown[8];
extern uint8_t RPS_empty[8];
extern uint8_t RPS_win[8];
extern uint8_t RPS_lose[8];
extern uint8_t RPS_draw[8];

#define RPS_scissors RPS_keo
#define RPS_rock     RPS_bua
#define RPS_paper    RPS_bao

#endif
