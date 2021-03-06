/* spi_master example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include "esp8266/spi_struct.h"
#include "esp8266/gpio_struct.h"
#include "esp_system.h"
#include "esp_log.h"

#include "driver/gpio.h"
#include "driver/spi.h"
#include "driver/uart.h"

static const char* TAG = "at_spi_master";

#define SPI_MASTER_HANDSHARK_GPIO     4
#define SPI_MASTER_HANDSHARK_SEL      (1ULL<<SPI_MASTER_HANDSHARK_GPIO)

#define SPI_BUFFER_MAX_SIZE               4096

static StreamBufferHandle_t spi_master_send_ring_buf = NULL;
static StreamBufferHandle_t spi_master_recv_ring_buf = NULL;
static uint32_t transmit_len = 0;
static bool wait_recv_data = false;

#define RD_BUF_SIZE 1024
static QueueHandle_t uart0_queue;

typedef enum {
    SPI_NULL = 0,
    SPI_WRITE,
    SPI_READ
} spi_master_mode_t;

static spi_master_mode_t intr_trans_mode = SPI_NULL;

/* SPI master send length, format: 8bit command(value:1) + 32bit status length */
static void IRAM_ATTR spi_master_send_length(uint32_t len)
{
    spi_trans_t trans;
    uint16_t cmd = SPI_MASTER_WRITE_STATUS_TO_SLAVE_CMD;
    memset(&trans, 0x0, sizeof(trans));
    trans.bits.val = 0;
    trans.bits.cmd = 8 * 1;
    trans.bits.addr = 0;          // transmit status do not use address bit
    trans.bits.mosi = 8 * 4;      // status length is 32bit
    trans.cmd = &cmd;
    trans.addr = NULL;
    trans.mosi = &len;
    spi_trans(HSPI_HOST, &trans);
}

/* SPI master revecive length, format: 8bit command(value:4) + 32bit status length */
static uint32_t IRAM_ATTR spi_master_get_length(void)
{
    spi_trans_t trans;
    uint32_t len = 0;
    uint16_t cmd = SPI_MASTER_READ_STATUS_FROM_SLAVE_CMD;
    memset(&trans, 0x0, sizeof(trans));
    trans.bits.val = 0;
    trans.cmd = &cmd;
    trans.miso = &len;
    trans.addr = NULL;
    trans.bits.cmd = 8 * 1;
    trans.bits.miso = 8 * 4;
    spi_trans(HSPI_HOST, &trans);
    return len;
}

/* SPI transmit data, format: 8bit command (read value: 3, write value: 4) + 8bit address(value: 0x0) + 64byte data
*  For convenience, every time we send 64bytes, SPI SLAVE will determine how much data to read based on the status value
*/
static void IRAM_ATTR spi_master_transmit(spi_master_mode_t trans_mode, uint32_t* data)
{
    spi_trans_t trans;
    uint16_t cmd;
    uint32_t addr = 0x0;

    memset(&trans, 0x0, sizeof(trans));
    trans.bits.val = 0;            // clear all bit

    if (trans_mode == SPI_WRITE) {
        cmd = SPI_MASTER_WRITE_DATA_TO_SLAVE_CMD;
        trans.bits.mosi = 8 * 64;             // One time transmit only support 64bytes
        trans.mosi = data;
    } else if (trans_mode == SPI_READ) {
        cmd = SPI_MASTER_READ_DATA_FROM_SLAVE_CMD;
        trans.bits.miso = 8 * 64;
        trans.miso = data;
    }

    trans.bits.cmd = 8 * 1;
    trans.bits.addr = 8 * 1;     // transmit data will use 8bit address
    trans.cmd = &cmd;
    trans.addr = &addr;

    spi_trans(HSPI_HOST, &trans);
}

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint32_t read_len = 0;
    uint32_t recv_actual_len = 0;
    uint32_t transmit_data[16];

    if (intr_trans_mode == SPI_NULL) {       // Some data need to read or write???
        // have some data need to send ???
        if (xStreamBufferIsEmpty(spi_master_send_ring_buf) == pdFALSE) {
            intr_trans_mode = SPI_WRITE;
            transmit_len = xStreamBufferBytesAvailable(spi_master_send_ring_buf);
            ESP_EARLY_LOGD(TAG, "Send len: %d\n", transmit_len);
            spi_master_send_length(transmit_len);
            return;
        }

        // Check if there is any data to receive
        transmit_len  = spi_master_get_length();

        if (transmit_len > 0) {
            ESP_EARLY_LOGD(TAG, "Receive data len: %d\n", transmit_len);
            intr_trans_mode = SPI_READ;
            return;
        } else {
            ESP_EARLY_LOGE(TAG, "Nothing to do");
            return;
        }
    }

    read_len =  transmit_len > 64 ? 64 : transmit_len;

    // SPI slave have some data want to transmit, read it
    if (intr_trans_mode == SPI_READ) {
        if (xStreamBufferSpacesAvailable(spi_master_recv_ring_buf) >= 64) {    // Stream buffer not full, can be read agian
            spi_master_transmit(SPI_READ, transmit_data);
            recv_actual_len = xStreamBufferSendFromISR(spi_master_recv_ring_buf, (void*) transmit_data, read_len, &xHigherPriorityTaskWoken);
            assert(recv_actual_len == read_len);
            transmit_len -= read_len;

            if (transmit_len == 0) {
                intr_trans_mode = SPI_NULL;

                /* When SPI slave sending data , maybe MCU also have some date wait for send */
                if (xStreamBufferIsEmpty(spi_master_send_ring_buf) == pdFALSE) {
                    GPIO.status_w1ts |= BIT(SPI_MASTER_HANDSHARK_GPIO);   // Manual generate GPIO interrupts
                }
            }
        } else {   // stream buffer full, wait to be tacken out
            wait_recv_data = true;
        }


        // MCU want to send data to ESP8266
    } else if (intr_trans_mode == SPI_WRITE) {
        if (read_len > 0) {
            recv_actual_len = xStreamBufferReceiveFromISR(spi_master_send_ring_buf,
                              (void*)transmit_data,
                              read_len,
                              &xHigherPriorityTaskWoken);

            if (recv_actual_len != read_len) {
                ESP_EARLY_LOGE(TAG, "Expect to send %d bytes, but only %d bytes", read_len, recv_actual_len);
                return;
            }

            spi_master_transmit(SPI_WRITE, transmit_data);
            transmit_len -= read_len;
        } else {
            intr_trans_mode = SPI_NULL;

            if (xStreamBufferIsEmpty(spi_master_send_ring_buf) == pdFALSE) {
                GPIO.status_w1ts |= BIT(SPI_MASTER_HANDSHARK_GPIO); // Manual generate GPIO interrupts
            } else {
                // if ring buffer is empty, send status=0 tell slave send done
                spi_master_send_length(0);
            }
        }
    }

    if (xHigherPriorityTaskWoken == pdTRUE) {
        taskYIELD();
    }
}

// SPI master sent to slave function
static int8_t IRAM_ATTR at_spi_load_data(uint8_t* buf, int32_t len)
{
    if (len > SPI_BUFFER_MAX_SIZE) {
        ESP_LOGE(TAG, "Send length %d is too large", len);
        return -1;
    }

    size_t xBytesSent = xStreamBufferSend(spi_master_send_ring_buf, (void*) buf, len, portMAX_DELAY);

    if (xBytesSent != len) {
        ESP_LOGE(TAG, "Send error, len:%d", xBytesSent);
        return -1;
    }

    portENTER_CRITICAL();

    if (intr_trans_mode == SPI_NULL) {
        ESP_LOGD(TAG, "Manual generate GPIO interrupts");
        GPIO.status_w1ts |= BIT(SPI_MASTER_HANDSHARK_GPIO);
    }

    portEXIT_CRITICAL();

    return 0;
}

static void IRAM_ATTR spi_master_read_slave_task(void* arg)
{
    size_t xReceivedBytes;
    uint8_t read_data[1024 + 1];

    while (1) {
        xReceivedBytes = xStreamBufferReceive(spi_master_recv_ring_buf, read_data, 1024, 2000 / portTICK_RATE_MS);

        if (xReceivedBytes != 0) {
            read_data[xReceivedBytes] = '\0';
            printf("%s", read_data);
            fflush(stdout);    //Force to print even if have not '\n'
        }

        // steam buffer full
        if (wait_recv_data) {
            if (xStreamBufferBytesAvailable(spi_master_recv_ring_buf) > 64) {
                wait_recv_data = false;
                GPIO.status_w1ts |= BIT(SPI_MASTER_HANDSHARK_GPIO); // Manual generate GPIO interrupts
            }
        }
    }
}

static void uart_event_task(void* pvParameters)
{
    uart_event_t event;
    uint8_t* dtmp = (uint8_t*) malloc(RD_BUF_SIZE);

    for (;;) {
        // Waiting for UART event.
        if (xQueueReceive(uart0_queue, (void*)&event, (portTickType)portMAX_DELAY)) {
            bzero(dtmp, RD_BUF_SIZE);

            switch (event.type) {
                    // Event of UART receving data
                    // We'd better handler data event fast, there would be much more data events than
                    // other types of events. If we take too much time on data event, the queue might be full.
                case UART_DATA:
                    ESP_LOGD(TAG, "[UART DATA len]: %d", event.size);
                    uart_read_bytes(UART_NUM_0, dtmp, event.size, portMAX_DELAY);
                    at_spi_load_data(dtmp, event.size);
                    break;

                    // Others
                default:
                    ESP_LOGE(TAG, "uart event type: %d", event.type);
                    break;
            }
        }
    }

    free(dtmp);
    dtmp = NULL;
    vTaskDelete(NULL);
}

void app_main(void)
{
    spi_master_send_ring_buf = xStreamBufferCreate(SPI_BUFFER_MAX_SIZE, 1024);
    spi_master_recv_ring_buf = xStreamBufferCreate(SPI_BUFFER_MAX_SIZE, 1);

    ESP_LOGI(TAG, "init gpio");
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = SPI_MASTER_HANDSHARK_SEL;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(SPI_MASTER_HANDSHARK_GPIO, gpio_isr_handler, (void*) SPI_MASTER_HANDSHARK_GPIO);

    ESP_LOGI(TAG, "init spi");
    spi_config_t spi_config;
    // Load default interface parameters
    // CS_EN:1, MISO_EN:1, MOSI_EN:1, BYTE_TX_ORDER:1, BYTE_TX_ORDER:1, BIT_RX_ORDER:0, BIT_TX_ORDER:0, CPHA:0, CPOL:0
    spi_config.interface.val = SPI_DEFAULT_INTERFACE;

    // Load default interrupt enable
    // TRANS_DONE: true, WRITE_STATUS: false, READ_STATUS: false, WRITE_BUFFER: false, READ_BUFFER: false
    spi_config.intr_enable.val = SPI_MASTER_DEFAULT_INTR_ENABLE;
    // Set SPI to master mode
    // ESP8266 Only support half-duplex
    spi_config.mode = SPI_MASTER_MODE;
    // Set the SPI clock frequency division factor
    spi_config.clk_div = SPI_20MHz_DIV;
    // Register SPI event callback function
    spi_config.event_cb = NULL;
    spi_init(HSPI_HOST, &spi_config);

    // Configure parameters of an UART driver,
    // communication pins and install the driver
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_NUM_0, &uart_config);

    // Install UART driver, and get the queue.
    uart_driver_install(UART_NUM_0, 2048, 0, 100, &uart0_queue, 0);

    // Create a task to handler UART event from ISR
    xTaskCreate(uart_event_task, "uart_event_task", 2048, NULL, 3, NULL);

    // create spi_master_read_slave_task
    xTaskCreate(spi_master_read_slave_task, "spi_master_read_slave_task", 2048, NULL, 5, NULL);
}


