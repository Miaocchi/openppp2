#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*openppp2_ios_packet_writer)(const void* packet, int packet_size, void* user_data);
typedef void (*openppp2_ios_statistics_writer)(const char* statistics_json, void* user_data);
typedef int (*openppp2_ios_http_post_fn)(const char* url, const void* body, int body_len, void* user_data);

typedef struct openppp2_ios_tunnel_options
{
    int         mux;
    int         vnet;
    int         lwip;
    int         block_quic;
    int         static_mode;
    const char* ip;
    const char* mask;
    const char* bypass_ip_list;
    const char* dns_rules_list;
    const char* root_path;
} openppp2_ios_tunnel_options;

typedef struct openppp2_ios_tap openppp2_ios_tap;

const char* openppp2_ios_version(void);

openppp2_ios_tap* openppp2_ios_tap_create(
    openppp2_ios_packet_writer writer,
    void*                      user_data);

void openppp2_ios_tap_destroy(openppp2_ios_tap* tap);

int openppp2_ios_tap_start(
    openppp2_ios_tap*                  tap,
    const char*                        configuration_json,
    const openppp2_ios_tunnel_options* options,
    openppp2_ios_statistics_writer     statistics_writer,
    void*                              statistics_user_data);

int openppp2_ios_tap_stop(openppp2_ios_tap* tap, int stop_reason);

int openppp2_ios_tap_input(
    openppp2_ios_tap* tap,
    const void*       packet,
    int               packet_size);

int openppp2_ios_tap_get_link_state(openppp2_ios_tap* tap);

int openppp2_ios_tap_get_statistics(
    openppp2_ios_tap* tap,
    char*             buffer,
    int               buffer_size);

int openppp2_ios_tap_get_start_stage(
    openppp2_ios_tap* tap,
    char*             buffer,
    int               buffer_size);

const char* openppp2_ios_last_error_text(void);

void openppp2_ios_set_telemetry_http_post(
    openppp2_ios_http_post_fn fn,
    void*                      user_data);

#ifdef __cplusplus
}
#endif
