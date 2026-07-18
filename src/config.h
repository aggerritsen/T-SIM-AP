#pragma once

#include <cstddef>
#include <cstdint>

struct ApConfig {
    const char *ssid_prefix;
    const char *password;
    uint8_t channel;
    uint8_t max_clients;
};

struct ApnCandidate {
    const char *supplier;
    const char *apn;
};

struct SimProfile {
    const char *supplier;
    const char *apn;
    const char *imsi_prefixes[4];
    const char *iccid_prefixes[4];
};

struct ModemConfig {
    bool enabled;
    bool operator_auto_select;
    int data_baud;
    uint32_t network_timeout_ms;
    const char *preferred_radio_mode;
    const char *fallback_apn;
    const ApnCandidate *apn_candidates;
    size_t apn_candidate_count;
    const SimProfile *sim_profiles;
    size_t sim_profile_count;
};

static constexpr ApConfig AP_CONFIG = {
    "AP",
    "tsim7080",
    6,
    4,
};

static constexpr ApnCandidate APN_CANDIDATES[] = {
    {"Onomondo", "onomondo"},
    {"KPNThings", "internet.m2m"},
    {"ThingsData/Tele2 2G-4G", "m2m.tele2.com"},
    {"ThingsData/Tele2 5G", "iot.tele2.com"},
};

static constexpr SimProfile SIM_PROFILES[] = {
    {"Onomondo", "onomondo", {"23450", "23873", nullptr, nullptr}, {"894573", nullptr, nullptr, nullptr}},
    {"KPNThings", "internet.m2m", {"20408", nullptr, nullptr, nullptr}, {nullptr, nullptr, nullptr, nullptr}},
    {"ThingsData/Tele2 2G-4G", "m2m.tele2.com", {"20402", "24007", nullptr, nullptr}, {"894620", nullptr, nullptr, nullptr}},
};

static constexpr ModemConfig MODEM_CONFIG = {
    true,
    true,
    115200,
    60000,
    "CAT-M",
    "internet.m2m",
    APN_CANDIDATES,
    sizeof(APN_CANDIDATES) / sizeof(APN_CANDIDATES[0]),
    SIM_PROFILES,
    sizeof(SIM_PROFILES) / sizeof(SIM_PROFILES[0]),
};
