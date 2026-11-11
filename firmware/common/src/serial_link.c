/* serial_link: see serial_link.h. Defines the USART1 and DMA1 stream 0 handlers
 * and the HAL's UART callbacks, so it is an OBJECT library (CMakeLists.txt). */
#include "serial_link.h"
#include "board.h"

/* The DMA's item counter is 16 bits, so one transfer moves at most 65,535 bytes. */
#define TX_CHUNK        32768
#define HOST_TIMEOUT_MS 1500

static DMA_HandleTypeDef hdma_tx;
static const uint8_t *tx_buf;
static uint32_t tx_offset, tx_len;
static volatile uint32_t tx_busy;

static uint8_t rx_byte;
static volatile uint32_t host_last_ms;
static volatile uint8_t command;

void DMA1_Stream0_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_tx); }
void USART1_IRQHandler(void)       { HAL_UART_IRQHandler(&hcom_uart[COM1]); }

/* ---- sending: one DMA chunk at a time; each completion starts the next ---------- */
static void send_next_chunk(void)
{
  uint32_t n = tx_len - tx_offset;
  if (n > TX_CHUNK) n = TX_CHUNK;
  if (HAL_UART_Transmit_DMA(&hcom_uart[COM1], (uint8_t *)tx_buf + tx_offset, (uint16_t)n) != HAL_OK)
    Error_Handler();
  tx_offset += n;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart != &hcom_uart[COM1]) return;
  if (tx_offset < tx_len)
    send_next_chunk();
  else
    tx_busy = 0;
}

void link_send(const uint8_t *buf, uint32_t len)
{
  /* The DMA reads SRAM, not the D-cache: push the CPU's writes out first. */
  SCB_CleanDCache_by_Addr((uint32_t *)buf, (int32_t)((len + 31) & ~31u));
  tx_buf = buf;
  tx_len = len;
  tx_offset = 0;
  tx_busy = 1;
  send_next_chunk();
}

int link_busy(void) { return tx_busy; }

/* ---- receiving: one byte per interrupt, re-armed each time ----------------------- */
static void listen_for_host(void)
{
  if (hcom_uart[COM1].RxState == HAL_UART_STATE_READY)
    HAL_UART_Receive_IT(&hcom_uart[COM1], &rx_byte, 1);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart != &hcom_uart[COM1]) return;
  host_last_ms = HAL_GetTick();
  if (rx_byte == 'G' || rx_byte == 'S') command = rx_byte;
  listen_for_host();
}

/* An overrun or framing error ends the reception; start it again. */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart == &hcom_uart[COM1]) listen_for_host();
}

int link_host_listening(void)
{
  listen_for_host();  /* in case an error callback couldn't re-arm it */
  return host_last_ms != 0 && HAL_GetTick() - host_last_ms < HOST_TIMEOUT_MS;
}

int link_take_command(void)
{
  int c = command;
  command = 0;
  return c;
}

void link_init(uint32_t baud)
{
  board_console_init(baud);

  /* DMAMUX routes the USART1_TX request to DMA1 stream 0. */
  __HAL_RCC_DMA1_CLK_ENABLE();
  hdma_tx.Instance = DMA1_Stream0;
  hdma_tx.Init.Request = DMA_REQUEST_USART1_TX;
  hdma_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
  hdma_tx.Init.PeriphInc = DMA_PINC_DISABLE;
  hdma_tx.Init.MemInc = DMA_MINC_ENABLE;
  hdma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
  hdma_tx.Init.Mode = DMA_NORMAL;
  hdma_tx.Init.Priority = DMA_PRIORITY_LOW;
  hdma_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
  if (HAL_DMA_Init(&hdma_tx) != HAL_OK) Error_Handler();
  __HAL_LINKDMA(&hcom_uart[COM1], hdmatx, hdma_tx);

  HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 14, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
  HAL_NVIC_SetPriority(USART1_IRQn, 14, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
  listen_for_host();
}

/* Reflected polynomial 0xEDB88320; the 1 KB table makes a frame ~1 ms. */
uint32_t link_crc32(const uint8_t *p, uint32_t n)
{
  static uint32_t table[256];
  if (!table[1])
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  uint32_t c = 0xFFFFFFFFu;
  while (n--) c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}
