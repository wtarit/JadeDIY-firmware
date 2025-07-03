#ifndef _SW_JADE_ESP_CHIP_INFO_H_
#define _SW_JADE_ESP_CHIP_INFO_H_ 1

typedef struct {
    uint32_t features;
} esp_chip_info_t;

void esp_chip_info(esp_chip_info_t* out);

#endif // _SW_JADE_ESP_CHIP_INFO_H_
