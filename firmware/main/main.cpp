#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <nvs_flash.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>

#include "modbus.h"
#include "sdm120.h"
#include "status_led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_ENABLE_STATUS_DISPLAY
#include "status_display.h"
#endif
#include "electrical_power_measurement_delegate.h"

static chip::app::Clusters::ElectricalPowerMeasurement::ElectricalPowerMeasurementDelegate EPMDelegate;

static const char *TAG = "Main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::cluster;
using namespace esp_matter::endpoint;

using namespace chip::app::Clusters;

static uint16_t electrical_sensor_endpoint_id = 0;

// RS-485 bring-up tests. 0 = off (normal SDM120M polling), 1 = transmit one
// byte repeatedly, 2 = receive and log bytes, 3 = transmit a byte and listen
// for it (jumper TXD to RXD to bypass the transceiver).
#define MODBUS_LINK_TEST 0

#define ABORT_APP_ON_FAILURE(x, ...)               \
    do                                             \
    {                                              \
        if (!(unlikely(x)))                        \
        {                                          \
            __VA_ARGS__;                           \
            vTaskDelay(5000 / portTICK_PERIOD_MS); \
            abort();                               \
        }                                          \
    } while (0)

static bool is_commissioned()
{
    return chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
}

static void open_commissioning_window_if_necessary()
{
    VerifyOrReturn(chip::Server::GetInstance().GetFabricTable().FabricCount() == 0);

    chip::CommissioningWindowManager &mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    VerifyOrReturn(mgr.IsCommissioningWindowOpen() == false);

    CHIP_ERROR err = mgr.OpenBasicCommissioningWindow(
        chip::System::Clock::Seconds16(300),
        chip::CommissioningWindowAdvertisement::kDnssdOnly);

    if (err != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "Failed to open commissioning window: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type)
    {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        status_led_set_state(STATUS_LED_OPERATIONAL);
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        status_led_set_state(STATUS_LED_PAIRING);
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        // Also fires when a pairing attempt is abandoned or fails, in which
        // case the window is still open and the device is pairable again.
        ESP_LOGI(TAG, "Commissioning session stopped");
        status_led_set_state(is_commissioned() ? STATUS_LED_OPERATIONAL : STATUS_LED_READY_TO_PAIR);
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        status_led_set_state(is_commissioned() ? STATUS_LED_OPERATIONAL : STATUS_LED_OFF);
#if CONFIG_ENABLE_STATUS_DISPLAY
        StatusDisplayMgr().ClearCommissioningCode();
#endif
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
    {
        ESP_LOGI(TAG, "Commissioning window opened");

        status_led_set_state(STATUS_LED_READY_TO_PAIR);

        chip::RendezvousInformationFlags rendezvousFlags = chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE);

        chip::PayloadContents payload;
        GetPayloadContents(payload, rendezvousFlags);

        char payloadBuffer[chip::QRCodeBasicSetupPayloadGenerator::kMaxQRCodeBase38RepresentationLength + 1];
        chip::MutableCharSpan qrCode(payloadBuffer);

        if (GetQRCode(qrCode, payload) == CHIP_NO_ERROR)
        {
            ESP_LOGI(TAG, "Generated QR CODE [%d]: %s", qrCode.size(), qrCode.data());
#if CONFIG_ENABLE_STATUS_DISPLAY
            StatusDisplayMgr().SetCommissioningCode(qrCode.data(), qrCode.size());
#endif
        }
        else
        {
            ESP_LOGE(TAG, "Failed to generate the commissioning QR code");
        }
        break;
    }
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Fabric removed");
        open_commissioning_window_if_necessary();
        break;
    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized");
        break;
    default:
        break;
    }
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    return ESP_OK;
}

extern "C" void matter_update_voltage(float voltage_v)
{
    chip::DeviceLayer::SystemLayer().ScheduleLambda([voltage_v] {
        EPMDelegate.SetVoltage(chip::app::DataModel::MakeNullable((int64_t)(voltage_v * 1000.0f))); // V → mV
    });
}

extern "C" void matter_update_current(float current_a)
{
    chip::DeviceLayer::SystemLayer().ScheduleLambda([current_a] {
        EPMDelegate.SetActiveCurrent(chip::app::DataModel::MakeNullable((int64_t)(current_a * 1000.0f))); // A → mA
    });
}

extern "C" void matter_update_power(float power_w)
{
    chip::DeviceLayer::SystemLayer().ScheduleLambda([power_w] {
        EPMDelegate.SetActivePower(chip::app::DataModel::MakeNullable((int64_t)(power_w * 1000.0f))); // W → mW
    });
}

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, 
                                         uint16_t endpoint_id,
                                         uint32_t cluster_id, 
                                         uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, 
                                         void *priv_data)
{
    return ESP_OK;
}

extern "C" void app_main()
{
    nvs_flash_init();

    status_led_init();

#if CONFIG_ENABLE_STATUS_DISPLAY
    StatusDisplayMgr().Init();
    StatusDisplayMgr().TurnOn();
    ESP_LOGI(TAG, "Display initialized");
#endif

    modbus_uart_init();
    ESP_LOGI(TAG, "Modbus UART initialized");

    // Create the root endpoint
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    electrical_sensor::config_t electrical_sensor_config;
    // Configure the sensor as a node.
    electrical_sensor_config.power_topology.feature_flags = power_topology::feature::node_topology::get_id();

    // Configure AC as the power type.
    electrical_sensor_config.electrical_power_measurement.feature_flags = electrical_power_measurement::feature::alternating_current::get_id();
    electrical_sensor_config.electrical_power_measurement.delegate = &EPMDelegate;

    endpoint_t *endpoint = electrical_sensor::create(node, &electrical_sensor_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create electrical sensor endpoint"));
    electrical_sensor_endpoint_id = endpoint::get_id(endpoint);

    cluster_t *cluster = cluster::get(endpoint, chip::app::Clusters::ElectricalPowerMeasurement::Id);
    ABORT_APP_ON_FAILURE(cluster != nullptr, ESP_LOGE(TAG, "Failed to get EPM cluster from endpoint"));

    electrical_power_measurement::attribute::create_voltage(cluster, 0);
    electrical_power_measurement::attribute::create_active_current(cluster, 0);
    electrical_power_measurement::attribute::create_active_power(cluster, 0);

    esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

#if MODBUS_LINK_TEST == 1
    modbus_start_tx_test();
#elif MODBUS_LINK_TEST == 2
    modbus_start_rx_test();
#elif MODBUS_LINK_TEST == 3
    modbus_start_loopback_test();
#else
    xTaskCreate(sdm120_read_task, "sdm120m_read", 4096, NULL, 5, NULL);
#endif
}
