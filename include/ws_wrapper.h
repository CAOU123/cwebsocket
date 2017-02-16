#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_wpa2.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"
#include "lwip/sockets.h"
#include "websocket.h"

#define BUF_LEN 1024 //max: 0xFFFF

void websocket_init(int port, void *onRecv);
int websocket_send(int clientSocket, const char *buffer, size_t bufferSize);