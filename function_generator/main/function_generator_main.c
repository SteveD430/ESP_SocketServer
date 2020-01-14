/* Function Generator
 
*/
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "tcpip_adapter.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include <lwip/netdb.h>
#include <fcntl.h>

#define PORT 23
#define WIFI_MAXIMUM_RETRY 5
#define MAX_WIFI_CONNECTION_ATTEMPTS 25

#define WIFI_SSID       CONFIG_ESP_WIFI_SSID
#define WIFI_PASSWORD   CONFIG_ESP_WIFI_PASSWORD
#define MAX_STA_CONN    CONFIG_ESP_MAX_STA_CONN

#define CORE0 0
#define CORE1 1
#define BUFFER_SIZE 2000 // Size of double buffer for data compilation. This value will need to be calibrated.
#define ASCII_BUFFER_SIZE BUFFER_SIZE*5 // Buffer for Ascii85 data tx.

/*
  This set-up caters for two generators (GPIO 18 & 19) and two Reveivers (GPIO 4 & 5)
  However we are ony using GPIO 18 & GPIO 4.
 */
#define GPIO_OUTPUT_IO_0 18
#define GPIO_OUTPUT_IO_1 19
#define GPIO_OUTPUT_PIN_SEL ((1ULL<<GPIO_OUTPUT_IO_0) | (1ULL<<GPIO_OUTPUT_IO_1))
#define GPIO_INPUT_IO_0 4
#define GPIO_INPUT_IO_1 5
#define GPIO_INPUT_PIN_SEL ((1ULL<<GPIO_INPUT_IO_0) | (1ULL<<GPIO_INPUT_IO_1))
#define ESP_INTR_FLAG_DEFAULT 0


static const char* TAG = "wifi function_generator";

int buffers[2][BUFFER_SIZE];

static int listen_socket;
static int telnet_socket;
static bool wifiConnected = false;
static bool telnetClientConnected = false;

static TaskHandle_t taskSignalGenerator;
static TaskHandle_t taskSignalReceiver;
static TaskHandle_t taskDataCompilation;
static TaskHandle_t taskDataTransmission;
static TaskHandle_t taskSocketListenConnect;

void SignalReceiverTask (void *pvParameters);
void SignalGeneratorTask (void *pvParameters);
void DataCompilationTask (void *pvParameters);
void DataTransmissionTask (void *pvParameters);

void SocketListenConnectTask (void *pvParameters);

static xQueueHandle gpio_evt_queue = NULL;

uint32_t dataBuffers[2][BUFFER_SIZE];

/* The event group allows multiple bits for each event, but we only care about one event
 * - are we connected to the AP and do we have an IP? */
const int WIFI_CONNECTED_BIT = BIT0;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

void socketCreation ()
{
        printf (">> socketCreation \n");
        xTaskCreatePinnedToCore (SocketListenConnectTask,
                                 "SocketListenConnectTask",
                                 4096,
                                 NULL,
                                 0,
                                 &taskSocketListenConnect,
                                 CORE0);
    printf ("<< socketCreation \n");
}


static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:%s",
                 ip4addr_ntoa(&event->ip_info.ip));
        ESP_LOGI(TAG, "IP Address: %s", ip4addr_ntoa(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // Open Telnet, then trigger listening for client.
        socketCreation();;
    }
}


void wifi_setup()
{
    printf (">> wifi_setup \n");
    s_wifi_event_group = xEventGroupCreate();
    
    tcpip_adapter_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    
    ESP_ERROR_CHECK(
                    esp_event_handler_register(
                                               WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &event_handler,
                                               NULL));
    ESP_ERROR_CHECK(
                    esp_event_handler_register(
                                               IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &event_handler,
                                               NULL));
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    
    printf ("Starting WifI with SSID %s Passwd %s\n", WIFI_SSID, WIFI_PASSWORD);
    
    ESP_ERROR_CHECK(esp_wifi_start());
    wifiConnected = true;
    printf("WiFi started\n");
    printf ("<< wifi_setup \n");
    
}


static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_setup()
{
    printf (">> gpio_setup \n");
    
    gpio_config_t io_conf;
    //disable interrupt
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);
    //interrupt of rising edge
    io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);
    //change gpio intrrupt type for one pin
    gpio_set_intr_type(GPIO_INPUT_IO_0, GPIO_INTR_ANYEDGE);
    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    //gpio task - now started with other tasks.
    // xTaskCreate(signalGenerator, "gpio_task_example", 2048, NULL, 10, NULL);
    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_IO_0, gpio_isr_handler, (void*) GPIO_INPUT_IO_0);
    
    //hook isr handler for specific gpio pin
    //gpio_isr_handler_add(GPIO_INPUT_IO_1, gpio_isr_handler, (void*) GPIO_INPUT_IO_1);
    //remove isr handler for gpio number.
    gpio_isr_handler_remove(GPIO_INPUT_IO_0);
    //hook isr handler for specific gpio pin again
    gpio_isr_handler_add(GPIO_INPUT_IO_0, gpio_isr_handler, (void*) GPIO_INPUT_IO_0);
    
    printf ("<< gpio_setup \n");
}


void app_main()
{
    printf("Function Generator\n");
    
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    
    wifi_setup();
    
    gpio_setup();
    {
        
    }
    
    // Now set up tasks to run independently.
    xTaskCreatePinnedToCore (SignalGeneratorTask,
                             "SignalGeneratorTask",  // A name just for humans
                             2048,  // This stack size can be checked & adjusted by reading the Stack Highwater
                             NULL,
                             10, // ISR has highest priority.
                             &taskSignalGenerator,
                             CORE0);
    
    // Now set up tasks to run independently.
    xTaskCreatePinnedToCore (SignalReceiverTask,
                             "SignalReceiverTask",  // A name just for humans
                             2048,  // This stack size can be checked & adjusted by reading the Stack Highwater
                             NULL,
                             10, // ISR has highest priority.
                             &taskSignalReceiver,
                             CORE0);
    
    xTaskCreatePinnedToCore (DataCompilationTask,
                            "DataCompilationTask",
                            2048,
                            NULL,
                            1,
                            &taskDataCompilation,
                            CORE1);
    
    // Data Transmission executes on Core 0
    xTaskCreatePinnedToCore (DataTransmissionTask,
                            "DataTransmissionTask",
                            16384,
                            NULL,
                            3,
                            &taskDataTransmission,
                            CORE0);

}

/*--------------------------------------------------*/
/*---------------------- Tasks ---------------------*/
/*--------------------------------------------------*/
#define PAUSE_COUNT 1000

void SignalGeneratorTask(void *pvParameters)
{
    (void) pvParameters;
    
    uint32_t dataValue = 0;
    uint32_t inData;
    int pauseCt = 0;
    
    vTaskDelay(1);  // Ensure all tassks are up and running before we commence
    while(1)
    {
        dataValue++;
        pauseCt++;
        gpio_set_level(GPIO_OUTPUT_IO_0, dataValue % 2);
        gpio_set_level(GPIO_OUTPUT_IO_1, dataValue % 2);
        xTaskNotifyWait(0, 0, &inData, 10000);
        if (pauseCt == PAUSE_COUNT) // Need to relinquish CPU every now and then
        {
            vTaskDelay(1);
            pauseCt = 0;
        }
    }
    
}
void SignalReceiverTask(void *pvParameters)
{
    (void) pvParameters;
    
    int32_t signalCt = 0;
    uint32_t io_num;
    int pauseCt = 0;
    
    vTaskDelay(1);  // Ensure all tassks are up and running before we commence
    while(1)
    {
        pauseCt++;
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY))
        {
            signalCt++;
            xTaskNotify(taskDataCompilation, signalCt, eSetValueWithOverwrite);
            xTaskNotify(taskSignalGenerator, signalCt, eSetValueWithOverwrite);
        }
        if (pauseCt == PAUSE_COUNT) // Need to relinquish CPU every now and then
        {
            vTaskDelay(1);
            pauseCt = 0;
        }
    }
    
}
void DataCompilationTask(void *pvParameters)
{
    (void) pvParameters;
    
    int buff = 0;
    int prevBuff = 1;
    int pauseCt = 0;
    int count = 0;
    uint32_t inData;
    
    vTaskDelay(1); // Ensure all tassks are up and running before we commence
    while(1)
    {
        xTaskNotifyWait(0, 0, &inData, 1000);
        buffers[buff][count++] = inData;
        
        if (count >= BUFFER_SIZE)
        {
            pauseCt++;
            count = 0;
            prevBuff = buff;
            buff = 1 - buff;
            xTaskNotify(taskDataTransmission, prevBuff, eSetValueWithOverwrite);
            if (pauseCt == PAUSE_COUNT) // Need to relinquish CPU every now and then
            {
                vTaskDelay(1);
                pauseCt = 0;
            }
        }
        xTaskNotify(taskSignalReceiver, inData, eSetValueWithOverwrite);
    }
}

void DataTransmissionTask(void *pvParameters)
{
    uint32_t inData;
    int iascii;
    char txData[ASCII_BUFFER_SIZE];
    
    (void) pvParameters;
    
    while(1)
    {
        xTaskNotifyWait(0, 0, &inData, 1000);
        //printf("Received buffer: \n");
        if (wifiConnected && telnetClientConnected)
        {
            // printf("Sending data to client from buffer: %d. First number: %d \n", inData, buffers[inData][0]);
            iascii = 0;
            for (int i = 0; i<BUFFER_SIZE; i++)
            {
                int val = buffers[inData][i];
                for (int j=0; j<4; j++)
                {
                    txData[iascii++] = val % 85 + 33;
                    val = val / 85;
                }
                txData[iascii++] = val + 33;
            }

            int sent = send(telnet_socket, txData, iascii, 0);
            if (sent < 0)
            {
                ESP_LOGE(TAG, "Error occurred sending data. Error No: %d", sent);
            }
        }
    }
}
    

void SocketListenConnectTask (void *pvParamters)
{
    char addr_str[128];
    int addr_family;
    int ip_protocol;
    
    struct sockaddr_in dest_addr;
    
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);
    addr_family = AF_INET;
    ip_protocol = IPPROTO_TCP;
    inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str)-1);
    
    listen_socket = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_socket < 0)
    {
        ESP_LOGE(TAG, "Unable to create socket: Error No: %d", errno);
        vTaskDelete(NULL);
    }
    
    // Mark socket as non blocking.
    int status = fcntl(listen_socket, F_SETFL, fcntl(listen_socket, F_GETFL, 0) | O_NONBLOCK);
    if (status == -1)
    {
        ESP_LOGE(TAG, "Error setting non-blocking status: %s", strerror(errno));
        vTaskDelete(NULL);
    }
    
    int err = bind(listen_socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0)
    {
        ESP_LOGE(TAG, "Socket unable to bind: Error No: %s", strerror(errno));
        close(listen_socket);
        vTaskDelete(NULL);
    }
    err = listen(listen_socket, 1);
    if (err != 0)
    {
        ESP_LOGE(TAG, "Socket unable to listen: Error No: %s", strerror(errno));
        close(listen_socket);
        vTaskDelete(NULL);
    }
    else
    {
        bool client_connected = false;
        
        // Listening for Client connection.
        while (!client_connected)
        {
            struct sockaddr source_addr;
            uint addr_len = sizeof(source_addr);
            telnet_socket = accept(listen_socket, (struct sockaddr *)&source_addr, &addr_len);
            if (telnet_socket == -1 && errno == EWOULDBLOCK)
            {
                ESP_LOGI(TAG, "Awaiting connection");
                vTaskDelay(100);
            }
            else if (telnet_socket < 0)
            {
                ESP_LOGE(TAG, "Unable to accept connection %s", strerror(errno));
                vTaskDelay(100);
            }
            else
            {
                ESP_LOGI(TAG, "Connected");
                client_connected = true;
            }
        }
        telnetClientConnected = true;
    }
    vTaskDelete(NULL);
}
