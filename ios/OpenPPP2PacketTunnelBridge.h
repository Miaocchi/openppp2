#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*openppp2_ios_packet_writer)(const void* packet, int packet_size, void* user_data);
typedef void (*openppp2_ios_statistics_writer)(const char* statistics_json, void* user_data);

typedef struct openppp2_ios_tunnel_options
{
    int         mux;
    int         vnet;
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

int openppp2_ios_tap_stop(openppp2_ios_tap* tap);

int openppp2_ios_tap_input(
    openppp2_ios_tap* tap,
    const void*       packet,
    int               packet_size);

int openppp2_ios_tap_get_link_state(openppp2_ios_tap* tap);

int openppp2_ios_tap_get_statistics(
    openppp2_ios_tap* tap,
    char*             buffer,
    int               buffer_size);

const char* openppp2_ios_last_error_text(void);

#ifdef __cplusplus
}
#endif
