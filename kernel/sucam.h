/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Surface Laptop Ultra front camera (OV02C10 -> MediaTek ISP8 seninf -> camsv).
 */
#ifndef SUCAM_DRV_H
#define SUCAM_DRV_H

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

#define SUCAM_W		1920
#define SUCAM_H		1080
#define SUCAM_STRIDE	2400			/* 1920 * 10 / 8, MTK packed RAW10 */
#define SUCAM_SIZE	(SUCAM_STRIDE * SUCAM_H)
#define SUCAM_FPS	30

/* MediaTek ISP packed 10-bit GBRG (little-endian bitstream), same fourcc the
 * downstream MediaTek ISP driver uses for V4L2_PIX_FMT_MTISP_SGBRG10 */
#define SUCAM_FOURCC	v4l2_fourcc('M', 'B', 'G', 'A')

/*
 * OV02C10 2-lane 1928x1092 mode. The stock table (HTS 1140, VTS 2328) assumes a
 * 19.2 MHz MCLK and runs at 18.8 fps here (line time 22.8 us, i.e. the board
 * oscillator is ~12 MHz), so the frame length is shortened to get 30 fps.
 */
#define SUCAM_VTS		1462
#define SUCAM_EXP_MIN		4
#define SUCAM_EXP_MAX		(SUCAM_VTS - 8)
#define SUCAM_EXP_DEF		1132
#define SUCAM_AGAIN_MIN		0x10
#define SUCAM_AGAIN_MAX		0xf8
#define SUCAM_AGAIN_DEF		0x80
#define SUCAM_DGAIN_MIN		0x400
#define SUCAM_DGAIN_MAX		0x3fff
#define SUCAM_DGAIN_DEF		0x400

enum { SUCAM_GPIO_CAMPOWER, SUCAM_GPIO_AVDD, SUCAM_GPIO_MCLK, SUCAM_GPIO_RESET,
       SUCAM_GPIO_PLED, SUCAM_GPIO_NUM };

struct sucam_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct sucam {
	struct device *dev;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue queue;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure, *again, *dgain;
	struct mutex lock;		/* vdev / vb2 / hw state */

	/* hw */
	void __iomem *hfrp, *ccu, *sv;
	struct i2c_client *sensor;
	struct device *sensor_dev;
	struct gpio_desc *gpio[SUCAM_GPIO_NUM];
	bool powered, streaming;

	/* buffer flow (camsv FBC, polled) */
	spinlock_t qlock;
	struct list_head queued;	/* waiting to be armed */
	struct sucam_buf *pending;	/* in the outer WDMA addr, latched at next SOF */
	struct sucam_buf *writing;	/* latched by the last SOF, being written */
	struct task_struct *poll;
	u8 last_wcnt;
	u32 sequence;
};

int sucam_hw_init(struct sucam *cam);
void sucam_hw_exit(struct sucam *cam);
int sucam_hw_start(struct sucam *cam, dma_addr_t first);
void sucam_hw_stop(struct sucam *cam);
void sucam_hw_arm(struct sucam *cam, dma_addr_t addr);
u8 sucam_hw_wcnt(struct sucam *cam);
bool sucam_hw_sof(struct sucam *cam);
void sucam_hw_diag(struct sucam *cam);
int sucam_sensor_set(struct sucam *cam, u32 id, s32 val);

#endif
