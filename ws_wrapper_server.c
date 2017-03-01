#include "ws_wrapper_server.h"

static const char *TAG = "ws_wrapper_server";

static void websocket_listen(void *pvParameters);
static void websocket_manage(void *pvParameters);
static void websocket_select(void *pvParameters);
int safeSend(int clientSocket, const uint8_t *buffer, size_t bufferSize);

int sockets[MAX_SOCKETS] = { 0 };
int sockets_ready[MAX_SOCKETS] = { 0 };

void websocket_init(int port, void *onRecv)
{
    int *pport = (int*)malloc(sizeof(int));
    *pport = port;

    xTaskCreate(&websocket_listen, "websocket_listen", 4096, (void*)pport, 5, NULL);
    xTaskCreate(&websocket_select, "websocket_select", 4096, NULL, 5, NULL);
    xTaskCreate(&websocket_manage, "websocket_manage", 4096 * MAX_SOCKETS, onRecv, 5, NULL);
}

static void websocket_listen(void *pvParameters)
{
    int port = *((int*)pvParameters);
    free(pvParameters);

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
        
        int i = 0;
        for(i = 0; i < MAX_SOCKETS; i++)
        {
            if(sockets[i] == 0)
            {
                sockets[i] = clientSocket;
                break;
            }
        }
        if(i == MAX_SOCKETS)
        {
            ESP_LOGI(TAG, "Rejected connection from %s, too many connections!", inet_ntoa(remote.sin_addr));
        }
    }

    close(listenSocket);

    vTaskDelete(NULL);
}

static void websocket_select(void *pvParameters)
{
    while(1)
    {
        vTaskDelay(10 / portTICK_PERIOD_MS);

        fd_set rdfs;
        struct timeval tv;
        int ndfs = 0;

        FD_ZERO(&rdfs);

        for(int i = 0; i < MAX_SOCKETS; i++)
        {
            if(sockets[i] != 0)
            {
                FD_SET(sockets[i], &rdfs);
                if(sockets[i] > ndfs) 
                {
                    ndfs = sockets[i]; // ndfs takes the highest-numbered fd, and adds one
                }
            }
        }

        tv.tv_usec = 50;

        int retval = select(ndfs + 1, &rdfs, NULL, NULL, &tv);

        if(retval == -1)
        {
            ESP_LOGE(TAG, "select failed!");
            break;
        }
        else if(retval)
        {
            for(int i = 0; i < MAX_SOCKETS; i++)
            {
                if(FD_ISSET(sockets[i], &rdfs))
                {
                    sockets_ready[i] = 1;
                }
            }
        }
    }

    vTaskDelete(NULL);
}

static void websocket_manage(void *pvParameters)
{
    int clientSocket;
    void (*onRecv)() = pvParameters;

    uint8_t gBuffer[BUF_LEN];
    char* resource = NULL;

    memset(gBuffer, 0, BUF_LEN);
    size_t readedLength[MAX_SOCKETS] = { 0 };
    size_t frameSize = BUF_LEN;
    enum wsState state[MAX_SOCKETS] = { WS_STATE_OPENING };
    uint8_t *data = NULL;
    size_t dataSize = 0;
    enum wsFrameType frameType[MAX_SOCKETS] = { WS_INCOMPLETE_FRAME };
    struct handshake hs;
    nullHandshake(&hs);
    
    #define prepareBuffer frameSize = BUF_LEN; memset(gBuffer, 0, BUF_LEN);
    #define initNewFrame frameType[i] = WS_INCOMPLETE_FRAME; readedLength[i] = 0; memset(gBuffer, 0, BUF_LEN);
    #define closeSocket sockets[i] = 0; close(clientSocket); state[i] = WS_STATE_OPENING; initNewFrame;
    
    while (1) {
        vTaskDelay(10 / portTICK_PERIOD_MS);

        for(int i = 0; i < MAX_SOCKETS; i++)
        {
            if(sockets[i] != 0 && sockets_ready[i])
            {
                sockets_ready[i] = 0;
                clientSocket = sockets[i];

                ssize_t readed = recv(clientSocket, gBuffer+readedLength[i], BUF_LEN-readedLength[i], 0);
                if (!readed) {
                    ESP_LOGE(TAG, "recv failed");
                    closeSocket;
                }
                #ifdef PACKET_DUMP
                ESP_LOGI(TAG, "in packet:\n%s", gBuffer);
                #endif
                readedLength[i] += readed;
                assert(readedLength[i] <= BUF_LEN);
                
                if (state[i] == WS_STATE_OPENING) {
                    frameType[i] = wsParseHandshake(gBuffer, readedLength[i], &hs);
                } else {
                    frameType[i] = wsParseInputFrame(gBuffer, readedLength[i], &data, &dataSize);
                }
                
                if ((frameType[i] == WS_INCOMPLETE_FRAME && readedLength[i] == BUF_LEN) || frameType[i] == WS_ERROR_FRAME) {
                    if (frameType[i] == WS_INCOMPLETE_FRAME) {
                        ESP_LOGE(TAG, "buffer too small");
                    }
                    else {
                        ESP_LOGE(TAG, "error in incoming frame\n");
                    }
                    
                    if (state[i] == WS_STATE_OPENING) {
                        prepareBuffer;
                        frameSize = sprintf((char *)gBuffer,
                                            "HTTP/1.1 400 Bad Request\r\n"
                                            "%s%s\r\n\r\n",
                                            versionField,
                                            version);
                        safeSend(clientSocket, gBuffer, frameSize);
                        closeSocket;
                    } else {
                        prepareBuffer;
                        wsMakeFrame(NULL, 0, gBuffer, &frameSize, WS_CLOSING_FRAME);
                        if (safeSend(clientSocket, gBuffer, frameSize) == EXIT_FAILURE)
                            break;
                        state[i] = WS_STATE_CLOSING;
                        initNewFrame;
                    }
                }
                
                if (state[i] == WS_STATE_OPENING) {
                    assert(frameType[i] == WS_OPENING_FRAME);
                    if (frameType[i] == WS_OPENING_FRAME) {
                        // if resource is right, generate answer handshake and send it
                        int ret = 0;
                        onRecv(clientSocket, hs.resource, NULL, 0, &ret);
                        if (ret == EXIT_FAILURE) {
                            frameSize = sprintf((char *)gBuffer, "HTTP/1.1 404 Not Found\r\n\r\n");
                            safeSend(clientSocket, gBuffer, frameSize);
                            closeSocket;
                        }
                        
                        if (resource != NULL)
                            free(resource);
                        resource = malloc(strlen(hs.resource) + 1);
                        memcpy(resource, hs.resource, strlen(hs.resource) + 1);

                        prepareBuffer;
                        wsGetHandshakeAnswer(&hs, gBuffer, &frameSize);
                        freeHandshake(&hs);
                        if (safeSend(clientSocket, gBuffer, frameSize) == EXIT_FAILURE)
                        {
                            closeSocket;
                        }
                        state[i] = WS_STATE_NORMAL;
                        initNewFrame;
                    }
                } else {
                    if (frameType[i] == WS_CLOSING_FRAME) {
                        if (state[i] == WS_STATE_CLOSING) {
                            closeSocket;
                        } else {
                            prepareBuffer;
                            wsMakeFrame(NULL, 0, gBuffer, &frameSize, WS_CLOSING_FRAME);
                            safeSend(clientSocket, gBuffer, frameSize);
                            closeSocket;
                        }
                    } else if (frameType[i] == WS_TEXT_FRAME) {
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
            }
        }
    } // read/write cycle

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