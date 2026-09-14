#include "main.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"
#include "stdio.h"
#include "stdlib.h"
#include "CLCD_I2C.h"
#include "rps_symbols.h"

/********************************************
Check/Add in FreeRTOSConfig.h:
#define configUSE_COUNTING_SEMAPHORES     1
#define configUSE_MUTEXES                 1
#define configUSE_TIMERS                  1
#define configUSE_TASK_NOTIFICATIONS      1
********************************************/

#define LCD_ADDR 0x4E     // LCD 0x27 => 0x4E. Neu LCD 0x3F thi doi thanh 0x7E
#define ROUND_MAX 10

typedef enum {START, PLAYING, LEVEL_WIN, LEVEL_LOSE} gameSTATE;
typedef enum {NONE = 0xFF, ROCK = 0, PAPER = 1, SCISSORS = 2} rpsMOVE;
typedef struct {
  uint32_t level, round, win, lose, draw, timeout;
  gameSTATE state;
} gameINFO;

I2C_HandleTypeDef hi2c1;
CLCD_I2C_Name LCD1;
char lcd_text[20];

gameINFO mygame = {1, 0, 0, 0, 0, 0, START};
rpsMOVE machine_list[ROUND_MAX];
rpsMOVE player_list[ROUND_MAX];

static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
void SystemClock_Config(void);
void Error_Handler(void);

void check_LCD_address(I2C_HandleTypeDef *hi2c, const uint8_t addr, int trails, int timeout);
void task_lcd(void * params);
void task_button(void * params);
void task_game(void * params);
void timer_input_callback(TimerHandle_t xTimer);

TaskHandle_t button_handle, game_handle, lcd_handle;
TimerHandle_t input_timer_hd;
QueueHandle_t qplayer_hd;
SemaphoreHandle_t dataMutex, taskLCD_sem, round_sem;

static uint32_t btn_to_move(uint32_t code){
  if(code == 0x0001) return SCISSORS;  // PB14 = KEO
  if(code == 0x0008) return ROCK;      // PA10 = BUA
  if(code == 0x0040) return PAPER;     // PA15 = BAO
  return NONE;
}

static uint8_t move_to_char(rpsMOVE move){
  if(move == ROCK) return RPS_BUA;
  if(move == PAPER) return RPS_BAO;
  if(move == SCISSORS) return RPS_KEO;
  return RPS_UNKNOWN;
}

static int player_result(rpsMOVE player, rpsMOVE machine){
  if(player == machine) return 0;
  if((player == ROCK && machine == SCISSORS) ||
     (player == PAPER && machine == ROCK) ||
     (player == SCISSORS && machine == PAPER)) return 1;
  return -1;
}

static uint32_t input_time_ms(uint32_t level){
  if(level >= 10) return 1500;
  return 4000 - (level - 1) * 250;
}

static void clear_round_data(void){
  for(int i = 0; i < ROUND_MAX; i++){
    machine_list[i] = NONE;
    player_list[i] = NONE;
  }
}

static void start_new_level(uint32_t level){
  mygame.level = level;
  mygame.round = 0;
  mygame.win = 0;
  mygame.lose = 0;
  mygame.draw = 0;
  mygame.timeout = 0;
  mygame.state = PLAYING;

  clear_round_data();
  xQueueReset(qplayer_hd);
  xQueueReset(round_sem);
  for(int i = 0; i < ROUND_MAX; i++) xSemaphoreGive(round_sem);

  xTimerChangePeriod(input_timer_hd, pdMS_TO_TICKS(input_time_ms(mygame.level)), 0);
  xTimerStart(input_timer_hd, 0);
}

int main(void){
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_I2C1_Init();

  check_LCD_address(&hi2c1, LCD_ADDR, 3, 100);
  CLCD_I2C_Init(&LCD1, &hi2c1, LCD_ADDR, 16, 2);

  // Test LCD truoc khi vao RTOS. Neu khong hien dong nay thi sai dia chi/day/tuong phan.
  CLCD_I2C_Clear(&LCD1);
  CLCD_I2C_SetCursor(&LCD1, 0, 0);
  CLCD_I2C_WriteString(&LCD1, "LCD OK");
  CLCD_I2C_SetCursor(&LCD1, 0, 1);
  CLCD_I2C_WriteString(&LCD1, "RPS GAME");
  HAL_Delay(1000);

  dataMutex = xSemaphoreCreateMutex();
  taskLCD_sem = xSemaphoreCreateBinary();
  round_sem = xSemaphoreCreateCounting(ROUND_MAX, 0);
  qplayer_hd = xQueueCreate(10, sizeof(uint32_t));

  input_timer_hd = xTimerCreate("input timer", pdMS_TO_TICKS(4000), pdFALSE, NULL, timer_input_callback);

  xTaskCreate(task_lcd,    "LCD task",    256, NULL, 2, &lcd_handle);
  xTaskCreate(task_button, "Button task", 256, NULL, 3, &button_handle);
  xTaskCreate(task_game,   "Game task",   256, NULL, 2, &game_handle);

  // Cho LCD hien man hinh START ngay khi scheduler chay
  xSemaphoreGive(taskLCD_sem);

  vTaskStartScheduler();

  while(1){
    CLCD_I2C_Clear(&LCD1);
    CLCD_I2C_SetCursor(&LCD1, 0, 0);
    CLCD_I2C_WriteString(&LCD1, "OS START FAIL");
    HAL_Delay(500);
  }
}

void timer_input_callback(TimerHandle_t xTimer){
  uint32_t code = 0xFFFFFFFF; // het gio, nguoi choi khong bam
  xQueueSend(qplayer_hd, &code, 0);
}

void task_game(void * params){
  uint32_t player_code, machine;
  srand(HAL_GetTick());

  for(;;){
    xQueueReceive(qplayer_hd, &player_code, portMAX_DELAY);

    xSemaphoreTake(dataMutex, portMAX_DELAY);

    // Dang o man hinh START/WIN/LOSE: bam nut de bat dau game hoac level moi
    if(mygame.state != PLAYING){
      uint32_t next_level = mygame.level;
      if(mygame.state == LEVEL_WIN) next_level = mygame.level + 1;
      start_new_level(next_level);
      xSemaphoreGive(dataMutex);
      xSemaphoreGive(taskLCD_sem);
      continue;
    }

    if(xSemaphoreTake(round_sem, 0) == pdFALSE){
      xSemaphoreGive(dataMutex);
      continue;
    }

    machine = rand() % 3;
    machine_list[mygame.round] = (rpsMOVE)machine;

    if(player_code == 0xFFFFFFFF || btn_to_move(player_code) == NONE){
      player_list[mygame.round] = NONE;
      mygame.timeout++;
      mygame.lose++;
    }else{
      rpsMOVE player = (rpsMOVE)btn_to_move(player_code);
      player_list[mygame.round] = player;

      int result = player_result(player, (rpsMOVE)machine);
      if(result > 0) mygame.win++;
      else if(result < 0) mygame.lose++;
      else mygame.draw++;
    }

    mygame.round++;

    if(mygame.round >= ROUND_MAX){
      xTimerStop(input_timer_hd, 0);
      if(mygame.win >= mygame.lose) mygame.state = LEVEL_WIN;
      else mygame.state = LEVEL_LOSE;
    }else{
      xTimerReset(input_timer_hd, 0);
    }

    xSemaphoreGive(dataMutex);
    xSemaphoreGive(taskLCD_sem);
  }
}

void task_button(void * params){
  uint32_t code;
  for(;;){
    xTaskNotifyWait(0, 0xFFFFFFFF, &code, portMAX_DELAY);
    xQueueSend(qplayer_hd, &code, portMAX_DELAY);
  }
}

void task_lcd(void* params){
  CLCD_I2C_CreateChar(&LCD1, RPS_ROCK,     RPS_rock);
  CLCD_I2C_CreateChar(&LCD1, RPS_PAPER,    RPS_paper);
  CLCD_I2C_CreateChar(&LCD1, RPS_SCISSORS, RPS_scissors);
  CLCD_I2C_CreateChar(&LCD1, RPS_UNKNOWN,  RPS_unknown);
  CLCD_I2C_CreateChar(&LCD1, RPS_EMPTY,    RPS_empty);
  CLCD_I2C_CreateChar(&LCD1, RPS_WIN,      RPS_win);
  CLCD_I2C_CreateChar(&LCD1, RPS_LOSE,     RPS_lose);
  CLCD_I2C_CreateChar(&LCD1, RPS_DRAW,     RPS_draw);

  for(;;){
    xSemaphoreTake(taskLCD_sem, portMAX_DELAY);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    gameINFO g = mygame;
    rpsMOVE m[ROUND_MAX], p[ROUND_MAX];
    for(int i = 0; i < ROUND_MAX; i++){
      m[i] = machine_list[i];
      p[i] = player_list[i];
    }
    xSemaphoreGive(dataMutex);

    CLCD_I2C_Clear(&LCD1);

    if(g.state == START){
      CLCD_I2C_SetCursor(&LCD1, 0, 0);
      CLCD_I2C_WriteString(&LCD1, "PB14 KEO PA10 B");
      CLCD_I2C_SetCursor(&LCD1, 0, 1);
      CLCD_I2C_WriteString(&LCD1, "PA15 BAO START");
    }
    else if(g.state == PLAYING){
      CLCD_I2C_SetCursor(&LCD1, 0, 0);
      CLCD_I2C_WriteString(&LCD1, "M:");
      for(int i = 0; i < ROUND_MAX; i++){
        CLCD_I2C_WriteChar(&LCD1, i < g.round ? move_to_char(m[i]) : RPS_UNKNOWN);
      }
      sprintf(lcd_text, " L%02d", (int)g.level);
      CLCD_I2C_WriteString(&LCD1, lcd_text);

      CLCD_I2C_SetCursor(&LCD1, 0, 1);
      CLCD_I2C_WriteString(&LCD1, "P:");
      for(int i = 0; i < ROUND_MAX; i++){
        CLCD_I2C_WriteChar(&LCD1, i < g.round ? move_to_char(p[i]) : RPS_EMPTY);
      }
      sprintf(lcd_text, " W%02d", (int)g.win);
      CLCD_I2C_WriteString(&LCD1, lcd_text);
    }
    else{
      CLCD_I2C_SetCursor(&LCD1, 0, 0);
      if(g.state == LEVEL_WIN) CLCD_I2C_WriteString(&LCD1, " WIN NEXT LEVEL");
      else CLCD_I2C_WriteString(&LCD1, " LOSE REPLAY");

      CLCD_I2C_SetCursor(&LCD1, 0, 1);
      sprintf(lcd_text, "W%d L%d D%d T%d", (int)g.win, (int)g.lose, (int)g.draw, (int)g.timeout);
      CLCD_I2C_WriteString(&LCD1, lcd_text);
    }
  }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin){
  BaseType_t xHPTW = pdFALSE;
  static uint32_t last_tick = 0;
  uint32_t now = xTaskGetTickCountFromISR();

  if(now - last_tick > pdMS_TO_TICKS(150)){
    last_tick = now;
    uint32_t code = 0;

    if(GPIO_Pin == GPIO_PIN_14)      code = 0x0001; // PB14 = KEO
    else if(GPIO_Pin == GPIO_PIN_10) code = 0x0008; // PA10 = BUA
    else if(GPIO_Pin == GPIO_PIN_15) code = 0x0040; // PA15 = BAO

    if(code != 0 && button_handle != NULL){
      xTaskNotifyFromISR(button_handle, code, eSetValueWithOverwrite, &xHPTW);
    }
    portYIELD_FROM_ISR(xHPTW);
  }
}

void EXTI15_10_IRQHandler(void){
  if(__HAL_GPIO_EXTI_GET_IT(GPIO_PIN_10)) HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_10);
  if(__HAL_GPIO_EXTI_GET_IT(GPIO_PIN_14)) HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_14);
  if(__HAL_GPIO_EXTI_GET_IT(GPIO_PIN_15)) HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_15);
}

void check_LCD_address(I2C_HandleTypeDef *hi2c, const uint8_t addr, int trails, int timeout){
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);

  if(HAL_I2C_IsDeviceReady(hi2c, addr, trails, timeout) == HAL_OK){
    for(int i = 0; i < 20; i++){
      HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
      HAL_Delay(50);
    }
  }else{
    for(int i = 0; i < 6; i++){
      HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
      HAL_Delay(500);
    }
  }

  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
}

void SystemClock_Config(void){
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if(HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if(HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK) Error_Handler();
}

static void MX_I2C1_Init(void){
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

  if(HAL_I2C_Init(&hi2c1) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void){
  GPIO_InitTypeDef InitStruct = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  // LED PC13
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
  InitStruct.Pin = GPIO_PIN_13;
  InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  InitStruct.Pull = GPIO_NOPULL;
  InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &InitStruct);

  // Nut PA10, PA15
  InitStruct.Pin = GPIO_PIN_10 | GPIO_PIN_15;
  InitStruct.Mode = GPIO_MODE_IT_FALLING;
  InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &InitStruct);

  // Nut PB14
  InitStruct.Pin = GPIO_PIN_14;
  InitStruct.Mode = GPIO_MODE_IT_FALLING;
  InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &InitStruct);

  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

void Error_Handler(void){
  __disable_irq();
  while(1){}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line){}
#endif
