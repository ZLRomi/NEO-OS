#include "hal.h"
#include "gfx_driver.h"
#include "audio.h"
#include "disk.h"
#include "pci.h"
#include "printk.h"
#include "serial.h"
#include <stdint.h>

static hal_status_t hal_status;

static int hal_probe_display(void) {
    pci_dev_t dev;
    if (pci_find_device(0x1234, 0x1111, &dev)) {
        hal_status.display_backend = HAL_DISP_BOCHS;
        hal_status.display_present = 1;
        return 1;
    }
    if (pci_find_device(0x15AD, 0x0405, &dev)) {
        hal_status.display_backend = HAL_DISP_VMWARE;
        hal_status.display_present = 1;
        return 1;
    }
    hal_status.display_backend = HAL_DISP_VBE;
    hal_status.display_present = 1;
    return 1;
}

static int hal_probe_audio(void) {
    pci_dev_t dev;
    if (pci_find_device(0x8086, 0x2415, &dev) ||
        pci_find_device(0x8086, 0x7195, &dev)) {
        hal_status.audio_backend = HAL_AUDIO_AC97;
        hal_status.audio_present = 1;
        return 1;
    }
    hal_status.audio_backend = HAL_AUDIO_NONE;
    hal_status.audio_present = 0;
    return 0;
}

static int hal_probe_disk(void) {
    hal_status.disk_backend = HAL_DISK_ATA;
    hal_status.disk_present = (disk_get_sectors() > 0);
    return hal_status.disk_present;
}

static int hal_probe_input(void) {
    hal_status.input_backend = HAL_INPUT_PS2;
    hal_status.input_present = 1;
    return 1;
}

void hal_init(void) {
    hal_status.display_backend = HAL_DISP_VBE;
    hal_status.audio_backend = HAL_AUDIO_NONE;
    hal_status.disk_backend = HAL_DISK_NONE;
    hal_status.input_backend = HAL_INPUT_PS2;
    hal_status.display_present = 0;
    hal_status.audio_present = 0;
    hal_status.disk_present = 0;
    hal_status.input_present = 0;

    hal_probe_display();
    hal_probe_audio();
    hal_probe_disk();
    hal_probe_input();

    serial_write("HAL: display=");
    serial_write(hal_backend_name(0, hal_status.display_backend));
    serial_write(" audio=");
    serial_write(hal_backend_name(1, hal_status.audio_backend));
    serial_write(" disk=");
    serial_write(hal_backend_name(2, hal_status.disk_backend));
    serial_write(" input=");
    serial_write(hal_backend_name(3, hal_status.input_backend));
    serial_write("\n");

    printk("HAL: display=%s audio=%s disk=%s input=%s\n",
           hal_backend_name(0, hal_status.display_backend),
           hal_backend_name(1, hal_status.audio_backend),
           hal_backend_name(2, hal_status.disk_backend),
           hal_backend_name(3, hal_status.input_backend));
}

const hal_status_t* hal_get_status(void) {
    return &hal_status;
}

const char* hal_backend_name(int category, int backend) {
    switch (category) {
    case 0:
        switch (backend) {
        case HAL_DISP_BOCHS:  return "Bochs BGA";
        case HAL_DISP_VMWARE: return "VMware SVGA-II";
        default:              return "VBE";
        }
    case 1:
        switch (backend) {
        case HAL_AUDIO_AC97: return "AC97";
        case HAL_AUDIO_HDA:  return "HDA";
        default:             return "none";
        }
    case 2:
        switch (backend) {
        case HAL_DISK_ATA:  return "ATA PIO";
        case HAL_DISK_AHCI: return "AHCI";
        default:            return "none";
        }
    case 3:
        switch (backend) {
        case HAL_INPUT_USB: return "USB HID";
        default:            return "PS/2";
        }
    default:
        return "unknown";
    }
}

int hal_display_set_mode(int w, int h, int bpp) {
    return gfx_set_mode(w, h, bpp);
}

uint32_t hal_display_get_fb(void) {
    return gfx_get_fb();
}

int hal_display_get_width(void) {
    return gfx_get_width();
}

int hal_display_get_height(void) {
    return gfx_get_height();
}

int hal_display_get_pitch(void) {
    return gfx_get_pitch();
}

int hal_disk_read_sector(uint32_t lba, uint8_t* buf) {
    return disk_read_sector(lba, buf);
}

int hal_disk_write_sector(uint32_t lba, const uint8_t* buf) {
    return disk_write_sector(lba, buf);
}

uint32_t hal_disk_get_sectors(void) {
    return disk_get_sectors();
}

int hal_audio_play(const void* pcm, uint32_t bytes, uint32_t rate, int channels) {
    if (!hal_status.audio_present) return -1;
    return audio_play(pcm, bytes, rate, channels, 0, 0, 0);
}

void hal_audio_stop(int handle) {
    audio_stop(handle);
}

int hal_audio_is_present(void) {
    return hal_status.audio_present;
}
