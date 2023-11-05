#include <stdio.h>
#include <string.h>

#include <netdb.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#define WIFI_MAXIMUM_RETRY 8

#define WEB_SERVER "example.com"
#define WEB_PORT 80
#define WEB_URL "http://example.com/"

typedef enum WiFiEventGroupBits {
    WiFiEventGroupBits_Connected = BIT0,
    WiFiEventGroupBits_Fail = BIT1
} WiFiEventGroupBits_t;

static const char LOG_TAG[] = "MeteALOlogia station";

static EventGroupHandle_t wifi_event_group;

static int wifi_retry_num = 0;

static void event_handler(void* arg,
                          esp_event_base_t event_base,
                          int32_t event_id,
                          void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGI(LOG_TAG, "Failed to connect to Wi-Fi");
            if (wifi_retry_num < WIFI_MAXIMUM_RETRY) {
                esp_wifi_connect();
                wifi_retry_num++;
                ESP_LOGI(LOG_TAG, "Retrying...");
            } else {
                xEventGroupSetBits(wifi_event_group, WiFiEventGroupBits_Fail);
            }
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
            char ip_buffer[17];
            ESP_LOGI(LOG_TAG, "Got IP:%s",
                     ip4addr_ntoa_r(&event->ip_info.ip, ip_buffer,
                                    sizeof(ip_buffer) / sizeof(ip_buffer[0])));
            wifi_retry_num = 0;
            xEventGroupSetBits(wifi_event_group, WiFiEventGroupBits_Connected);
        }
    }
}

void wifi_init(void) {
    // Create event group, which will be used to notify that task when certain
    // Wi-Fi events will happen
    wifi_event_group = xEventGroupCreate();

    tcpip_adapter_init();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta =
            {
                .ssid = CONFIG_WIFI_SSID,
                .password = CONFIG_WIFI_PASSWORD,
            },
    };

    if (strlen((char*)wifi_config.sta.password)) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Waiting until either the connection is established (WIFI_CONNECTED_BIT)
    // or connection failed for the maximum number of re-tries (WIFI_FAIL_BIT).
    // The bits are set by event_handler() (see above)
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WiFiEventGroupBits_Connected | WiFiEventGroupBits_Fail, pdFALSE,
        pdFALSE, portMAX_DELAY);

    if (bits & WiFiEventGroupBits_Connected) {
        ESP_LOGI(LOG_TAG, "Connected to Wi-Fi");
    } else if (bits & WiFiEventGroupBits_Fail) {
        ESP_LOGI(LOG_TAG, "Failed to connect to Wi-Fi");
    } else {
        ESP_LOGE(LOG_TAG, "UNEXPECTED EVENT");
    }

    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                 &event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                 &event_handler));
    vEventGroupDelete(wifi_event_group);
}

void tcp_test(void) {
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo* res;
    struct in_addr* addr;
    int my_socket;

    // Get IP of desired host
    int err = getaddrinfo(WEB_SERVER, "80", &hints, &res);

    if (err != 0 || res == NULL) {
        ESP_LOGE(LOG_TAG, "DNS lookup failed! err=%d res=%p", err, res);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        return;
    }

    // Print IP of host
    addr = &((struct sockaddr_in*)res->ai_addr)->sin_addr;
    char ip_buffer[17];
    ESP_LOGI(LOG_TAG, "DNS lookup succeeded. IP: %s",
             inet_ntoa_r(*addr, ip_buffer,
                         sizeof(ip_buffer) / sizeof(ip_buffer[0])));

    // Create socket
    my_socket = socket(res->ai_family, res->ai_socktype, 0);
    if (my_socket < 0) {
        ESP_LOGE(LOG_TAG, "Failed to allocate socket");
        freeaddrinfo(res);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        return;
    }
    ESP_LOGI(LOG_TAG, "Allocated socket");

    // Try to connect
    if (connect(my_socket, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(LOG_TAG, "Failed to connect socket; errno=%d", errno);
        close(my_socket);
        freeaddrinfo(res);
        vTaskDelay(4000 / portTICK_PERIOD_MS);
        return;
    }

    ESP_LOGI(LOG_TAG, "Connected!");
    freeaddrinfo(res);

    // Send data
    static const char request[] =
        "GET " WEB_URL
        " HTTP/1.0\r\n"
        "Host: " WEB_SERVER
        "\r\n"
        "User-Agent: ESP8266/0.1 MeteALOlogia\r\n\r\n";
    if (write(my_socket, request, strlen(request)) < 0) {
        ESP_LOGE(LOG_TAG, "Socket send failed");
        close(my_socket);
        vTaskDelay(4000 / portTICK_PERIOD_MS);
        return;
    }
    ESP_LOGI(LOG_TAG, "Successfully sent data through socket");

    struct timeval receiving_timeout;
    receiving_timeout.tv_sec = 5;
    receiving_timeout.tv_usec = 0;
    if (setsockopt(my_socket, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
                   sizeof(receiving_timeout)) < 0) {
        ESP_LOGE(LOG_TAG, "Failed to set socket receiving timeout");
        close(my_socket);
        vTaskDelay(4000 / portTICK_PERIOD_MS);
        return;
    }
    ESP_LOGI(LOG_TAG, "Successfully set socket receiving timeout");

    // Receive response
    char recv_buf[64];
    char response_buffer[1024];
    char* response_buffer_ptr = response_buffer;
    uint16_t received_bytes;
    do {
        bzero(recv_buf, sizeof(recv_buf));
        received_bytes = read(my_socket, recv_buf, sizeof(recv_buf) - 1);
        for (int i = 0; i < received_bytes; i++) {
            *response_buffer_ptr = recv_buf[i];
            response_buffer_ptr++;
        }
    } while (received_bytes > 0);
    *response_buffer_ptr = 0;

    // Print response
    ESP_LOGI(LOG_TAG, "Received response:\n\"\"\"%s\"\"\"", response_buffer);

    // Close socket
    close(my_socket);
}

void app_main() {
    printf("MeteALOlogia station is starting...\n");

    /* Print chip information */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is ESP8266 chip with %d CPU cores, WiFi, ", chip_info.cores);

    printf("silicon revision %d, ", chip_info.revision);

    printf("%dMB %s flash\n", (int)(spi_flash_get_chip_size() / (1024 * 1024)),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded"
                                                         : "external");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init();

    tcp_test();
}
