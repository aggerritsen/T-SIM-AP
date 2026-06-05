#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "config.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_ppp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

namespace {
constexpr const char *TAG = "T-SIM-AP";

constexpr gpio_num_t PMU_I2C_SDA = GPIO_NUM_15;
constexpr gpio_num_t PMU_I2C_SCL = GPIO_NUM_7;
constexpr uint8_t AXP2101_ADDR = 0x34;

constexpr uart_port_t MODEM_UART = UART_NUM_1;
constexpr gpio_num_t MODEM_RXD = GPIO_NUM_4;
constexpr gpio_num_t MODEM_TXD = GPIO_NUM_5;
constexpr gpio_num_t MODEM_PWR = GPIO_NUM_41;
constexpr int MODEM_BAUD = 115200;

constexpr int PPP_CONNECTED_BIT = BIT0;
constexpr int PPP_FAILED_BIT = BIT1;
constexpr int DNS_PROXY_PORT = 53;

constexpr uint8_t AXP2101_DC_ONOFF_DVM_CTRL = 0x80;
constexpr uint8_t AXP2101_DC_VOL2_CTRL = 0x84;
constexpr uint8_t AXP2101_LDO_ONOFF_CTRL0 = 0x90;
constexpr uint8_t AXP2101_LDO_VOL5_CTRL = 0x97;
constexpr uint8_t AXP2101_TS_PIN_CTRL = 0x50;

EventGroupHandle_t ppp_events;
esp_netif_t *ap_netif = nullptr;
esp_netif_t *ppp_netif = nullptr;
httpd_handle_t http_server = nullptr;

char ap_ssid[33] = {};
char sim_imsi[24] = "-";
char sim_iccid[32] = "-";
char selected_supplier[33] = "-";
char selected_apn[33] = "-";
char ppp_ip[16] = "-";
char ppp_dns[16] = "-";
char router_state[64] = "booting";
bool modem_ready = false;
bool modem_registered = false;
bool ppp_connected = false;
bool nat_enabled = false;
bool ppp_raw_mode = false;

void set_state(const char *state)
{
    snprintf(router_state, sizeof(router_state), "%s", state ? state : "-");
    ESP_LOGI(TAG, "STATE: %s", router_state);
}

esp_err_t i2c_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_write_to_device(I2C_NUM_0, AXP2101_ADDR, data, sizeof(data), pdMS_TO_TICKS(1000));
}

esp_err_t i2c_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(I2C_NUM_0, AXP2101_ADDR, &reg, 1, value, 1, pdMS_TO_TICKS(1000));
}

esp_err_t i2c_update_bits(uint8_t reg, uint8_t mask, bool set)
{
    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(i2c_read_reg(reg, &value), TAG, "i2c read 0x%02X", reg);
    value = set ? (value | mask) : (value & ~mask);
    return i2c_write_reg(reg, value);
}

esp_err_t init_i2c()
{
    i2c_config_t cfg = {};
    cfg.mode = I2C_MODE_MASTER;
    cfg.sda_io_num = PMU_I2C_SDA;
    cfg.scl_io_num = PMU_I2C_SCL;
    cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.master.clk_speed = 400000;
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_NUM_0, &cfg), TAG, "i2c config");
    return i2c_driver_install(I2C_NUM_0, cfg.mode, 0, 0, 0);
}

esp_err_t enable_modem_rails()
{
    set_state("powering modem rails");
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "i2c init");

    uint8_t probe = 0;
    ESP_RETURN_ON_ERROR(i2c_read_reg(0x00, &probe), TAG, "AXP2101 probe");
    ESP_LOGI(TAG, "PMU: AXP2101 probe reg0=0x%02X", probe);

    ESP_LOGI(TAG, "PMU: modem rails reset");
    ESP_RETURN_ON_ERROR(i2c_update_bits(AXP2101_LDO_ONOFF_CTRL0, BIT5, false), TAG, "disable BLDO2");
    ESP_RETURN_ON_ERROR(i2c_update_bits(AXP2101_DC_ONOFF_DVM_CTRL, BIT2, false), TAG, "disable DC3");
    vTaskDelay(pdMS_TO_TICKS(2000));

    uint8_t dc3_3000mv = 88 + ((3000 - 1600) / 100);
    uint8_t bldo2_3300mv = (3300 - 500) / 100;
    ESP_RETURN_ON_ERROR(i2c_write_reg(AXP2101_DC_VOL2_CTRL, dc3_3000mv), TAG, "set DC3");
    ESP_RETURN_ON_ERROR(i2c_update_bits(AXP2101_DC_ONOFF_DVM_CTRL, BIT2, true), TAG, "enable DC3");
    ESP_RETURN_ON_ERROR(i2c_write_reg(AXP2101_LDO_VOL5_CTRL, bldo2_3300mv), TAG, "set BLDO2");
    ESP_RETURN_ON_ERROR(i2c_update_bits(AXP2101_LDO_ONOFF_CTRL0, BIT5, true), TAG, "enable BLDO2");
    ESP_RETURN_ON_ERROR(i2c_write_reg(AXP2101_TS_PIN_CTRL, 0x00), TAG, "disable TS pin measure");
    vTaskDelay(pdMS_TO_TICKS(2500));
    return ESP_OK;
}

void pwrkey_pulse()
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << MODEM_PWR;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(MODEM_PWR, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(MODEM_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(MODEM_PWR, 0);
}

esp_err_t init_uart()
{
    uart_config_t cfg = {};
    cfg.baud_rate = MODEM_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_RETURN_ON_ERROR(uart_driver_install(MODEM_UART, 4096, 4096, 0, nullptr, 0), TAG, "uart install");
    ESP_RETURN_ON_ERROR(uart_param_config(MODEM_UART, &cfg), TAG, "uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(MODEM_UART, MODEM_TXD, MODEM_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart pins");
    ESP_LOGI(TAG, "MODEM: UART1 started baud=%d RX=%d TX=%d", MODEM_BAUD, MODEM_RXD, MODEM_TXD);
    return ESP_OK;
}

void uart_write_text(const char *text)
{
    uart_write_bytes(MODEM_UART, text, strlen(text));
}

void modem_uart_flush_input()
{
    uart_flush_input(MODEM_UART);
}

bool at_collect(const char *cmd, char *out, size_t out_len, uint32_t timeout_ms, bool expect_connect = false)
{
    if (out && out_len)
        out[0] = 0;

    modem_uart_flush_input();
    ESP_LOGI(TAG, "MODEM: AT%s", cmd);
    uart_write_text("AT");
    uart_write_text(cmd);
    uart_write_text("\r\n");

    std::string response;
    response.reserve(512);
    uint8_t byte = 0;
    int64_t start = esp_timer_get_time() / 1000;
    while ((esp_timer_get_time() / 1000) - start < timeout_ms) {
        int len = uart_read_bytes(MODEM_UART, &byte, 1, pdMS_TO_TICKS(20));
        if (len <= 0)
            continue;

        response.push_back((char)byte);
        if (response.find("\r\nOK\r\n") != std::string::npos) {
            if (out && out_len)
                snprintf(out, out_len, "%s", response.c_str());
            ESP_LOGI(TAG, "MODEM: AT%s OK", cmd);
            return true;
        }
        if (expect_connect && response.find("CONNECT") != std::string::npos) {
            if (out && out_len)
                snprintf(out, out_len, "%s", response.c_str());
            ESP_LOGI(TAG, "MODEM: AT%s CONNECT", cmd);
            return true;
        }
        if (response.find("\r\nERROR\r\n") != std::string::npos ||
            response.find("+CME ERROR") != std::string::npos ||
            response.find("NO CARRIER") != std::string::npos) {
            if (out && out_len)
                snprintf(out, out_len, "%s", response.c_str());
            ESP_LOGW(TAG, "MODEM: AT%s failed: %s", cmd, response.c_str());
            return false;
        }
    }

    if (out && out_len)
        snprintf(out, out_len, "%s", response.c_str());
    ESP_LOGW(TAG, "MODEM: AT%s timeout: %s", cmd, response.c_str());
    return false;
}

bool at_ok(const char *cmd, uint32_t timeout_ms)
{
    return at_collect(cmd, nullptr, 0, timeout_ms);
}

bool set_modem_data_baud()
{
    if (MODEM_CONFIG.data_baud == MODEM_BAUD)
        return true;

    char cmd[32] = {};
    snprintf(cmd, sizeof(cmd), "+IPR=%d", MODEM_CONFIG.data_baud);
    if (!at_ok(cmd, 3000)) {
        ESP_LOGW(TAG, "MODEM: baud switch command failed, staying at %d", MODEM_BAUD);
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_baudrate(MODEM_UART, MODEM_CONFIG.data_baud));
    vTaskDelay(pdMS_TO_TICKS(150));

    if (!at_ok("", 1000)) {
        ESP_LOGW(TAG, "MODEM: no AT response at %d, reverting to %d", MODEM_CONFIG.data_baud, MODEM_BAUD);
        ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_baudrate(MODEM_UART, MODEM_BAUD));
        at_ok("+IPR=115200", 3000);
        return false;
    }

    ESP_LOGI(TAG, "MODEM: UART baud switched to %d", MODEM_CONFIG.data_baud);
    return true;
}

bool recover_modem_baud()
{
    const int bauds[] = {MODEM_BAUD, MODEM_CONFIG.data_baud, 460800, 921600};
    for (int baud : bauds) {
        if (baud <= 0)
            continue;
        ESP_LOGI(TAG, "MODEM: probing baud %d", baud);
        ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_baudrate(MODEM_UART, baud));
        vTaskDelay(pdMS_TO_TICKS(150));
        if (!at_ok("", 800))
            continue;

        if (baud == MODEM_BAUD) {
            ESP_LOGI(TAG, "MODEM: responding at %d", MODEM_BAUD);
            return true;
        }

        ESP_LOGW(TAG, "MODEM: recovered at %d, restoring %d", baud, MODEM_BAUD);
        char cmd[32] = {};
        snprintf(cmd, sizeof(cmd), "+IPR=%d", MODEM_BAUD);
        at_ok(cmd, 3000);
        vTaskDelay(pdMS_TO_TICKS(150));
        ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_baudrate(MODEM_UART, MODEM_BAUD));
        vTaskDelay(pdMS_TO_TICKS(150));
        if (at_ok("", 1000)) {
            ESP_LOGI(TAG, "MODEM: restored UART baud to %d", MODEM_BAUD);
            return true;
        }
    }

    return false;
}

std::string extract_line_value(const char *response, const char *prefix)
{
    if (!response || !prefix)
        return {};

    std::string text(response);
    const bool any_data_line = prefix[0] == '\0';
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find_first_of("\r\n", pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;

        while (!line.empty() && std::isspace((unsigned char)line.front()))
            line.erase(line.begin());
        while (!line.empty() && std::isspace((unsigned char)line.back()))
            line.pop_back();

        if (line.empty() || line == "OK" || line == "ERROR" || line.rfind("AT", 0) == 0)
            continue;

        size_t value_pos = 0;
        if (!any_data_line) {
            if (line.rfind(prefix, 0) != 0)
                continue;
            value_pos = strlen(prefix);
            while (value_pos < line.size() && (line[value_pos] == ' ' || line[value_pos] == ':'))
                value_pos++;
        }

        std::string value = line.substr(value_pos);
        while (!value.empty() && value.front() == '"')
            value.erase(value.begin());
        while (!value.empty() && (value.back() == '"' || std::isspace((unsigned char)value.back())))
            value.pop_back();
        return value;
    }

    return {};
}

std::string read_identity(const char *cmd, const char *prefix)
{
    char response[384] = {};
    if (!at_collect(cmd, response, sizeof(response), 3000))
        return {};

    std::string value = extract_line_value(response, prefix);
    if (value.empty()) {
        std::string text(response);
        size_t ok = text.find("\r\nOK\r\n");
        if (ok != std::string::npos) {
            text = text.substr(0, ok);
            size_t last_crlf = text.find_last_of("\r\n");
            if (last_crlf != std::string::npos)
                value = text.substr(last_crlf + 1);
        }
    }
    while (!value.empty() && !std::isdigit((unsigned char)value.front()))
        value.erase(value.begin());
    while (!value.empty() && !std::isdigit((unsigned char)value.back()))
        value.pop_back();
    return value;
}

bool starts_with_any(const char *value, const char *const prefixes[4])
{
    if (!value || !value[0])
        return false;
    for (int i = 0; i < 4; i++) {
        if (prefixes[i] && strncmp(value, prefixes[i], strlen(prefixes[i])) == 0)
            return true;
    }
    return false;
}

const SimProfile *find_sim_profile()
{
    for (size_t i = 0; i < MODEM_CONFIG.sim_profile_count; i++) {
        const SimProfile &profile = MODEM_CONFIG.sim_profiles[i];
        if (starts_with_any(sim_imsi, profile.imsi_prefixes) ||
            starts_with_any(sim_iccid, profile.iccid_prefixes)) {
            return &profile;
        }
    }
    return nullptr;
}

bool cereg_registered(const char *response)
{
    std::string value = extract_line_value(response, "+CEREG");
    size_t comma = value.find(',');
    if (comma == std::string::npos)
        return false;
    size_t next = value.find(',', comma + 1);
    std::string stat = value.substr(comma + 1, next == std::string::npos ? std::string::npos : next - comma - 1);
    return stat == "1" || stat == "5";
}

bool wait_for_registration(uint32_t timeout_ms)
{
    set_state("waiting for cellular registration");
    int64_t start = esp_timer_get_time() / 1000;
    while ((esp_timer_get_time() / 1000) - start < timeout_ms) {
        char response[512] = {};
        at_collect("+CEREG?", response, sizeof(response), 2500);
        ESP_LOGI(TAG, "MODEM: CEREG raw %s", response);
        if (cereg_registered(response)) {
            modem_registered = true;
            at_collect("+CSQ", response, sizeof(response), 2000);
            at_collect("+COPS?", response, sizeof(response), 3000);
            at_collect("+CPSI?", response, sizeof(response), 3000);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

bool init_modem_at(const char **apn_out)
{
    set_state("initializing modem");
    if (enable_modem_rails() != ESP_OK)
        return false;
    if (init_uart() != ESP_OK)
        return false;

    bool pulsed = false;
    int64_t start = esp_timer_get_time() / 1000;
    while ((esp_timer_get_time() / 1000) - start < 30000) {
        if (recover_modem_baud()) {
            modem_ready = true;
            break;
        }
        if (!pulsed && (esp_timer_get_time() / 1000) - start > 20000) {
            ESP_LOGW(TAG, "MODEM: pulsing PWRKEY");
            pwrkey_pulse();
            pulsed = true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (!modem_ready)
        return false;

    at_ok("+CMEE=2", 2000);
    at_ok("+CFUN=1", 5000);
    at_ok("+CEREG=2", 2000);
    at_ok("+CREG=2", 2000);
    at_ok("+CGREG=2", 2000);
    at_ok("+COPS=0", 15000);
    set_modem_data_baud();

    std::string imsi = read_identity("+CIMI", "");
    std::string iccid = read_identity("+CCID", "+CCID");
    if (!imsi.empty())
        snprintf(sim_imsi, sizeof(sim_imsi), "%s", imsi.c_str());
    if (!iccid.empty())
        snprintf(sim_iccid, sizeof(sim_iccid), "%s", iccid.c_str());

    const SimProfile *profile = find_sim_profile();
    const char *apn = MODEM_CONFIG.fallback_apn;
    if (profile && profile->apn && profile->apn[0]) {
        apn = profile->apn;
        snprintf(selected_supplier, sizeof(selected_supplier), "%s", profile->supplier);
    } else {
        snprintf(selected_supplier, sizeof(selected_supplier), "fallback");
    }
    snprintf(selected_apn, sizeof(selected_apn), "%s", apn);
    *apn_out = apn;

    ESP_LOGI(TAG, "MODEM: IMSI=%s ICCID=%s supplier=%s apn=%s",
             sim_imsi, sim_iccid, selected_supplier, selected_apn);

    char cmd[96] = {};
    snprintf(cmd, sizeof(cmd), "+CGDCONT=1,\"IP\",\"%s\"", apn);
    at_ok(cmd, 5000);
    return wait_for_registration(MODEM_CONFIG.network_timeout_ms);
}

esp_err_t ppp_transmit(void *, void *buffer, size_t len)
{
    int written = uart_write_bytes(MODEM_UART, static_cast<const char *>(buffer), len);
    return written == (int)len ? ESP_OK : ESP_FAIL;
}

esp_netif_driver_ifconfig_t ppp_driver_config = {
    .handle = (void *)1,
    .transmit = ppp_transmit,
    .transmit_wrap = nullptr,
    .driver_free_rx_buffer = nullptr,
    .driver_set_mac_filter = nullptr,
};

void ppp_rx_task(void *)
{
    uint8_t *buf = static_cast<uint8_t *>(malloc(2048));
    if (!buf) {
        ESP_LOGE(TAG, "PPP: rx buffer alloc failed");
        vTaskDelete(nullptr);
    }

    while (true) {
        int len = uart_read_bytes(MODEM_UART, buf, 2048, pdMS_TO_TICKS(100));
        if (len > 0 && ppp_raw_mode && ppp_netif) {
            esp_netif_receive(ppp_netif, buf, len, nullptr);
        }
    }
}

void enable_nat()
{
    if (!ap_netif || nat_enabled)
        return;

    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK) {
        ip_napt_enable(ip.ip.addr, 1);
        nat_enabled = true;
        ESP_LOGI(TAG, "ROUTER: NAT enabled on AP %s", ip4addr_ntoa((const ip4_addr_t *)&ip.ip));
    }
}

void dns_proxy_task(void *)
{
    int listen_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "DNS: listen socket failed errno=%d", errno);
        vTaskDelete(nullptr);
    }

    sockaddr_in listen_addr = {};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    listen_addr.sin_port = htons(DNS_PROXY_PORT);
    if (bind(listen_sock, reinterpret_cast<sockaddr *>(&listen_addr), sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "DNS: bind failed errno=%d", errno);
        close(listen_sock);
        vTaskDelete(nullptr);
    }

    ESP_LOGI(TAG, "DNS: proxy listening on 192.168.4.1:%d", DNS_PROXY_PORT);
    uint8_t query[512] = {};
    uint8_t response[512] = {};

    while (true) {
        sockaddr_in client_addr = {};
        socklen_t client_len = sizeof(client_addr);
        int query_len = recvfrom(listen_sock, query, sizeof(query), 0,
                                 reinterpret_cast<sockaddr *>(&client_addr), &client_len);
        if (query_len <= 0)
            continue;

        if (!ppp_connected) {
            ESP_LOGW(TAG, "DNS: query before PPP connected");
            continue;
        }

        int upstream_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (upstream_sock < 0) {
            ESP_LOGW(TAG, "DNS: upstream socket failed errno=%d", errno);
            continue;
        }

        timeval timeout = {};
        timeout.tv_sec = 1;
        setsockopt(upstream_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        const char *upstreams[] = {ppp_dns, "8.8.8.8"};
        bool answered = false;
        for (const char *server : upstreams) {
            if (!server || !server[0] || strcmp(server, "-") == 0 || strcmp(server, "0.0.0.0") == 0)
                continue;

            sockaddr_in upstream = {};
            upstream.sin_family = AF_INET;
            upstream.sin_addr.s_addr = inet_addr(server);
            upstream.sin_port = htons(DNS_PROXY_PORT);

            int sent = sendto(upstream_sock, query, query_len, 0,
                              reinterpret_cast<sockaddr *>(&upstream), sizeof(upstream));
            if (sent != query_len) {
                ESP_LOGW(TAG, "DNS: upstream send failed server=%s errno=%d", server, errno);
                continue;
            }

            sockaddr_in upstream_from = {};
            socklen_t upstream_len = sizeof(upstream_from);
            int response_len = recvfrom(upstream_sock, response, sizeof(response), 0,
                                        reinterpret_cast<sockaddr *>(&upstream_from), &upstream_len);
            if (response_len > 0) {
                sendto(listen_sock, response, response_len, 0,
                       reinterpret_cast<sockaddr *>(&client_addr), client_len);
                answered = true;
                break;
            }
        }
        if (!answered) {
            ESP_LOGW(TAG, "DNS: upstream timeout errno=%d", errno);
        }

        close(upstream_sock);
    }
}

void ip_event_handler(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        snprintf(ppp_ip, sizeof(ppp_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ppp_connected = true;
        set_state("PPP connected");
        ESP_LOGI(TAG, "PPP: got IP " IPSTR " gw " IPSTR " netmask " IPSTR,
                 IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.gw),
                 IP2STR(&event->ip_info.netmask));
        esp_netif_dns_info_t dns = {};
        if (esp_netif_get_dns_info(ppp_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0) {
            snprintf(ppp_dns, sizeof(ppp_dns), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
            ESP_LOGI(TAG, "PPP: DNS %s", ppp_dns);
        }
        esp_netif_set_default_netif(ppp_netif);
        enable_nat();
        xEventGroupSetBits(ppp_events, PPP_CONNECTED_BIT);
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ppp_connected = false;
        nat_enabled = false;
        set_state("PPP lost IP");
    }
}

void ppp_status_handler(void *, esp_event_base_t, int32_t event_id, void *)
{
    ESP_LOGI(TAG, "PPP: status event %ld", (long)event_id);
}

esp_err_t start_ppp(const char *apn)
{
    set_state("starting PPP");

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_PPP();
    cfg.driver = &ppp_driver_config;
    ppp_netif = esp_netif_new(&cfg);
    if (!ppp_netif)
        return ESP_FAIL;
    ESP_LOGI(TAG, "PPP: netif created");

    esp_netif_ppp_config_t ppp_config = {};
    ppp_config.ppp_phase_event_enabled = true;
    ESP_RETURN_ON_ERROR(esp_netif_ppp_set_params(ppp_netif, &ppp_config), TAG, "PPP params");
    ESP_LOGI(TAG, "PPP: params set");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, nullptr), TAG, "IP handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, ppp_status_handler, nullptr), TAG, "PPP handler");
    ESP_LOGI(TAG, "PPP: event handlers registered");

    ESP_LOGI(TAG, "PPP: closing old modem data context");
    at_ok("+CNACT=0,0", 1000);
    ESP_LOGI(TAG, "PPP: attaching packet service");
    at_ok("+CGATT=1", 60000);
    char cmd[96] = {};
    snprintf(cmd, sizeof(cmd), "+CGDCONT=1,\"IP\",\"%s\"", apn);
    ESP_LOGI(TAG, "PPP: setting PDP APN=%s", apn);
    at_ok(cmd, 5000);

    char response[256] = {};
    ESP_LOGI(TAG, "PPP: dialing *99***1#");
    if (!at_collect("D*99***1#", response, sizeof(response), 30000, true) &&
        !at_collect("D*99#", response, sizeof(response), 30000, true)) {
        set_state("PPP dial failed");
        return ESP_FAIL;
    }

    ppp_raw_mode = true;
    ESP_RETURN_ON_FALSE(xTaskCreate(ppp_rx_task, "ppp_rx", 4096, nullptr, 18, nullptr) == pdPASS,
                        ESP_FAIL, TAG, "PPP rx task");
    ESP_LOGI(TAG, "PPP: rx task started");
    ESP_LOGI(TAG, "PPP: modem CONNECT, starting netif");
    esp_netif_action_start(ppp_netif, nullptr, 0, nullptr);
    esp_netif_action_connected(ppp_netif, nullptr, 0, nullptr);
    set_state("PPP dialing");
    return ESP_OK;
}

void build_ap_ssid()
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02X%02X%02X",
             AP_CONFIG.ssid_prefix, mac[3], mac[4], mac[5]);
}

esp_err_t start_wifi_ap()
{
    build_ap_ssid();

    ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(ap_netif, ESP_FAIL, TAG, "create AP netif");

    esp_netif_dhcps_stop(ap_netif);
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ipaddr_addr("192.168.4.1");
    ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns), TAG, "AP DNS");
    uint8_t offer_dns = 1;
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                               &offer_dns, sizeof(offer_dns)),
                        TAG, "AP offer DNS");
    esp_netif_dhcps_start(ap_netif);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "wifi mode");

    wifi_config_t wifi_config = {};
    size_t ssid_len = strnlen(ap_ssid, sizeof(wifi_config.ap.ssid));
    memcpy(wifi_config.ap.ssid, ap_ssid, ssid_len);
    strncpy(reinterpret_cast<char *>(wifi_config.ap.password), AP_CONFIG.password, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.ssid_len = ssid_len;
    wifi_config.ap.channel = AP_CONFIG.channel;
    wifi_config.ap.max_connection = AP_CONFIG.max_clients;
    wifi_config.ap.authmode = strlen(AP_CONFIG.password) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_RETURN_ON_FALSE(xTaskCreate(dns_proxy_task, "dns_proxy", 4096, nullptr, 5, nullptr) == pdPASS,
                        ESP_FAIL, TAG, "DNS proxy task");

    ESP_LOGI(TAG, "AP: ssid=%s password=%s ip=192.168.4.1 dns=192.168.4.1", ap_ssid, AP_CONFIG.password);
    return ESP_OK;
}

esp_err_t http_root(httpd_req_t *req)
{
    char html[1800] = {};
    snprintf(html, sizeof(html),
             "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<meta http-equiv=\"refresh\" content=\"5\"><title>T-SIM-AP</title>"
             "<style>body{font-family:Arial,sans-serif;margin:0;background:#f5f7fb;color:#172033}"
             "main{max-width:720px;margin:0 auto;padding:32px 20px}section{background:white;border:1px solid #d9e0ea;border-radius:8px;padding:20px}"
             "dl{display:grid;grid-template-columns:150px 1fr;gap:10px}dd{margin:0;font-family:monospace;overflow-wrap:anywhere}</style></head>"
             "<body><main><section><h1>T-SIM-AP Router</h1><dl>"
             "<dt>SSID</dt><dd>%s</dd><dt>Password</dt><dd>%s</dd><dt>State</dt><dd>%s</dd>"
             "<dt>PPP</dt><dd>%s</dd><dt>NAT</dt><dd>%s</dd><dt>Provider</dt><dd>%s</dd><dt>APN</dt><dd>%s</dd>"
             "<dt>PPP IP</dt><dd>%s</dd><dt>IMSI</dt><dd>%s</dd><dt>ICCID</dt><dd>%s</dd>"
             "</dl></section></main></body></html>",
             ap_ssid, AP_CONFIG.password, router_state,
             ppp_connected ? "connected" : "not connected",
             nat_enabled ? "enabled" : "disabled",
             selected_supplier, selected_apn, ppp_ip, sim_imsi, sim_iccid);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

esp_err_t start_http_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&http_server, &config), TAG, "http start");
    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = http_root;
    return httpd_register_uri_handler(http_server, &root);
}
} // namespace

extern "C" void app_main()
{
    esp_log_level_set("*", ESP_LOG_INFO);
    ppp_events = xEventGroupCreate();

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(start_wifi_ap());
    ESP_ERROR_CHECK(start_http_server());

    const char *apn = MODEM_CONFIG.fallback_apn;
    if (init_modem_at(&apn)) {
        ESP_ERROR_CHECK(start_ppp(apn));
    } else {
        set_state("modem init failed");
    }

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(ppp_events, PPP_CONNECTED_BIT | PPP_FAILED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
        if (bits & PPP_CONNECTED_BIT) {
            ESP_LOGI(TAG, "ROUTER: AP=%s password=%s PPP=%s NAT=%s",
                     ap_ssid, AP_CONFIG.password, ppp_ip, nat_enabled ? "on" : "off");
        } else {
            ESP_LOGI(TAG, "ROUTER: waiting state=%s AP=%s password=%s", router_state, ap_ssid, AP_CONFIG.password);
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
