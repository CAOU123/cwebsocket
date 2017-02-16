#include "ws_wrapper.h"

static const char *TAG = "ws_wrapper";

struct listenParams
{
    int port;
    void (*onRecv)();
};

struct remoteParams
{
    int clientSocket;
    void (*onRecv)();
};

static void websocket_listen(void *pvParameters);
static void websocket_manage(void *pvParameters);
int safeSend(int clientSocket, const uint8_t *buffer, size_t bufferSize);

void websocket_init(int port, void *onRecv)
{
    struct listenParams *params = malloc(sizeof(struct listenParams));
    params->port = port;
    params->onRecv = onRecv;

    xTaskCreate(&websocket_listen, "websocket_listen", 4096, (void*)params, 5, NULL);
}

static void websocket_listen(void *pvParameters)
{
    int port = ((struct listenParams*)pvParameters)->port;
    void (*onRecv)() = ((struct listenParams*)pvParameters)->onRecv;

    int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket == -1)
    {
        ESP_LOGE(TAG, "create socket FAILED");
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(port);
    if (bind(listenSocket, (struct sockaddr*)&local, sizeof(local)) == -1)
    {
        ESP_LOGE(TAG, "bind FAILED");
    }

    if(listen(listenSocket, 1) == -1)
    {
        ESP_LOGE(TAG, "listen FAILED");
    }
    ESP_LOGI(TAG, "opened %s:%d\n", inet_ntoa(local.sin_addr), ntohs(local.sin_port));

    while(1)
    {
        struct sockaddr_in remote;
        socklen_t sockaddrLen = sizeof(remote);
        int clientSocket = accept(listenSocket, (struct sockaddr*)&remote, &sockaddrLen);
        if (clientSocket == -1)
        {
            ESP_LOGE(TAG, "accept FAILED");
        }

        ESP_LOGI(TAG, "connected %s:%d\n", inet_ntoa(remote.sin_addr), ntohs(remote.sin_port));

        struct remoteParams *params = malloc(sizeof(struct remoteParams));
        params->clientSocket = clientSocket;
        params->onRecv = onRecv;

        xTaskCreate(&websocket_manage, "websocket_manage", 4096, (void*)params, 5, NULL);
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }

    close(listenSocket);

    vTaskDelete(NULL);
}

static void websocket_manage(void *pvParameters)
{
    int clientSocket = ((struct remoteParams*)pvParameters)->clientSocket;
    void (*onRecv)() = ((struct remoteParams*)pvParameters)->onRecv;

    uint8_t gBuffer[BUF_LEN];
    char* resource = NULL;

    memset(gBuffer, 0, BUF_LEN);
    size_t readedLength = 0;
    size_t frameSize = BUF_LEN;
    enum wsState state = WS_STATE_OPENING;
    uint8_t *data = NULL;
    size_t dataSize = 0;
    enum wsFrameType frameType = WS_INCOMPLETE_FRAME;
    struct handshake hs;
    nullHandshake(&hs);
    
    #define prepareBuffer frameSize = BUF_LEN; memset(gBuffer, 0, BUF_LEN);
    #define initNewFrame frameType = WS_INCOMPLETE_FRAME; readedLength = 0; memset(gBuffer, 0, BUF_LEN);
    
    while (frameType == WS_INCOMPLETE_FRAME) {
        ssize_t readed = recv(clientSocket, gBuffer+readedLength, BUF_LEN-readedLength, 0);
        if (!readed) {
            close(clientSocket);
            ESP_LOGE(TAG, "recv failed");
            return;
        }
        #ifdef PACKET_DUMP
        ESP_LOGI(TAG, "in packet:\n%s", gBuffer);
        #endif
        readedLength+= readed;
        assert(readedLength <= BUF_LEN);
        
        if (state == WS_STATE_OPENING) {
            frameType = wsParseHandshake(gBuffer, readedLength, &hs);
        } else {
            frameType = wsParseInputFrame(gBuffer, readedLength, &data, &dataSize);
        }
        
        if ((frameType == WS_INCOMPLETE_FRAME && readedLength == BUF_LEN) || frameType == WS_ERROR_FRAME) {
            if (frameType == WS_INCOMPLETE_FRAME) {
                ESP_LOGE(TAG, "buffer too small");
            }
            else {
                ESP_LOGE(TAG, "error in incoming frame\n");
            }
            
            if (state == WS_STATE_OPENING) {
                prepareBuffer;
                frameSize = sprintf((char *)gBuffer,
                                    "HTTP/1.1 400 Bad Request\r\n"
                                    "%s%s\r\n\r\n",
                                    versionField,
                                    version);
                safeSend(clientSocket, gBuffer, frameSize);
                break;
            } else {
                prepareBuffer;
                wsMakeFrame(NULL, 0, gBuffer, &frameSize, WS_CLOSING_FRAME);
                if (safeSend(clientSocket, gBuffer, frameSize) == EXIT_FAILURE)
                    break;
                state = WS_STATE_CLOSING;
                initNewFrame;
            }
        }
        
        if (state == WS_STATE_OPENING) {
            assert(frameType == WS_OPENING_FRAME);
            if (frameType == WS_OPENING_FRAME) {
                // if resource is right, generate answer handshake and send it
                int ret = 0;
                onRecv(clientSocket, hs.resource, NULL, 0, &ret);
                if (ret == EXIT_FAILURE) {
                    frameSize = sprintf((char *)gBuffer, "HTTP/1.1 404 Not Found\r\n\r\n");
                    safeSend(clientSocket, gBuffer, frameSize);
                    break;
                }
                
                if (resource != NULL)
                    free(resource);
                resource = malloc(strlen(hs.resource) + 1);
                memcpy(resource, hs.resource, strlen(hs.resource) + 1);

                prepareBuffer;
                wsGetHandshakeAnswer(&hs, gBuffer, &frameSize);
                freeHandshake(&hs);
                if (safeSend(clientSocket, gBuffer, frameSize) == EXIT_FAILURE)
                    break;
                state = WS_STATE_NORMAL;
                initNewFrame;
            }
        } else {
            if (frameType == WS_CLOSING_FRAME) {
                if (state == WS_STATE_CLOSING) {
                    break;
                } else {
                    prepareBuffer;
                    wsMakeFrame(NULL, 0, gBuffer, &frameSize, WS_CLOSING_FRAME);
                    safeSend(clientSocket, gBuffer, frameSize);
                    break;
                }
            } else if (frameType == WS_TEXT_FRAME) {
                uint8_t *receivedString = NULL;
                receivedString = malloc(dataSize+1);
                assert(receivedString);
                memcpy(receivedString, data, dataSize);
                receivedString[ dataSize ] = 0;

                int ret = 0;
                onRecv(clientSocket, resource, receivedString, dataSize, &ret);
                
                initNewFrame;
            }
        }
    } // read/write cycle
    
    close(clientSocket);

    vTaskDelete(NULL);
}

int websocket_send(int clientSocket, const char *buffer, size_t bufferSize)
{
    uint8_t gBuffer[BUF_LEN];
    size_t frameSize = BUF_LEN;
    memset(gBuffer, 0, BUF_LEN);

    wsMakeFrame((uint8_t*)buffer, bufferSize, gBuffer, &frameSize, WS_TEXT_FRAME);
    if (safeSend(clientSocket, gBuffer, frameSize) == EXIT_FAILURE)
    {
        ESP_LOGE(TAG, "Send FAILED");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int safeSend(int clientSocket, const uint8_t *buffer, size_t bufferSize)
{
    #ifdef PACKET_DUMP
    ESP_LOGI(TAG, "out packet:\n%s", buffer);
    #endif

    ssize_t written = send(clientSocket, buffer, bufferSize, 0);
    if (written == -1) {
        close(clientSocket);
        ESP_LOGE(TAG, "send failed");
        return EXIT_FAILURE;
    }
    if (written != bufferSize) {
        close(clientSocket);
        ESP_LOGE(TAG, "written not all bytes");
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
}