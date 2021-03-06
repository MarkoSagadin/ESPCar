#include <stdio.h>
#include <string.h>
#include <sys/fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "nvs_flash.h"


#define AP_SSID "ESP_32"
#define AP_PASSPHARSE "12345678"            


// Used for wifi event handling
static EventGroupHandle_t wifi_event_group;
const int CLIENT_CONNECTED_BIT = BIT0;
const int CLIENT_DISCONNECTED_BIT = BIT1;
const int AP_STARTED_BIT = BIT2;

// Used for log ouput
static const char *TAG="ap_mode_tcp_server";

#define WHITE_BLINK (18)


/* Function is looking for specific wifi related events, such as 
connection of station, start of Ap point etc. If event happens 
it sets appropriate bit in wifi_event_group, thus enabling execution 
of code that was dependend on event. */
static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id)
    {
        case SYSTEM_EVENT_AP_START:
            printf("Event:ESP32 is started in AP mode\n");
            xEventGroupSetBits(wifi_event_group, AP_STARTED_BIT);
            break;
            
        case SYSTEM_EVENT_AP_STACONNECTED:
            xEventGroupSetBits(wifi_event_group, CLIENT_CONNECTED_BIT);
            break;

        case SYSTEM_EVENT_AP_STADISCONNECTED:
            xEventGroupSetBits(wifi_event_group, CLIENT_DISCONNECTED_BIT);
            break;		

        default:
            break;
    }
    return ESP_OK;
}

/* Function assings static IP to ESP32 and starts up DHCP server*/
static void start_dhcp_server()
{
    // initialize the tcp stack
    tcpip_adapter_init();

    // stop DHCP server
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP));
        
    // assign a static IP to the network interface
    tcpip_adapter_ip_info_t info;
    memset(&info, 0, sizeof(info));
    IP4_ADDR(&info.ip, 192, 168, 1, 1);
    IP4_ADDR(&info.gw, 192, 168, 1, 1);
    IP4_ADDR(&info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &info));
        
    // start the DHCP server   
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP));
    printf("DHCP server started \n");    
}

/* Creates an Access Point*/
static void initialise_wifi_in_ap(void)
{
     // disable wifi driver logging
    esp_log_level_set("wifi", ESP_LOG_NONE);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_AP) );

    // configure the wifi connection and start the interface
	wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .password = AP_PASSPHARSE,
            .channel = 0,
			.authmode = WIFI_AUTH_WPA2_PSK,
			.ssid_hidden = 0,
			.max_connection = 4,
			.beacon_interval = 100,		
        },
    };
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK( esp_wifi_start() );
    printf("ESP WiFi started in AP mode \n");
}

/* Function starts up a TCP connection: it creates a socket, 
binds it and listens for new connections. After it accepts 
the new connection, it will turn on and off an led, which is 
used for testing purposes.*/
static void tcp_server_task(void *pvParameters)
{
    char rx_buffer[128];
    char addr_str[128];
    int addr_family;
    int ip_protocol;

    static struct sockaddr_in sourceAddr; 
    static uint addrLen = sizeof(sourceAddr);

	struct sockaddr_in destAddr;
        destAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        destAddr.sin_family = AF_INET;
        destAddr.sin_port = htons(3000);    // Configures socket for port 3000
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
    
    // Converts IP address into string
    inet_ntoa_r(destAddr.sin_addr, addr_str, sizeof(addr_str) - 1);

    // Waits for AP to start
	xEventGroupWaitBits(wifi_event_group,AP_STARTED_BIT,false,true,portMAX_DELAY);

    // With this while loop we make it possible to connect again 
    // in case we close the socket after transmission. 
    while (1) {

        // Creates socket
        int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
        if (listen_sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket created");

        // Binds it to port, address family, etc.
        int err = bind(listen_sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket binded");

        // Listen alllows socket to have only one connection
        err = listen(listen_sock, 1);
        if (err != 0) {
            ESP_LOGE(TAG, "Error occured during listen: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket listening");

        // Accept is a locking statement, we need to connect 
        // with client before we can continue with execution of code 
        int sock = accept(listen_sock, (struct sockaddr *)&sourceAddr, &addrLen);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket accepted");
        
        // Client accepted now we receive data
        while (1) 
        {
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            // Error occured during receiving, break and close socket
            if (len < 0) {
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                break;
            }
            // Connection closed, break and close socket
            else if (len == 0)
            {
                ESP_LOGI(TAG, "Connection closed");
                break;
            }
            // Data received
            else 
            {
                rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
                ESP_LOGI(TAG, "Received %d bytes from %s:", len, addr_str);
                ESP_LOGI(TAG, "%s", rx_buffer);

                // For demonstration purposes we switch on or off led 
                if(atoi(rx_buffer) == 1)
                {
                    gpio_set_level(WHITE_BLINK, 1);
                    write(sock, "Led turned on", strlen("Led turned on"));
                }
                else
                {
                    gpio_set_level(WHITE_BLINK, 0);
                    write(sock, "Led turned off", strlen("Led turned 0ff"));
                }
            }
        }

        // Something went wrong, close socket and restart it 
        if (sock != -1) {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL);
}

/* Print the list of connected stations */
void printStationList() 
{
	printf(" Connected stations:\n");
	printf("--------------------------------------------------\n");
	
	wifi_sta_list_t wifi_sta_list;
	tcpip_adapter_sta_list_t adapter_sta_list;
   
	memset(&wifi_sta_list, 0, sizeof(wifi_sta_list));
	memset(&adapter_sta_list, 0, sizeof(adapter_sta_list));
   
	ESP_ERROR_CHECK(esp_wifi_ap_get_sta_list(&wifi_sta_list));	
	ESP_ERROR_CHECK(tcpip_adapter_get_sta_list(&wifi_sta_list, &adapter_sta_list));
	
	for(int i = 0; i < adapter_sta_list.num; i++) {
		
		tcpip_adapter_sta_info_t station = adapter_sta_list.sta[i];
         printf("%d - mac: %.2x:%.2x:%.2x:%.2x:%.2x:%.2x - IP: %s\n", i + 1,
				station.mac[0], station.mac[1], station.mac[2],
				station.mac[3], station.mac[4], station.mac[5],
				ip4addr_ntoa(&(station.ip)));
	}
	
	printf("\n");
}

/* Displays information about newly connected station.*/
void print_sta_info(void *pvParam)
{
    printf("print_sta_info task started \n");
    while(1) 
    {	
		EventBits_t staBits = xEventGroupWaitBits(wifi_event_group, CLIENT_CONNECTED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
		if((staBits & CLIENT_CONNECTED_BIT) != 0) printf("New station connected\n\n");
        else printf("A station disconnected\n\n");
        printStationList();
	}
}

/* Simple turn on, turn off function to show 
that TCP is not blocking execution of other tasks. */
void white_task(void *pvParameter)
{
    /*White Led setup*/
    gpio_pad_select_gpio(WHITE_BLINK);
    gpio_set_direction(WHITE_BLINK, GPIO_MODE_OUTPUT);

    
    
    while(1) {
         /* Blink off (output low) */
        gpio_set_level(WHITE_BLINK, 1);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        /* Blink on (output high) */
        gpio_set_level(WHITE_BLINK, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS);   
    }
}

void app_main()
{	
	// Initialize non-volatile flash space
	nvs_flash_init();

    // Start up the event handler 
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_event_group = xEventGroupCreate();

    // Setup IP of ESP32 and start DHCP 
    start_dhcp_server();

    // Set ESP32 as AP
    initialise_wifi_in_ap();

    // LED pin setup
    gpio_pad_select_gpio(WHITE_BLINK);
    gpio_set_direction(WHITE_BLINK, GPIO_MODE_OUTPUT);

    xTaskCreate(&tcp_server_task,"tcp_server",4096,NULL,5,NULL);
    xTaskCreate(&print_sta_info,"print_sta_info",4096,NULL,5,NULL);
    xTaskCreate(&white_task, "white_task", 2048, NULL, 5, NULL);
}
