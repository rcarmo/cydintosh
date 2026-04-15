#ifndef MQTT_HOME_H
#define MQTT_HOME_H

#include "mqtt_weather.h"

#include <stdbool.h>
#include <stdint.h>

#define MQTT_HOME_MAX_LIGHTS 3
#define MQTT_HOME_MAX_PLUGS  2
#define MQTT_HOME_MAX_DOORS  2
#define MQTT_HOME_MAX_EVENTS 8

#define MQTT_HOME_LIGHT_STATUS_UPDATED 0x00
#define MQTT_HOME_LIGHT_STATUS_NOT_CHANGED 0x01
#define MQTT_HOME_POWER_STATUS_UPDATED 0x00
#define MQTT_HOME_POWER_STATUS_NOT_CHANGED 0x01
#define MQTT_HOME_DOOR_STATUS_UPDATED 0x00
#define MQTT_HOME_DOOR_STATUS_NOT_CHANGED 0x01

#define MQTT_HOME_LIGHT_SET_OK         0x00
#define MQTT_HOME_LIGHT_SET_INVALID    0x01
#define MQTT_HOME_LIGHT_SET_UNAVAILABLE 0x02

/** Compact light state mirrored from Zigbee2MQTT. */
typedef struct {
    uint8_t id;
    uint8_t state;        // 0=off, 1=on
    uint8_t brightness;   // 0-254 Zigbee brightness
    uint16_t color_temp;
    uint8_t linkquality;
    uint8_t available;
    char name[24];
    char last_seen[20];
} LightState;

/** Power-monitoring plug snapshot. */
typedef struct {
    uint8_t id;
    uint8_t state;      // 0=off, 1=on
    uint8_t available;
    uint8_t linkquality;
    uint16_t power_w;   // bounded to integer watts for IPC simplicity
    uint16_t voltage_v; // bounded to integer volts
    uint16_t current_ma;
    uint32_t energy_wh;
    char name[24];
    char last_seen[20];
} PowerState;

/** Current door sensor state. */
typedef struct {
    uint8_t id;
    uint8_t open;      // 0=closed, 1=open
    uint8_t available;
    uint8_t battery;
    uint8_t linkquality;
    uint16_t trigger_count;
    char name[24];
    char last_seen[20];
} DoorState;

/** Compact recent event entry for doors. */
typedef struct {
    uint8_t door_id;
    uint8_t open; // 0=closed, 1=open
    uint8_t seq;
    char name[24];
    char timestamp[20];
} DoorEvent;

/** Fixed-size light snapshot payload returned over IPC. */
typedef struct {
    uint8_t seq;
    uint8_t count;
    LightState lights[MQTT_HOME_MAX_LIGHTS];
} LightSnapshot;

/** Fixed-size power snapshot payload returned over IPC. */
typedef struct {
    uint8_t seq;
    uint8_t count;
    PowerState plugs[MQTT_HOME_MAX_PLUGS];
} PowerSnapshot;

/** Fixed-size door snapshot payload returned over IPC. */
typedef struct {
    uint8_t seq;
    uint8_t count;
    DoorState doors[MQTT_HOME_MAX_DOORS];
} DoorSnapshot;

/** Fixed-size door-event ring buffer payload returned over IPC. */
typedef struct {
    uint8_t seq;
    uint8_t count;
    DoorEvent events[MQTT_HOME_MAX_EVENTS];
} DoorEventSnapshot;

void mqtt_home_init(void);
void mqtt_home_start(void);

WeatherData *mqtt_home_get_weather_data(void);
uint8_t mqtt_home_get_weather_seq(void);

LightSnapshot *mqtt_home_get_light_snapshot(void);
uint8_t mqtt_home_get_light_seq(void);

PowerSnapshot *mqtt_home_get_power_snapshot(void);
uint8_t mqtt_home_get_power_seq(void);

DoorSnapshot *mqtt_home_get_door_snapshot(void);
uint8_t mqtt_home_get_door_seq(void);

DoorEventSnapshot *mqtt_home_get_door_event_snapshot(void);
uint8_t mqtt_home_get_door_event_seq(void);

uint8_t mqtt_home_set_light_state(uint8_t light_id, bool on);
uint8_t mqtt_home_set_light_brightness(uint8_t light_id, uint8_t brightness);

#endif
