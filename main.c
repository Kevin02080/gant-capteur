#include <driver/i2c.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include <math.h>


#include "sdkconfig.h"


/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define SSID "Routeur Main Robotisee"
#define PASSWORD "12345678"

static int s_retry_num = 0; //nombre d'essai de reconnexion au reseau WIFI

esp_mqtt_client_handle_t mqtt_client;

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD("MQTT", "Event dispatched from event loop base=%s, event_id=%d", base, event_id);

    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI("MQTT", "Connecté au brokeur\n");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI("MQTT", "Déconnecté du brokeur\n");
        break;
/* Mis en commentaire car provoque un crash
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI("MQTT", "Souscription au topic %s, msg_id=%d\n", event->topic, event->msg_id);
        break;*/
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI("MQTT", "Annulation souscription au topic %s, msg_id=%d\n", event->topic, event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI("MQTT", "Publcation sur le topic %s, msg_id=%d\n", event->topic, event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI("MQTT", "Reception de donnees\n");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI("MQTT", "Erreur\n");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGI("MQTT", "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI("MQTT", "Other event id:%d", event->event_id);
        break;
    }
}

//fonction initialisation du client MQTT
void MQTT_Init()
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtt://192.168.1.100",//adresse du  routeur
        .event_handle = NULL,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);// config du  client
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);// enregistrement fonction event
    esp_mqtt_client_start(mqtt_client);// lancement du client
}

//fonction gérant les évènements WIFI
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        ESP_LOGI("Wifi", "Wifi connecté.");
    
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 3) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI("Wifi", "Wifi deconnecté. Tentative de reconnection");
         
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI("Wifi","connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI("Wifi", "IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

//fonction pour se connecter au wifi
void wifi_init()
{
    // Initialisation
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();

    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // enregistrement des évènements
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT,
                                                    ESP_EVENT_ANY_ID,
                                                    &event_handler,
                                                    NULL,
                                                    &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT,
                                                    IP_EVENT_STA_GOT_IP,
                                                    &event_handler,
                                                    NULL,
                                                     &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = SSID,
            .password = PASSWORD,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI("Wifi", "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI("wifi", "connected to ap SSID:%s password:%s",
                SSID, PASSWORD);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI("Wifi", "Failed to connect to SSID:%s, password:%s",
                SSID, PASSWORD);
    } else {
        ESP_LOGE("Wifi", "UNEXPECTED EVENT");
    }
}

//initialisation de la memoire flash
void NVS_Init()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }

}



void app_main()
{
    
   
    NVS_Init();
    wifi_init();

    vTaskDelay(1000 / portTICK_PERIOD_MS);//Attent que le esp32 se connecte au routeur wifi
    
    MQTT_Init();

    vTaskDelay(1000 / portTICK_PERIOD_MS);//Attent que le client se connecte au brokeur

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_11); // pouces
    adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_DB_11); // index
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11); // majeur
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11); // annulaire
    adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_DB_11); // auriculaire



    while(1)
    { 
        int val1 = adc1_get_raw(ADC1_CHANNEL_3);
        int val2 = adc1_get_raw(ADC1_CHANNEL_7);
        int val3 = adc1_get_raw(ADC1_CHANNEL_0);
        int val4 = adc1_get_raw(ADC1_CHANNEL_6);
        int val5 = adc1_get_raw(ADC1_CHANNEL_4);

        int v1 = (int)(0.1 * val1 - 160);
        int v2 = (int)(0.1 * val2 - 160);
        int v3 = (int)(0.1 * val3 - 160);
        int v4 = (int)(0.1 * val4 - 160);
        int v5 = (int)(0.1 * val5 - 160);

        char v1_s[10];
        char v2_s[10];
        char v3_s[10];
        char v4_s[10];
        char v5_s[10];

        sprintf(v1_s, "%d\n", v1);
        sprintf(v2_s, "%d\n", v2);
        sprintf(v3_s, "%d\n", v3);
        sprintf(v4_s, "%d\n", v4);
        sprintf(v5_s, "%d\n", v5);

        ESP_LOGI("Valeurs", "val1 = %s, val2 = %s, val3 = %s, val4 = %s, val5 = %s", v1_s, v2_s, v3_s, v4_s, v5_s);

        esp_mqtt_client_publish(mqtt_client, "Doigt0", v5_s, strlen(v5_s), 0, 0);
        esp_mqtt_client_publish(mqtt_client, "Doigt1", v4_s, strlen(v4_s), 0, 0);
        esp_mqtt_client_publish(mqtt_client, "Doigt2", v3_s, strlen(v3_s), 0, 0);
        esp_mqtt_client_publish(mqtt_client, "Doigt3", v2_s, strlen(v2_s), 0, 0);
        esp_mqtt_client_publish(mqtt_client, "Doigt4", v1_s, strlen(v1_s), 0, 0);

        vTaskDelay(100 / portTICK_PERIOD_MS);

    }


}