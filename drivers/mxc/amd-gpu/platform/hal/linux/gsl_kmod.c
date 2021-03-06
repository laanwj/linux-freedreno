/* Copyright (c) 2008-2010, Advanced Micro Devices. All rights reserved.
 * Copyright (C) 2008-2011 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include "gsl_types.h"
#include "gsl.h"
#include "gsl_buildconfig.h"
#include "gsl_halconfig.h"
#include "gsl_ioctl.h"
#include "gsl_kmod_cleanup.h"
#include "gsl_linux_map.h"

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_device.h>
#endif

#include <linux/debugfs.h>
#include <linux/io.h>

#include "mxc_gpu.h"

int gpu_2d_irq, gpu_3d_irq;

phys_addr_t gpu_2d_regbase;
int gpu_2d_regsize;
phys_addr_t gpu_3d_regbase;
int gpu_3d_regsize;
int gmem_size;
phys_addr_t gpu_reserved_mem;
int gpu_reserved_mem_size;
int z160_version;
int enable_mmu;
bool debugfs_dumpcmdbufs;
bool debugfs_noisy;

static ssize_t gsl_kmod_read(struct file *fd, char __user *buf, size_t len, loff_t *ptr);
static ssize_t gsl_kmod_write(struct file *fd, const char __user *buf, size_t len, loff_t *ptr);
static long gsl_kmod_ioctl(struct file *fd, unsigned int cmd, unsigned long arg);
static int gsl_kmod_mmap(struct file *fd, struct vm_area_struct *vma);
static int gsl_kmod_fault(struct vm_fault *vmf);
static int gsl_kmod_open(struct inode *inode, struct file *fd);
static int gsl_kmod_release(struct inode *inode, struct file *fd);
static irqreturn_t z160_irq_handler(int irq, void *dev_id);
static irqreturn_t z430_irq_handler(int irq, void *dev_id);

static int gsl_kmod_major;
static struct class *gsl_kmod_class;
DEFINE_MUTEX(gsl_mutex);

static const struct file_operations gsl_kmod_fops =
{
    .owner = THIS_MODULE,
    .read = gsl_kmod_read,
    .write = gsl_kmod_write,
    .unlocked_ioctl = gsl_kmod_ioctl,
    .mmap = gsl_kmod_mmap,
    .open = gsl_kmod_open,
    .release = gsl_kmod_release
};

static struct vm_operations_struct gsl_kmod_vmops =
{
	.fault = gsl_kmod_fault,
};

static ssize_t gsl_kmod_read(struct file *fd, char __user *buf, size_t len, loff_t *ptr)
{
    return 0;
}

static ssize_t gsl_kmod_write(struct file *fd, const char __user *buf, size_t len, loff_t *ptr)
{
    return 0;
}


static void mf_dump_vertex_bufs(u32 *cmd_buf, u32 cmd_dwords, u32 *already, int *nb_already, const char *owner)
{
	int i, j;
	char hdr[50];

	for (i=0; i<cmd_dwords; i++) {
		if ((cmd_buf[i] == 0xc0062d00 || cmd_buf[i] == 0xc0022d00) &&
			(cmd_buf[i+1] == 0x00010078 || cmd_buf[i+1] == 0x0001009c)) {
			u32 gpu_addr = cmd_buf[i+2];
			u32 len = cmd_buf[i+3];
			u32 *vtx_buf;
			bool duplicate = false;

			i+= 7;

			for (j=0; j < *nb_already; j++) {
				if (gpu_addr == already[j]) {
					duplicate = true;
					break;
				}
			}
			if (duplicate)
				break;
			already[*nb_already] = gpu_addr;
			*nb_already += 1;

			vtx_buf = (u32 *)kgsl_sharedmem_convertaddr(gpu_addr & ~0x3, 0);

			printk(KERN_INFO "@MF@ dumping vertex buf gpu=%08x host=%p\n", gpu_addr, vtx_buf);

			snprintf(hdr, sizeof(hdr), "VTX: %08x (%s) > ", gpu_addr, owner);
			print_hex_dump(KERN_INFO, hdr, DUMP_PREFIX_OFFSET, 16, 4,
					vtx_buf, len, false);
		}
	}
}

static void mf_dump_drawindx_buf(u32 *cmd_buf, u32 cmd_dwords)
{
	int i;

	for (i=0; i<cmd_dwords; i++) {
		u32 gpu_addr;
		u32 *buf;

		if (cmd_buf[i] != 0xc0032200)
			continue;

		gpu_addr = cmd_buf[i+3];

		printk(KERN_INFO "@MF@ found long form DRAW_INDX addr=%08x\n", gpu_addr);

		buf = (u32 *)kgsl_sharedmem_convertaddr(gpu_addr, 0);
		print_hex_dump(KERN_INFO, "DRAW_INDX ", DUMP_PREFIX_OFFSET, 16, 4, buf, 64, false);
	}
}

static void mf_patch_ib(u32 *cmd_buf, u32 cmd_dwords)
{
#if 0
	int i, j;

	for (i=0; i<cmd_dwords; i++) {
		char *reason = NULL;

		switch(cmd_buf[i] & 0xff00ff00) {
		case 0xc0003400:
			reason = "CP_DRAW_INDX_BIN";
			break;
		case 0xc0004b00:
			reason = "CP_SET_DRAW_INIT_FLAGS";
			break;

		case 0xc0002d00:
			switch(cmd_buf[i+1]) {
			case 0x00040203:
				reason = "CP_SET_CONSTANT(VGT_CURRENT_BIN_ID_MAX)";
				break;
			case 0x00040207:
				reason = "CP_SET_CONSTANT(VGT_CURRENT_BIN_ID_MIN)";
				break;
			}
			break;
		}

		if (reason) {
			int zcnt = ((cmd_buf[i] >> 16) & 0xff) + 1;

			printk(KERN_INFO "@MF@ nopping %s @%x\n", reason, (i*4));

			for (j=0; j<= zcnt; j++)
			    cmd_buf[i+j] = 0x80000000;
			i += zcnt;
		}
	}
#endif
}

static long gsl_kmod_ioctl(struct file *fd, unsigned int cmd, unsigned long arg)
{
    int kgslStatus = GSL_FAILURE;

    switch (cmd) {
    case IOCTL_KGSL_DEVICE_START:
        {
            kgsl_device_start_t param;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_device_start_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_device_start(param.device_id, param.flags);
            break;
        }
    case IOCTL_KGSL_DEVICE_STOP:
        {
            kgsl_device_stop_t param;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_device_stop_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_device_stop(param.device_id);
            break;
        }
    case IOCTL_KGSL_DEVICE_IDLE:
        {
            kgsl_device_idle_t param;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_device_idle_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_device_idle(param.device_id, param.timeout);
            break;
        }
    case IOCTL_KGSL_DEVICE_ISIDLE:
        {
            kgsl_device_isidle_t param;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_device_isidle_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_device_isidle(param.device_id);
            break;
        }
    case IOCTL_KGSL_DEVICE_GETPROPERTY:
        {
            kgsl_device_getproperty_t param;
            void *tmp;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_device_getproperty_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            tmp = kmalloc(param.sizebytes, GFP_KERNEL);
            if (!tmp)
            {
                printk(KERN_ERR "%s:kmalloc error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_device_getproperty(param.device_id, param.type, tmp, param.sizebytes);
            if (kgslStatus == GSL_SUCCESS)
            {
                if (copy_to_user(param.value, tmp, param.sizebytes))
                {
                    printk(KERN_ERR "%s: copy_to_user error\n", __func__);
                    kgslStatus = GSL_FAILURE;
                    kfree(tmp);
                    break;
                }
            }
            else
            {
                printk(KERN_ERR "%s: kgsl_device_getproperty error\n", __func__);
            }
            kfree(tmp);
            break;
        }
    case IOCTL_KGSL_DEVICE_SETPROPERTY:
        {
            kgsl_device_setproperty_t param;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_device_setproperty_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_device_setproperty(param.device_id, param.type, param.value, param.sizebytes);
            if (kgslStatus != GSL_SUCCESS)
            {
                printk(KERN_ERR "%s: kgsl_device_setproperty error\n", __func__);
            }
            break;
        }
    case IOCTL_KGSL_DEVICE_REGREAD:
        {
            kgsl_device_regread_t param;
            unsigned int tmp;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_device_regread_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_device_regread(param.device_id, param.offsetwords, &tmp);
            if (kgslStatus == GSL_SUCCESS)
            {
                if (copy_to_user(param.value, &tmp, sizeof(unsigned int)))
                {
                    printk(KERN_ERR "%s: copy_to_user error\n", __func__);
                    kgslStatus = GSL_FAILURE;
                    break;
                }
            }
            break;
        }
    case IOCTL_KGSL_DEVICE_REGWRITE:
        {
            kgsl_device_regwrite_t param;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_device_regwrite_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_device_regwrite(param.device_id, param.offsetwords, param.value);
            break;
        }
    case IOCTL_KGSL_DEVICE_WAITIRQ:
        {
            kgsl_device_waitirq_t param;
            unsigned int count;

            printk(KERN_ERR "IOCTL_KGSL_DEVICE_WAITIRQ obsoleted!\n");
//          kgslStatus = -ENOTTY; break;

            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_device_waitirq_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_device_waitirq(param.device_id, param.intr_id, &count, param.timeout);
            if (kgslStatus == GSL_SUCCESS)
            {
                if (copy_to_user(param.count, &count, sizeof(unsigned int)))
                {
                    printk(KERN_ERR "%s: copy_to_user error\n", __func__);
                    kgslStatus = GSL_FAILURE;
                    break;
                }
            }
            break;
        }
    case IOCTL_KGSL_CMDSTREAM_ISSUEIBCMDS:
        {
            kgsl_cmdstream_issueibcmds_t param;
            gsl_timestamp_t tmp;

            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_cmdstream_issueibcmds_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }

	    if (debugfs_dumpcmdbufs) /* dump full command buffers: NOISY */
            {
            	    u32 *mf_buf;
            	    char hdr[50];
            	    u32 subs[10];
            	    u32 vbufs[10];
            	    u32 vbuf_cnt = 0;
            	    int sub_cnt = 0;
            	    static int ib_idx = 0;
            	    int i, j;

#if 0
            	    if (param.sizedwords == 999)
            	    	    param.sizedwords = 342;
#endif

            	    mf_buf = (u32 *)kgsl_sharedmem_convertaddr(param.ibaddr, 0);
		    mf_patch_ib(mf_buf, param.sizedwords);
            	    snprintf(hdr, sizeof(hdr), "@MF@ ib=%d ic=%d >", ib_idx, param.drawctxt_index);
            	    printk(KERN_INFO "@MF@ dumping IB gpu=%08x host=%p hdr='%s'\n",
            	    	    param.ibaddr, mf_buf, hdr);

            	    print_hex_dump(KERN_INFO, hdr, DUMP_PREFIX_OFFSET, 32, 4,
				mf_buf,
				param.sizedwords * 4, false);

		    mf_dump_drawindx_buf(mf_buf, param.sizedwords);
		    mf_dump_vertex_bufs(mf_buf, param.sizedwords, vbufs, &vbuf_cnt, hdr);

		    for (i=0; i<param.sizedwords; i++) {
		    	    if (mf_buf[i] == 0xc0004b00) { /* CP_SET_DRAW_INIT_FLAGS */
		    	    	    u32 *init_buf;
		    	    	    u32 init_addr;

		    	    	    init_addr = mf_buf[i+1];

		    	    	    if (init_addr) {
					    printk(KERN_INFO "@MF@ contents of CP_SET_DRAW_INIT_FLAGS @%08x:\n", init_addr);
					    init_buf = (u32 *)kgsl_sharedmem_convertaddr(init_addr, 0);
					    print_hex_dump(KERN_INFO, "INIT_BUF ", DUMP_PREFIX_OFFSET, 32, 4,
								init_buf,
								64, false);

					    //mf_buf[++i] = 0;
				    }
		    	    	    continue;
		    	    }

		    	    if (mf_buf[i] == 0xc0013700) {
		    	    	    u32 *sub_buf;
		    	    	    u32 sub_gpu, sub_len;
		    	    	    bool duplicate = false;

		    	    	    sub_gpu = mf_buf[++i];
		    	    	    sub_len = mf_buf[++i];

		    	    	    for (j=0; j < sub_cnt; j++) {
		    	    	    	    if (subs[j] == sub_gpu) {
		    	    	    		duplicate = true;
		    	    	    		break;
		    	    	    	    }
		    	    	    }
		    	    	    if (duplicate)
		    	    	    	    break;
		    	    	    subs[sub_cnt++] = sub_gpu;

		    	    	    sub_buf = (u32 *)kgsl_sharedmem_convertaddr(sub_gpu, 0);
				    mf_patch_ib(sub_buf, sub_len);

		    	    	    printk(KERN_INFO "@MF@ dumping sub IB gpu=%08x host=%p\n",
		    	    	    	    sub_gpu, sub_buf);

		    	    	    snprintf(hdr, sizeof(hdr), "@MF@ ib=%d sub=%08x >", ib_idx, sub_gpu);
		    	    	    print_hex_dump(KERN_INFO, hdr, DUMP_PREFIX_OFFSET, 32, 4,
		    	    	    	    		sub_buf, sub_len * 4, false);

		    	    	    mf_dump_drawindx_buf(sub_buf, sub_len);
    	    	    		    mf_dump_vertex_bufs(sub_buf, sub_len, vbufs, &vbuf_cnt, hdr);
		    	    }
		    }
		    ib_idx++;
            }

            kgslStatus = kgsl_cmdstream_issueibcmds(param.device_id, param.drawctxt_index, param.ibaddr, param.sizedwords, &tmp, param.flags);
            if (kgslStatus == GSL_SUCCESS)
            {
		if (debugfs_noisy) {
            	    printk(KERN_INFO "@MF@ SUBMIT DONE OK ts=%d flags=%08x\n", tmp, param.flags);
		}

                if (copy_to_user(param.timestamp, &tmp, sizeof(gsl_timestamp_t)))
                {
                    printk(KERN_ERR "%s: copy_to_user error ts=%p\n", __func__, param.timestamp);
                    kgslStatus = GSL_FAILURE;
                    break;
                }
            } else {
            	    printk(KERN_ERR "@MF@ kgsl_cmdstream_issueibcmds FAILED (%d)\n", kgslStatus);
            }
            break;
        }
    case IOCTL_KGSL_CMDSTREAM_READTIMESTAMP:
        {
            kgsl_cmdstream_readtimestamp_t param;
            gsl_timestamp_t tmp;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_cmdstream_readtimestamp_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            tmp = kgsl_cmdstream_readtimestamp(param.device_id, param.type);
            if (copy_to_user(param.timestamp, &tmp, sizeof(gsl_timestamp_t)))
            {
                    printk(KERN_ERR "%s: copy_to_user error\n", __func__);
                    kgslStatus = GSL_FAILURE;
                    break;
            }
            kgslStatus = GSL_SUCCESS;
	    if (debugfs_noisy) {
                printk(KERN_INFO "@MF@ IOCTL_KGSL_CMDSTREAM_READTIMESTAMP ts=%d\n", tmp);
	    }
            break;
        }
    case IOCTL_KGSL_CMDSTREAM_FREEMEMONTIMESTAMP:
        {
            int err;
            kgsl_cmdstream_freememontimestamp_t param;
	    gsl_memdesc_t memdesc;

            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_cmdstream_freememontimestamp_t)) ||
		copy_from_user(&memdesc, (void __user *)param.memdesc, sizeof(gsl_memdesc_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
	    param.memdesc = &memdesc;

            printk(KERN_INFO "@MF@ IOCTL_KGSL_CMDSTREAM_FREEMEMONTIMESTAMP gpuaddr=%08x ts=%d\n",
            	    param.memdesc->gpuaddr,
            	    param.timestamp);

            err = del_memblock_from_allocated_list(fd, param.memdesc);
            if(err)
            {
                /* tried to remove a block of memory that is not allocated!
                 * NOTE that -EINVAL is Linux kernel's error codes!
                 * the drivers error codes COULD mix up with kernel's. */
                kgslStatus = -EINVAL;
            }
            else
            {
                kgslStatus = kgsl_cmdstream_freememontimestamp(param.device_id,
                                                               param.memdesc,
                                                               param.timestamp,
                                                               param.type);
            }
            break;
        }
    case IOCTL_KGSL_CMDSTREAM_WAITTIMESTAMP:
        {
            kgsl_cmdstream_waittimestamp_t param;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_cmdstream_waittimestamp_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_cmdstream_waittimestamp(param.device_id, param.timestamp, param.timeout);
	    if (debugfs_noisy) {
                printk(KERN_INFO "@MF@ IOCTL_KGSL_CMDSTREAM_WAITTIMESTAMP ts=%d to=%d st=%d\n",
                        param.timestamp, param.timeout, kgslStatus);
	    }
            break;
        }
    case IOCTL_KGSL_CMDWINDOW_WRITE:
        {
            kgsl_cmdwindow_write_t param;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_cmdwindow_write_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_cmdwindow_write(param.device_id, param.target, param.addr, param.data);
            break;
        }
    case IOCTL_KGSL_CONTEXT_CREATE:
        {
            kgsl_context_create_t param;
            unsigned int tmp;
            int tmpStatus;

            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_context_create_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_context_create(param.device_id, param.type, &tmp, param.flags);
            printk(KERN_INFO "@MF@ IOCTL_KGSL_CONTEXT_CREATE device=%d type=0x%x flags=0x%x\n",
            	    param.device_id, param.type, param.flags);

            if (kgslStatus == GSL_SUCCESS)
            {
                if (copy_to_user(param.drawctxt_id, &tmp, sizeof(unsigned int)))
                {
                    tmpStatus = kgsl_context_destroy(param.device_id, tmp);
                    /* is asserting ok? Basicly we should return the error from copy_to_user
                     * but will the user space interpret it correctly? Will the user space
                     * always check against GSL_SUCCESS  or GSL_FAILURE as they are not the only
                     * return values.
                     */
                    KOS_ASSERT(tmpStatus == GSL_SUCCESS);
                    printk(KERN_ERR "%s: copy_to_user error\n", __func__);
                    kgslStatus = GSL_FAILURE;
                    break;
                }
                else
                {
                    add_device_context_to_array(fd, param.device_id, tmp);
                }
            }
            break;
        }
    case IOCTL_KGSL_CONTEXT_DESTROY:
        {
            kgsl_context_destroy_t param;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_context_destroy_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_context_destroy(param.device_id, param.drawctxt_id);
            del_device_context_from_array(fd, param.device_id, param.drawctxt_id);
            break;
        }
    case IOCTL_KGSL_DRAWCTXT_BIND_GMEM_SHADOW:
        {
            kgsl_drawctxt_bind_gmem_shadow_t param;
            gsl_rect_t gmem_rect;
            gsl_buffer_desc_t shadow_buffer;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_drawctxt_bind_gmem_shadow_t)) ||
		copy_from_user(&gmem_rect, (void __user *)param.gmem_rect, sizeof(gsl_rect_t)) ||
		copy_from_user(&shadow_buffer, (void __user *)param.shadow_buffer, sizeof(gsl_buffer_desc_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
	    param.shadow_buffer = &shadow_buffer;
	    param.gmem_rect = &gmem_rect;
            kgslStatus = kgsl_drawctxt_bind_gmem_shadow(param.device_id, param.drawctxt_id, param.gmem_rect, param.shadow_x, param.shadow_y, param.shadow_buffer, param.buffer_id);
            break;
        }
    case IOCTL_KGSL_SHAREDMEM_ALLOC:
        {
            kgsl_sharedmem_alloc_t param;
            gsl_memdesc_t tmp;
            int tmpStatus;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_sharedmem_alloc_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_sharedmem_alloc(param.device_id, param.flags, param.sizebytes, &tmp);
            if (kgslStatus == GSL_SUCCESS)
            {
                if (copy_to_user(param.memdesc, &tmp, sizeof(gsl_memdesc_t)))
                {
                    tmpStatus = kgsl_sharedmem_free(&tmp);
                    KOS_ASSERT(tmpStatus == GSL_SUCCESS);
                    printk(KERN_ERR "%s: copy_to_user error\n", __func__);
                    kgslStatus = GSL_FAILURE;
                    break;
                }
                else
                {
                    printk(KERN_INFO "@MF@  alloc %x flags=%x => gpu=%x\n", param.sizebytes, param.flags, tmp.gpuaddr);
                    add_memblock_to_allocated_list(fd, &tmp);
                }
	    } else {
		    printk(KERN_ERR "GPU %s:%d kgsl_sharedmem_alloc failed!\n", __func__, __LINE__);
	    }
            break;
        }
    case IOCTL_KGSL_SHAREDMEM_FREE:
        {
            kgsl_sharedmem_free_t param;
            gsl_memdesc_t tmp;
            int err;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_sharedmem_free_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            if (copy_from_user(&tmp, (void __user *)param.memdesc, sizeof(gsl_memdesc_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            err = del_memblock_from_allocated_list(fd, &tmp);
            if(err)
            {
                printk(KERN_ERR "%s: tried to free memdesc that was not allocated!\n", __func__);
                kgslStatus = err;
                break;
            }
            kgslStatus = kgsl_sharedmem_free(&tmp);
            if (kgslStatus == GSL_SUCCESS)
            {
                if (copy_to_user(param.memdesc, &tmp, sizeof(gsl_memdesc_t)))
                {
                    printk(KERN_ERR "%s: copy_to_user error\n", __func__);
                    kgslStatus = GSL_FAILURE;
                    break;
                }
            }
            break;
        }
    case IOCTL_KGSL_SHAREDMEM_READ:
        {
            kgsl_sharedmem_read_t param;
            gsl_memdesc_t memdesc;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_sharedmem_read_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            if (copy_from_user(&memdesc, (void __user *)param.memdesc, sizeof(gsl_memdesc_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_sharedmem_read(&memdesc, param.dst, param.offsetbytes, param.sizebytes, true);
            if (kgslStatus != GSL_SUCCESS)
            {
                printk(KERN_ERR "%s: kgsl_sharedmem_read failed\n", __func__);
            }
            break;
        }
    case IOCTL_KGSL_SHAREDMEM_WRITE:
        {
            kgsl_sharedmem_write_t param;
            gsl_memdesc_t memdesc;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_sharedmem_write_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            if (copy_from_user(&memdesc, (void __user *)param.memdesc, sizeof(gsl_memdesc_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_sharedmem_write(&memdesc, param.offsetbytes, param.src, param.sizebytes, true);
            if (kgslStatus != GSL_SUCCESS)
            {
                printk(KERN_ERR "%s: kgsl_sharedmem_write failed\n", __func__);
            }

            break;
        }
    case IOCTL_KGSL_SHAREDMEM_SET:
        {
            kgsl_sharedmem_set_t param;
            gsl_memdesc_t memdesc;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_sharedmem_set_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            if (copy_from_user(&memdesc, (void __user *)param.memdesc, sizeof(gsl_memdesc_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_sharedmem_set(&memdesc, param.offsetbytes, param.value, param.sizebytes);
            break;
        }
    case IOCTL_KGSL_SHAREDMEM_LARGESTFREEBLOCK:
        {
            kgsl_sharedmem_largestfreeblock_t param;
            unsigned int largestfreeblock;

            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_sharedmem_largestfreeblock_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            largestfreeblock = kgsl_sharedmem_largestfreeblock(param.device_id, param.flags);
            if (copy_to_user(param.largestfreeblock, &largestfreeblock, sizeof(unsigned int)))
            {
                printk(KERN_ERR "%s: copy_to_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = GSL_SUCCESS;
            break;
        }
    case IOCTL_KGSL_SHAREDMEM_CACHEOPERATION:
        {
            kgsl_sharedmem_cacheoperation_t param;
            gsl_memdesc_t memdesc;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_sharedmem_cacheoperation_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            if (copy_from_user(&memdesc, (void __user *)param.memdesc, sizeof(gsl_memdesc_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_sharedmem_cacheoperation(&memdesc, param.offsetbytes, param.sizebytes, param.operation);
            break;
        }
    case IOCTL_KGSL_SHAREDMEM_FROMHOSTPOINTER:
        {
            kgsl_sharedmem_fromhostpointer_t param;
            gsl_memdesc_t memdesc;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_sharedmem_fromhostpointer_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            if (copy_from_user(&memdesc, (void __user *)param.memdesc, sizeof(gsl_memdesc_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_sharedmem_fromhostpointer(param.device_id, &memdesc, param.hostptr);
            break;
        }
    case IOCTL_KGSL_ADD_TIMESTAMP:
        {
            kgsl_add_timestamp_t param;
            gsl_timestamp_t tmp;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_add_timestamp_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            tmp = kgsl_add_timestamp(param.device_id, &tmp);
            if (copy_to_user(param.timestamp, &tmp, sizeof(gsl_timestamp_t)))
            {
                    printk(KERN_ERR "%s: copy_to_user error\n", __func__);
                    kgslStatus = GSL_FAILURE;
                    break;
            }
            kgslStatus = GSL_SUCCESS;
            break;
        }

    case IOCTL_KGSL_DEVICE_CLOCK:
        {
            kgsl_device_clock_t param;
            if (copy_from_user(&param, (void __user *)arg, sizeof(kgsl_device_clock_t)))
            {
                printk(KERN_ERR "%s: copy_from_user error\n", __func__);
                kgslStatus = GSL_FAILURE;
                break;
            }
            kgslStatus = kgsl_device_clock(param.device, param.enable);
            break;
        }
    default:
        kgslStatus = -ENOTTY;
        break;
    }

    return kgslStatus;
}

static int gsl_kmod_mmap(struct file *fd, struct vm_area_struct *vma)
{
    int status = 0;
    unsigned long start = vma->vm_start;
    unsigned long pfn = vma->vm_pgoff;
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long prot = pgprot_writecombine(vma->vm_page_prot);
    unsigned long addr = vma->vm_pgoff << PAGE_SHIFT;
    void *va = NULL;

    printk(KERN_INFO "@MF@ %s(%08x size=%08x\n", __func__, addr, size);

    if (gsl_driver.enable_mmu && (addr < GSL_LINUX_MAP_RANGE_END) && (addr >= GSL_LINUX_MAP_RANGE_START)) {
	va = gsl_linux_map_find(addr);
	while (size > 0) {
	    if (remap_pfn_range(vma, start, vmalloc_to_pfn(va), PAGE_SIZE, prot)) {
		return -EAGAIN;
	    }
	    start += PAGE_SIZE;
	    va += PAGE_SIZE;
	    size -= PAGE_SIZE;
	}
    } else {
	if (remap_pfn_range(vma, start, pfn, size, prot)) {
	    status = -EAGAIN;
	}
    }

    vma->vm_ops = &gsl_kmod_vmops;

    return status;
}

static int gsl_kmod_fault(struct vm_fault *vmf)
{
    return VM_FAULT_SIGBUS;
}

static struct platform_device *mf_plat_dev;
static int imxgpu_clear_gmem(struct platform_device *dev);

static int gsl_kmod_open(struct inode *inode, struct file *fd)
{
    gsl_flags_t flags = 0;
    struct gsl_kmod_per_fd_data *datp;
    int err = 0;

    printk(KERN_INFO "@MF@ %s\n", __func__);
    if(mutex_lock_interruptible(&gsl_mutex))
    {
        return -EINTR;
    }

    if (kgsl_driver_entry(flags) != GSL_SUCCESS)
    {
        printk(KERN_INFO "%s: kgsl_driver_entry error\n", __func__);
        err = -EIO;  // TODO: not sure why did it fail?
    }
    else
    {
        /* allocate per file descriptor data structure */
        datp = (struct gsl_kmod_per_fd_data *)kzalloc(
                                             sizeof(struct gsl_kmod_per_fd_data),
                                             GFP_KERNEL);
        if(datp)
        {
            init_created_contexts_array(datp->created_contexts_array[0]);
            INIT_LIST_HEAD(&datp->allocated_blocks_head);

            fd->private_data = (void *)datp;
        }
        else
        {
            err = -ENOMEM;
        }
    }

    imxgpu_clear_gmem(mf_plat_dev);

    mutex_unlock(&gsl_mutex);

    return err;
}

static int gsl_kmod_release(struct inode *inode, struct file *fd)
{
    struct gsl_kmod_per_fd_data *datp;
    int err = 0;

    if(mutex_lock_interruptible(&gsl_mutex))
    {
        return -EINTR;
    }

    /* make sure contexts are destroyed */
    del_all_devices_contexts(fd);

    if (kgsl_driver_exit() != GSL_SUCCESS)
    {
        printk(KERN_INFO "%s: kgsl_driver_exit error\n", __func__);
        err = -EIO; // TODO: find better error code
    }
    else
    {
        /* release per file descriptor data structure */
        datp = (struct gsl_kmod_per_fd_data *)fd->private_data;
        del_all_memblocks_from_allocated_list(fd);
        kfree(datp);
        fd->private_data = 0;
    }

    mutex_unlock(&gsl_mutex);

    return err;
}

static struct class *gsl_kmod_class;

static irqreturn_t z160_irq_handler(int irq, void *dev_id)
{
    kgsl_intr_isr(&gsl_driver.device[GSL_DEVICE_G12-1]);
    return IRQ_HANDLED;
}

static irqreturn_t z430_irq_handler(int irq, void *dev_id)
{
    if (debugfs_noisy) {
	printk(KERN_INFO "@MF@ kgsl 3D IRQ\n");
    }
    kgsl_intr_isr(&gsl_driver.device[GSL_DEVICE_YAMATO-1]);
    return IRQ_HANDLED;
}

#if CONFIG_OF
static struct mxc_gpu_platform_data* gpu_parse_dt(struct device *dev)
{
	struct mxc_gpu_platform_data *pdata;
	struct device_node *np = dev->of_node;
	u32 param_z160_revision = 1;
	u32 param_enable_mmu = 0;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return pdata;

	of_property_read_u32(np, "z160-revision", &param_z160_revision);
	of_property_read_u32(np, "enable-mmu", &param_enable_mmu);

	pdata->enable_mmu = param_enable_mmu;
	pdata->z160_revision = param_z160_revision;
	return pdata;
}
#else
static struct mxc_gpu_platform_data* gpu_parse_dt(struct device *dev)
{
	return NULL;
}
#endif

struct imxgpu_gmem_ctx {
	struct platform_device *dev;
	struct resource	*res;
	void __iomem 	*gmem;
};

static struct imxgpu_gmem_ctx *imxgpu_gmem_open(struct platform_device *dev)
{
	struct 	imxgpu_gmem_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ctx->res = platform_get_resource_byname(dev, IORESOURCE_MEM, "gpu_graphics_mem");
	if (!ctx->res) {
		dev_err(&dev->dev, "No gmem defined in DT %p '%s'\n", dev, dev->name);
		ret = -ENODEV;
		goto out_err;
	}
	ctx->gmem = ioremap_nocache(ctx->res->start, resource_size(ctx->res));
	if (!ctx->gmem) {
		dev_err(&dev->dev, "Unable to map gmem\n");
		ret = -ENOMEM;
		goto out_err;
	}

	ctx->dev = dev;

	dev_info(&dev->dev, "@MF@ mapped gmem %08x len=%08x\n", ctx->res->start, resource_size(ctx->res));
	kgsl_clock(GSL_DEVICE_YAMATO, 1);

	return ctx;

out_err:
	kfree(ctx);

	return ERR_PTR(ret);
}

static void imxgpu_gmem_close(struct imxgpu_gmem_ctx *ctx)
{
	dev_info(&ctx->dev->dev, "@MF@ unmapping GEMEM\n");
	iounmap(ctx->gmem);
	kfree(ctx);

	kgsl_clock(GSL_DEVICE_YAMATO, 0);
}

static int gmem_open(struct inode *inode, struct file *file)
{
	struct platform_device *dev = inode->i_private;
	struct imxgpu_gmem_ctx *ctx;

	ctx = imxgpu_gmem_open(dev);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	file->private_data = ctx;

	return 0;
}

static int gmem_release(struct inode *inode, struct file *file)
{
	struct imxgpu_gmem_ctx *ctx = file->private_data;

	imxgpu_gmem_close(ctx);

	return 0;
}

static ssize_t gmem_read(struct file *file, char __user *user_buf,
			       size_t count, loff_t *ppos)
{
	struct imxgpu_gmem_ctx *ctx = file->private_data;
	loff_t pos = *ppos;
	u32 val;
	unsigned long remain;
	size_t i;

	printk(KERN_INFO "@MF@ %s pos=%ld\n", __func__, (long)pos);

	if (pos < 0)
		return -EINVAL;

	if (pos >= resource_size(ctx->res))
		return 0;

	if (count > resource_size(ctx->res) - pos)
		count = resource_size(ctx->res) - pos;

	count &= ~3;

	for (i=0; i < count; i += sizeof(val)) {
		val = readl(ctx->gmem + pos);
		remain = copy_to_user(user_buf, &val, sizeof(val));
		if (remain)
			return -EFAULT;
		user_buf += sizeof(val);
		pos += sizeof(val);
	}

	*ppos = pos;

	return count;
}

static const struct file_operations fops_gmem = {
	.open		= gmem_open,
	.release	= gmem_release,
	.read		= gmem_read,
	.llseek		= default_llseek,
};


static int imxgpu_debugfs_gmem_init(struct platform_device *dev)
{
	struct dentry *dentry, *dir;

	dir = debugfs_create_dir("kgsl", NULL);
	if (!dir)
		return -ENODEV;

	dentry = debugfs_create_file("gmem", S_IRUGO,
					dir,
					dev, &fops_gmem);

	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	dentry = debugfs_create_bool("dumpcmdbufs", S_IRUGO|S_IWUSR,
				       dir, &debugfs_dumpcmdbufs);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	dentry = debugfs_create_bool("noisy", S_IRUGO|S_IWUSR,
				       dir, &debugfs_noisy);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	return 0;
}

static int imxgpu_clear_gmem(struct platform_device *dev)
{
	struct imxgpu_gmem_ctx *ctx;
	int i;

	ctx = imxgpu_gmem_open(dev);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	for (i=0; i < resource_size(ctx->res); i += 4)
		writel(0, ctx->gmem + i);

	imxgpu_gmem_close(ctx);

	return 0;
}


static int gpu_probe(struct platform_device *pdev)
{
    int i;
    struct resource *res;
    struct device *dev;
    struct mxc_gpu_platform_data *pdata;

    pdata = pdev->dev.platform_data;
    if (!pdata)
	pdata = gpu_parse_dt(&pdev->dev);

    if (pdata) {
	z160_version = pdata->z160_revision;
	gpu_reserved_mem = pdata->reserved_mem_base;
	gpu_reserved_mem_size = pdata->reserved_mem_size;
	enable_mmu = pdata->enable_mmu;
    }
    enable_mmu = 0;
    //gpu_reserved_mem = 0xc8000000;
    //gpu_reserved_mem_size = 128*1024*1024;
    debugfs_dumpcmdbufs = 0;
    debugfs_noisy = 0;

    for(i = 0; i < 2; i++){
        res = platform_get_resource(pdev, IORESOURCE_IRQ, i);
        if (!res) {
            if (i == 0) {
                printk(KERN_ERR "gpu: unable to get gpu irq\n");
                return -ENODEV;
            } else {
                break;
            }
        }
        if(strcmp(res->name, "gpu_2d_irq") == 0){
            gpu_2d_irq = res->start;
        }else if(strcmp(res->name, "gpu_3d_irq") == 0){
            gpu_3d_irq = res->start;
        }
    }

    for (i = 0; i < 3; i++) {
        res = platform_get_resource(pdev, IORESOURCE_MEM, i);
        if (!res) {
            gpu_2d_regbase = 0;
            gpu_2d_regsize = 0;
            gpu_3d_regbase = 0;
            gpu_2d_regsize = 0;
            gmem_size = 0;
            gpu_reserved_mem = 0;
            gpu_reserved_mem_size = 0;
            break;
        }else{
            if(strcmp(res->name, "gpu_2d_registers") == 0){
                gpu_2d_regbase = res->start;
                gpu_2d_regsize = res->end - res->start + 1;
            }else if(strcmp(res->name, "gpu_3d_registers") == 0){
                gpu_3d_regbase = res->start;
                gpu_3d_regsize = res->end - res->start + 1;
            }else if(strcmp(res->name, "gpu_graphics_mem") == 0){
                gmem_size = res->end - res->start + 1;
            }
        }
    }

    if (gpu_3d_irq > 0)
    {
	if (request_irq(gpu_3d_irq, z430_irq_handler, 0, "ydx", NULL) < 0) {
	    printk(KERN_ERR "%s: request_irq error\n", __func__);
	    gpu_3d_irq = 0;
	    goto request_irq_error;
	}
    }

    if (gpu_2d_irq > 0)
    {
	if (request_irq(gpu_2d_irq, z160_irq_handler, 0, "g12", NULL) < 0) {
	    printk(KERN_ERR "DO NOT use uio_pdrv_genirq kernel module for X acceleration!\n");
	    gpu_2d_irq = 0;
	}
    }

    if (kgsl_driver_init() != GSL_SUCCESS) {
	printk(KERN_ERR "%s: kgsl_driver_init error\n", __func__);
	goto kgsl_driver_init_error;
    }
    gsl_driver.osdep_dev = &pdev->dev;

    gsl_kmod_major = register_chrdev(0, "gsl_kmod", &gsl_kmod_fops);
    gsl_kmod_vmops.fault = gsl_kmod_fault;

    if (gsl_kmod_major <= 0)
    {
        pr_err("%s: register_chrdev error\n", __func__);
        goto register_chrdev_error;
    }

    gsl_kmod_class = class_create(THIS_MODULE, "gsl_kmod");

    if (IS_ERR(gsl_kmod_class))
    {
        pr_err("%s: class_create error\n", __func__);
        goto class_create_error;
    }

    #if(LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28))
        dev = device_create(gsl_kmod_class, NULL, MKDEV(gsl_kmod_major, 0), "gsl_kmod");
    #else
        dev = device_create(gsl_kmod_class, NULL, MKDEV(gsl_kmod_major, 0), NULL,"gsl_kmod");
    #endif

    if (!IS_ERR(dev))
    {
    //    gsl_kmod_data.device = dev;
    	mf_plat_dev = pdev;
    	imxgpu_clear_gmem(pdev);
    	imxgpu_debugfs_gmem_init(pdev);

        return 0;
    }

    pr_err("%s: device_create error\n", __func__);

class_create_error:
    class_destroy(gsl_kmod_class);

register_chrdev_error:
    unregister_chrdev(gsl_kmod_major, "gsl_kmod");

kgsl_driver_init_error:
    kgsl_driver_close();
    if (gpu_2d_irq > 0) {
	free_irq(gpu_2d_irq, NULL);
    }
    if (gpu_3d_irq > 0) {
	free_irq(gpu_3d_irq, NULL);
    }
request_irq_error:
    return 0;   // TODO: return proper error code
}

static int gpu_remove(struct platform_device *pdev)
{
    device_destroy(gsl_kmod_class, MKDEV(gsl_kmod_major, 0));
    class_destroy(gsl_kmod_class);
    unregister_chrdev(gsl_kmod_major, "gsl_kmod");

    if (gpu_3d_irq)
    {
        free_irq(gpu_3d_irq, NULL);
    }

    if (gpu_2d_irq)
    {
        free_irq(gpu_2d_irq, NULL);
    }

    kgsl_driver_close();
    return 0;
}

#ifdef CONFIG_PM
static int gpu_suspend(struct platform_device *pdev, pm_message_t state)
{
    int              i;
    gsl_powerprop_t  power;

    power.flags = GSL_PWRFLAGS_POWER_OFF;
    for (i = 0; i < GSL_DEVICE_MAX; i++)
    {
        kgsl_device_setproperty(
                        (gsl_deviceid_t) (i+1),
                        GSL_PROP_DEVICE_POWER,
                        &power,
                        sizeof(gsl_powerprop_t));
    }

    return 0;
}

static int gpu_resume(struct platform_device *pdev)
{
    int              i;
    gsl_powerprop_t  power;

    power.flags = GSL_PWRFLAGS_POWER_ON;
    for (i = 0; i < GSL_DEVICE_MAX; i++)
    {
        kgsl_device_setproperty(
                        (gsl_deviceid_t) (i+1),
                        GSL_PROP_DEVICE_POWER,
                        &power,
                        sizeof(gsl_powerprop_t));
    }

    return 0;
}
#else
#define	gpu_suspend	NULL
#define	gpu_resume	NULL
#endif /* !CONFIG_PM */

/*! Driver definition
 */
#ifdef CONFIG_OF
static const struct of_device_id mxc_gpu_ids[] = {
	{ .compatible = "fsl,imx53-gpu" },
	{ /* sentinel */ }
};
#endif

static struct platform_driver gpu_driver = {
    .driver = {
        .name = "mxc_gpu",
#ifdef CONFIG_OF
	.of_match_table = mxc_gpu_ids,
#endif

        },
    .probe = gpu_probe,
    .remove = gpu_remove,
    .suspend = gpu_suspend,
    .resume = gpu_resume,
};

static int __init gsl_kmod_init(void)
{
     return platform_driver_register(&gpu_driver);
}

static void __exit gsl_kmod_exit(void)
{
     platform_driver_unregister(&gpu_driver);
}

module_init(gsl_kmod_init);
module_exit(gsl_kmod_exit);
MODULE_AUTHOR("Advanced Micro Devices");
MODULE_DESCRIPTION("AMD graphics core driver for i.MX");
MODULE_LICENSE("GPL v2");
