#ifndef HAL_H
#define HAL_H

#include <stdint.h>

/* 显示后端 */
#define HAL_DISP_VBE      0
#define HAL_DISP_BOCHS    1
#define HAL_DISP_VMWARE   2

/* 音频后端 */
#define HAL_AUDIO_NONE    0
#define HAL_AUDIO_AC97    1
#define HAL_AUDIO_HDA     2  /* 预留 */

/* 磁盘后端 */
#define HAL_DISK_NONE     0
#define HAL_DISK_ATA      1
#define HAL_DISK_AHCI     2  /* 预留 */

/* 输入后端 */
#define HAL_INPUT_PS2     0
#define HAL_INPUT_USB     1  /* 预留，Task 51B */

typedef struct {
    int display_backend;
    int audio_backend;
    int disk_backend;
    int input_backend;
    int display_present;
    int audio_present;
    int disk_present;
    int input_present;
} hal_status_t;

void hal_init(void);
const hal_status_t* hal_get_status(void);
const char* hal_backend_name(int category, int backend);

/* 统一设备访问（委托现有驱动） */
int  hal_display_set_mode(int w, int h, int bpp);
uint32_t hal_display_get_fb(void);
int  hal_display_get_width(void);
int  hal_display_get_height(void);
int  hal_display_get_pitch(void);

int  hal_disk_read_sector(uint32_t lba, uint8_t* buf);
int  hal_disk_write_sector(uint32_t lba, const uint8_t* buf);
uint32_t hal_disk_get_sectors(void);

int  hal_audio_play(const void* pcm, uint32_t bytes, uint32_t rate, int channels);
void hal_audio_stop(int handle);
int  hal_audio_is_present(void);

#endif
