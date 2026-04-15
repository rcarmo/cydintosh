#include "mqtt_home.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "mqtt_home";

static const char *LIGHT_NAMES[MQTT_HOME_MAX_LIGHTS] = {
    "Office Desk Strip",
    "Office Wall Strip",
    "Office Shelves Strip",
};

static const char *PLUG_NAMES[MQTT_HOME_MAX_PLUGS] = {
    "Office Desk UPS Power",
    "Office Benchmark Plug",
};

static const char *DOOR_NAMES[MQTT_HOME_MAX_DOORS] = {
    "Hallway Main Door",
    "Hallway Service Door",
};

static esp_mqtt_client_handle_t mqtt_client = NULL;
static WeatherData weather_data;
static LightSnapshot light_snapshot;
static PowerSnapshot power_snapshot;
static DoorSnapshot door_snapshot;
static DoorEventSnapshot door_event_snapshot;

static void copy_string(char *dest, size_t dest_size, const char *src, size_t src_len) {
    if (dest_size == 0) {
        return;
    }
    size_t copy_len = src_len < (dest_size - 1) ? src_len : (dest_size - 1);
    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
}

static int parse_topic_index(const char *topic, const char *prefix) {
    const char *p = strstr(topic, prefix);
    if (!p) {
        return -1;
    }
    p += strlen(prefix);
    while (*p >= '0' && *p <= '9') {
        int idx = atoi(p);
        if (idx >= 0 && idx < 10) {
            return idx;
        }
        p++;
    }
    return -1;
}

static uint8_t parse_icon(const char *str) {
    if (strstr(str, "sunny") || strstr(str, "clear"))
        return WEATHER_ICON_SUN;
    if (strstr(str, "partly"))
        return WEATHER_ICON_SUN_CLOUD;
    if (strstr(str, "cloud"))
        return WEATHER_ICON_CLOUD;
    if (strstr(str, "rain") || strstr(str, "shower") || strstr(str, "pouring"))
        return WEATHER_ICON_RAIN;
    if (strstr(str, "snow"))
        return WEATHER_ICON_SNOW;
    if (strstr(str, "fog") || strstr(str, "hail") || strstr(str, "lightning") ||
        strstr(str, "windy") || strstr(str, "exceptional"))
        return WEATHER_ICON_CLOUD;
    return WEATHER_ICON_SUN;
}

static bool json_extract_string(const char *json, const char *key, char *out, size_t out_size) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (!start) {
        return false;
    }
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    if (!end) {
        return false;
    }
    copy_string(out, out_size, start, (size_t)(end - start));
    return true;
}

static bool json_extract_int(const char *json, const char *key, int *value) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *start = strstr(json, pattern);
    if (!start) {
        return false;
    }
    start += strlen(pattern);
    *value = atoi(start);
    return true;
}

static bool json_extract_bool(const char *json, const char *key, bool *value) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *start = strstr(json, pattern);
    if (!start) {
        return false;
    }
    start += strlen(pattern);
    if (strncmp(start, "true", 4) == 0) {
        *value = true;
        return true;
    }
    if (strncmp(start, "false", 5) == 0) {
        *value = false;
        return true;
    }
    return false;
}

static int topic_matches_name(const char *topic, const char *prefix, const char *name) {
    char full[96];
    snprintf(full, sizeof(full), "%s%s", prefix, name);
    return strcmp(topic, full) == 0;
}

static void init_snapshots(void) {
    memset(&weather_data, 0, sizeof(weather_data));
    memset(&light_snapshot, 0, sizeof(light_snapshot));
    memset(&power_snapshot, 0, sizeof(power_snapshot));
    memset(&door_snapshot, 0, sizeof(door_snapshot));
    memset(&door_event_snapshot, 0, sizeof(door_event_snapshot));

    light_snapshot.count = MQTT_HOME_MAX_LIGHTS;
    power_snapshot.count = MQTT_HOME_MAX_PLUGS;
    door_snapshot.count = MQTT_HOME_MAX_DOORS;

    for (int i = 0; i < MQTT_HOME_MAX_LIGHTS; i++) {
        light_snapshot.lights[i].id = (uint8_t)i;
        copy_string(light_snapshot.lights[i].name, sizeof(light_snapshot.lights[i].name),
                    LIGHT_NAMES[i], strlen(LIGHT_NAMES[i]));
    }
    for (int i = 0; i < MQTT_HOME_MAX_PLUGS; i++) {
        power_snapshot.plugs[i].id = (uint8_t)i;
        copy_string(power_snapshot.plugs[i].name, sizeof(power_snapshot.plugs[i].name),
                    PLUG_NAMES[i], strlen(PLUG_NAMES[i]));
    }
    for (int i = 0; i < MQTT_HOME_MAX_DOORS; i++) {
        door_snapshot.doors[i].id = (uint8_t)i;
        copy_string(door_snapshot.doors[i].name, sizeof(door_snapshot.doors[i].name),
                    DOOR_NAMES[i], strlen(DOOR_NAMES[i]));
    }
}

static void push_door_event(uint8_t door_id, bool open, const char *timestamp) {
    if (door_event_snapshot.count < MQTT_HOME_MAX_EVENTS) {
        door_event_snapshot.count++;
    }

    for (int i = MQTT_HOME_MAX_EVENTS - 1; i > 0; i--) {
        door_event_snapshot.events[i] = door_event_snapshot.events[i - 1];
    }

    DoorEvent *event = &door_event_snapshot.events[0];
    memset(event, 0, sizeof(*event));
    event->door_id = door_id;
    event->open = open ? 1 : 0;
    event->seq = door_event_snapshot.seq + 1;
    copy_string(event->name, sizeof(event->name), DOOR_NAMES[door_id], strlen(DOOR_NAMES[door_id]));
    copy_string(event->timestamp, sizeof(event->timestamp), timestamp, strlen(timestamp));
    door_event_snapshot.seq++;
}

static void handle_current_topic(const char *topic, const char *data) {
    if (strstr(topic, "/location")) {
        copy_string(weather_data.location, sizeof(weather_data.location), data, strlen(data));
    } else if (strstr(topic, "/temp")) {
        weather_data.temp_current = atoi(data);
    } else if (strstr(topic, "/condition")) {
        weather_data.condition = parse_icon(data);
    } else if (strstr(topic, "/humidity")) {
        weather_data.humidity = atoi(data);
    } else if (strstr(topic, "/wind_speed")) {
        weather_data.wind_speed = atoi(data);
    }
}

static void handle_hourly_topic(const char *topic, const char *data) {
    int idx = parse_topic_index(topic, "/hourly/");
    if (idx < 0 || idx >= 5)
        return;

    char *slash = strchr(topic + strlen(MQTT_TOPIC_BASE) + 9, '/');
    if (!slash)
        return;

    const char *field = slash + 1;
    if (strstr(field, "time")) {
        copy_string(weather_data.hourly[idx].time, sizeof(weather_data.hourly[idx].time), data,
                    strlen(data));
    } else if (strstr(field, "icon")) {
        weather_data.hourly[idx].icon = parse_icon(data);
    } else if (strstr(field, "temp")) {
        weather_data.hourly[idx].temp = atoi(data);
    }
}

static void handle_forecast_topic(const char *topic, const char *data) {
    int idx = parse_topic_index(topic, "/forecast/");
    if (idx < 0 || idx >= 3)
        return;

    char *slash = strchr(topic + strlen(MQTT_TOPIC_BASE) + 10, '/');
    if (!slash)
        return;

    const char *field = slash + 1;
    if (strstr(field, "date")) {
        copy_string(weather_data.forecast[idx].date, sizeof(weather_data.forecast[idx].date), data,
                    strlen(data));
    } else if (strstr(field, "day")) {
        copy_string(weather_data.forecast[idx].day, sizeof(weather_data.forecast[idx].day), data,
                    strlen(data));
    } else if (strstr(field, "icon")) {
        weather_data.forecast[idx].icon = parse_icon(data);
    } else if (strstr(field, "hi")) {
        weather_data.forecast[idx].hi = atoi(data);
    } else if (strstr(field, "lo")) {
        weather_data.forecast[idx].lo = atoi(data);
    }
}

static void handle_weather_topic(const char *topic, const char *data) {
    if (strstr(topic, "/current/")) {
        handle_current_topic(topic, data);
    } else if (strstr(topic, "/hourly/")) {
        handle_hourly_topic(topic, data);
    } else if (strstr(topic, "/forecast/")) {
        handle_forecast_topic(topic, data);
    } else if (strstr(topic, "/updated")) {
        copy_string(weather_data.updated, sizeof(weather_data.updated), data, strlen(data));
        weather_data.seq++;
    }
}

static void handle_light_topic(const char *topic, const char *data) {
    for (int i = 0; i < MQTT_HOME_MAX_LIGHTS; i++) {
        if (!topic_matches_name(topic, "zigbee2mqtt/", LIGHT_NAMES[i])) {
            continue;
        }

        LightState *light = &light_snapshot.lights[i];
        int value = 0;
        char text[24];

        if (json_extract_string(data, "state", text, sizeof(text))) {
            light->state = strcmp(text, "ON") == 0 ? 1 : 0;
            light->available = 1;
        }
        if (json_extract_int(data, "brightness", &value)) {
            if (value < 0) value = 0;
            if (value > 254) value = 254;
            light->brightness = (uint8_t)value;
        }
        if (json_extract_int(data, "color_temp", &value)) {
            if (value < 0) value = 0;
            if (value > 65535) value = 65535;
            light->color_temp = (uint16_t)value;
        }
        if (json_extract_int(data, "linkquality", &value)) {
            if (value < 0) value = 0;
            if (value > 255) value = 255;
            light->linkquality = (uint8_t)value;
        }
        if (json_extract_string(data, "last_seen", text, sizeof(text))) {
            copy_string(light->last_seen, sizeof(light->last_seen), text, strlen(text));
        }
        light_snapshot.seq++;
        return;
    }
}

static void handle_power_topic(const char *topic, const char *data) {
    for (int i = 0; i < MQTT_HOME_MAX_PLUGS; i++) {
        if (!topic_matches_name(topic, "zigbee2mqtt/", PLUG_NAMES[i])) {
            continue;
        }

        PowerState *plug = &power_snapshot.plugs[i];
        int value = 0;
        char text[24];
        bool state_bool = false;

        if (json_extract_string(data, "state", text, sizeof(text))) {
            plug->state = strcmp(text, "ON") == 0 ? 1 : 0;
            plug->available = 1;
        } else if (json_extract_bool(data, "state", &state_bool)) {
            plug->state = state_bool ? 1 : 0;
            plug->available = 1;
        }
        if (json_extract_int(data, "power", &value)) {
            if (value < 0) value = 0;
            if (value > 65535) value = 65535;
            plug->power_w = (uint16_t)value;
        }
        if (json_extract_int(data, "voltage", &value)) {
            if (value < 0) value = 0;
            if (value > 65535) value = 65535;
            plug->voltage_v = (uint16_t)value;
        }
        if (json_extract_int(data, "current", &value)) {
            if (value < 0) value = 0;
            if (value > 65535) value = 65535;
            plug->current_ma = (uint16_t)value;
        }
        if (json_extract_int(data, "energy", &value)) {
            if (value < 0) value = 0;
            plug->energy_wh = (uint32_t)value;
        }
        if (json_extract_int(data, "linkquality", &value)) {
            if (value < 0) value = 0;
            if (value > 255) value = 255;
            plug->linkquality = (uint8_t)value;
        }
        if (json_extract_string(data, "last_seen", text, sizeof(text))) {
            copy_string(plug->last_seen, sizeof(plug->last_seen), text, strlen(text));
        }
        power_snapshot.seq++;
        return;
    }
}

static void handle_door_topic(const char *topic, const char *data) {
    for (int i = 0; i < MQTT_HOME_MAX_DOORS; i++) {
        if (!topic_matches_name(topic, "zigbee2mqtt/", DOOR_NAMES[i])) {
            continue;
        }

        DoorState *door = &door_snapshot.doors[i];
        bool contact = true;
        bool changed = false;
        int value = 0;
        char text[24] = {0};
        uint8_t previous_open = door->open;

        if (json_extract_bool(data, "contact", &contact)) {
            door->open = contact ? 0 : 1;
            door->available = 1;
            changed = true;
        }
        if (json_extract_int(data, "battery", &value)) {
            if (value < 0) value = 0;
            if (value > 100) value = 100;
            door->battery = (uint8_t)value;
        }
        if (json_extract_int(data, "linkquality", &value)) {
            if (value < 0) value = 0;
            if (value > 255) value = 255;
            door->linkquality = (uint8_t)value;
        }
        if (json_extract_int(data, "trigger_count", &value)) {
            if (value < 0) value = 0;
            if (value > 65535) value = 65535;
            door->trigger_count = (uint16_t)value;
        }
        if (json_extract_string(data, "last_seen", text, sizeof(text))) {
            copy_string(door->last_seen, sizeof(door->last_seen), text, strlen(text));
        }

        if (changed && (door_snapshot.seq == 0 || previous_open != door->open)) {
            const char *ts = door->last_seen[0] ? door->last_seen : "event";
            push_door_event((uint8_t)i, door->open == 1, ts);
        }
        door_snapshot.seq++;
        return;
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                               void *event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected, subscribing to home topics");
        esp_mqtt_client_subscribe(mqtt_client, MQTT_TOPIC_SUBSCRIBE, MQTT_QOS);
        esp_mqtt_client_subscribe(mqtt_client, "zigbee2mqtt/Office Desk Strip", MQTT_QOS);
        esp_mqtt_client_subscribe(mqtt_client, "zigbee2mqtt/Office Wall Strip", MQTT_QOS);
        esp_mqtt_client_subscribe(mqtt_client, "zigbee2mqtt/Office Shelves Strip", MQTT_QOS);
        esp_mqtt_client_subscribe(mqtt_client, "zigbee2mqtt/Office Desk UPS Power", MQTT_QOS);
        esp_mqtt_client_subscribe(mqtt_client, "zigbee2mqtt/Office Benchmark Plug", MQTT_QOS);
        esp_mqtt_client_subscribe(mqtt_client, "zigbee2mqtt/Hallway Main Door", MQTT_QOS);
        esp_mqtt_client_subscribe(mqtt_client, "zigbee2mqtt/Hallway Service Door", MQTT_QOS);
        break;

    case MQTT_EVENT_DATA: {
        char topic[96];
        char data[256];
        int topic_len = event->topic_len < 95 ? event->topic_len : 95;
        int data_len = event->data_len < 255 ? event->data_len : 255;

        memcpy(topic, event->topic, topic_len);
        topic[topic_len] = '\0';
        memcpy(data, event->data, data_len);
        data[data_len] = '\0';

        if (strncmp(topic, MQTT_TOPIC_BASE, strlen(MQTT_TOPIC_BASE)) == 0) {
            handle_weather_topic(topic, data);
        } else if (strncmp(topic, "zigbee2mqtt/Office ", 18) == 0) {
            handle_light_topic(topic, data);
            handle_power_topic(topic, data);
        } else if (strncmp(topic, "zigbee2mqtt/Hallway ", 20) == 0) {
            handle_door_topic(topic, data);
        }
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        break;

    default:
        break;
    }
}

void mqtt_home_init(void) {
    init_snapshots();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
}

void mqtt_home_start(void) {
    if (mqtt_client) {
        esp_mqtt_client_start(mqtt_client);
        ESP_LOGI(TAG, "MQTT home client started");
    }
}

WeatherData *mqtt_home_get_weather_data(void) {
    return &weather_data;
}

uint8_t mqtt_home_get_weather_seq(void) {
    return weather_data.seq;
}

LightSnapshot *mqtt_home_get_light_snapshot(void) {
    return &light_snapshot;
}

uint8_t mqtt_home_get_light_seq(void) {
    return light_snapshot.seq;
}

PowerSnapshot *mqtt_home_get_power_snapshot(void) {
    return &power_snapshot;
}

uint8_t mqtt_home_get_power_seq(void) {
    return power_snapshot.seq;
}

DoorSnapshot *mqtt_home_get_door_snapshot(void) {
    return &door_snapshot;
}

uint8_t mqtt_home_get_door_seq(void) {
    return door_snapshot.seq;
}

DoorEventSnapshot *mqtt_home_get_door_event_snapshot(void) {
    return &door_event_snapshot;
}

uint8_t mqtt_home_get_door_event_seq(void) {
    return door_event_snapshot.seq;
}

static uint8_t publish_json_command(const char *friendly_name, const char *json_payload) {
    if (!mqtt_client) {
        return MQTT_HOME_LIGHT_SET_UNAVAILABLE;
    }
    char topic[96];
    snprintf(topic, sizeof(topic), "zigbee2mqtt/%s/set", friendly_name);
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, json_payload, 0, MQTT_QOS, 0);
    return msg_id >= 0 ? MQTT_HOME_LIGHT_SET_OK : MQTT_HOME_LIGHT_SET_UNAVAILABLE;
}

uint8_t mqtt_home_set_light_state(uint8_t light_id, bool on) {
    if (light_id >= MQTT_HOME_MAX_LIGHTS) {
        return MQTT_HOME_LIGHT_SET_INVALID;
    }
    return publish_json_command(LIGHT_NAMES[light_id], on ? "{\"state\":\"ON\"}" : "{\"state\":\"OFF\"}");
}

uint8_t mqtt_home_set_light_brightness(uint8_t light_id, uint8_t brightness) {
    if (light_id >= MQTT_HOME_MAX_LIGHTS) {
        return MQTT_HOME_LIGHT_SET_INVALID;
    }
    char payload[40];
    snprintf(payload, sizeof(payload), "{\"brightness\":%u}", brightness);
    return publish_json_command(LIGHT_NAMES[light_id], payload);
}
