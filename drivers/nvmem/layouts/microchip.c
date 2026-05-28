// SPDX-License-Identifier: GPL-2.0-only

#include <linux/etherdevice.h>
#include <linux/nvmem-consumer.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>

struct mfg_otp_fields {
	u32 otp_prg;
	u8 feat_ids;
	u16 partid;
	u8 tst_trk;
	u64 serial_number;
} __packed;

#define OTP_TAG_OFFSET		4096
#define OTP_TAG_ENTRY_LENGTH	8
#define OTP_TAG_VAL_LENGTH	6

static u8 otp_tag_get_size(const u8 *tag)
{
	return tag[7] / 32;
}

static u8 otp_tag_get_cont(const u8 *tag)
{
	return (tag[7] / 16) & 0x1;
}

static u8 otp_tag_get_tag(const u8 *tag)
{
	return (tag[7] & 0x4) | tag[6];
}

static bool otp_tag_valid(const u8 *tag_raw)
{
	u8 size;

	size = otp_tag_get_size(tag_raw);
	if (size != 0 && size != 7)
		return true;

	return false;
}

static const char *microchip_tag_cell_name(u8 type)
{
	switch (type) {
	case 1:
		return "password";
	case 2:
		return "pcb";
	case 3:
		return "revision";
	case 4:
		return "base-mac-address";
	case 5:
		return "mac-address-count";
	case 6:
		return "fit-config";
	case 7:
		return "pcb2";
	default:
		break;
	}

	return NULL;
}

static int microchip_tag_read_cb(void *priv, const char *id, int index,
				 unsigned int offset, void *buf,
				 size_t bytes)
{
	u8 tag_type = (uintptr_t)priv;
	u8 *src = buf;
	u8 *dst = buf;
	size_t pos;
	int i;

	for (pos = 0; pos < bytes; pos += OTP_TAG_ENTRY_LENGTH) {
		u8 size = otp_tag_get_size(src + pos);

		memmove(dst, src + pos, size);
		dst += size;
	}

	if (tag_type == 4) {
		for (i = 0; i < ETH_ALEN / 2; i++)
			swap(src[i], src[ETH_ALEN - 1 - i]);

		eth_addr_add(buf, index);
	}

	return 0;
}

static int microchip_tag_parse(struct device *dev, u8 *data, size_t dev_size,
			       unsigned int offset, unsigned int *raw_len,
			       unsigned int *bytes)
{
	u8 *tag_raw = data + offset;
	u8 tag_type = otp_tag_get_tag(tag_raw);
	u8 size;

	*raw_len = 0;
	*bytes = 0;

	do {
		size = otp_tag_get_size(tag_raw);
		*raw_len += OTP_TAG_ENTRY_LENGTH;
		*bytes += size;

		if (!otp_tag_get_cont(tag_raw))
			return 0;

		offset += OTP_TAG_ENTRY_LENGTH;
		if (offset > dev_size - OTP_TAG_ENTRY_LENGTH) {
			dev_warn(dev, "Ignoring truncated tag at 0x%x\n", offset);
			return -EINVAL;
		}

		tag_raw = data + offset;
		if (!otp_tag_valid(tag_raw) ||
		    otp_tag_get_tag(tag_raw) != tag_type) {
			dev_warn(dev, "Ignoring malformed continuation tag at 0x%x\n",
				 offset);
			return -EINVAL;
		}
	} while (true);
}

static const struct nvmem_cell_info microchip_otp_entries[] = {
	{
		.name = "partid",
		.offset = offsetof(struct mfg_otp_fields, partid),
		.bytes = sizeof_field(struct mfg_otp_fields, partid),
	},
	{
		.name = "feat-ids",
		.offset = offsetof(struct mfg_otp_fields, feat_ids),
		.bytes = sizeof_field(struct mfg_otp_fields, feat_ids),
	},
	{
		.name = "serial-number",
		.offset = offsetof(struct mfg_otp_fields, serial_number),
		.bytes = sizeof_field(struct mfg_otp_fields, serial_number),
	},
};

static int microchip_add_static_cells(struct nvmem_layout *layout)
{
	struct nvmem_device *nvmem = layout->nvmem;
	const struct nvmem_cell_info *pinfo;
	struct nvmem_cell_info info = {0};
	struct device_node *layout_np;
	int ret, i;

	layout_np = of_nvmem_layout_get_container(nvmem);
	if (!layout_np)
		return -ENOENT;

	/* First register fixed fields */
	for (i = 0; i < ARRAY_SIZE(microchip_otp_entries); i++) {
		pinfo = &microchip_otp_entries[i];

		info.name = pinfo->name;
		info.offset = pinfo->offset;
		info.bytes = pinfo->bytes;
		info.read_post_process = pinfo->read_post_process;
		info.np = of_get_child_by_name(layout_np, pinfo->name);

		ret = nvmem_add_one_cell(nvmem, &info);
		if (ret) {
			of_node_put(layout_np);
			return ret;
		}
	}

	of_node_put(layout_np);

	return 0;
}

static int microchip_add_tag_cells(struct nvmem_layout *layout)
{
	struct nvmem_device *nvmem = layout->nvmem;
	unsigned int offset = OTP_TAG_OFFSET;
	u8 tag_raw[OTP_TAG_ENTRY_LENGTH];
	struct device *dev = &layout->dev;
	struct nvmem_cell_info cell = {0};
	struct device_node *layout_np;
	unsigned int raw_len;
	unsigned int bytes;
	u8 tag_type;
	size_t dev_size;
	u8 *data;
	int ret;

	layout_np = of_nvmem_layout_get_container(nvmem);
	if (!layout_np)
		return -ENOENT;

	dev_size = nvmem_dev_size(nvmem);
	data = devm_kmalloc(dev, dev_size, GFP_KERNEL);
	if (!data) {
		of_node_put(layout_np);
		return -ENOMEM;
	}

	ret = nvmem_device_read(nvmem, 0, dev_size, data);
	if (ret < 0) {
		of_node_put(layout_np);
		return ret;
	} else if (ret != dev_size) {
		of_node_put(layout_np);
		return -EIO;
	}

	while (offset + OTP_TAG_ENTRY_LENGTH <= dev_size) {
		memcpy(tag_raw, data + offset, sizeof(tag_raw));

		if (otp_tag_valid(tag_raw)) {
			tag_type = otp_tag_get_tag(tag_raw);
			cell.name = microchip_tag_cell_name(tag_type);
			if (!cell.name)
				goto next;

			ret = microchip_tag_parse(dev, data, dev_size, offset,
						  &raw_len, &bytes);
			if (ret)
				goto next;

			cell.offset = offset;
			cell.raw_len = raw_len;
			cell.bytes = bytes;
			cell.np = of_get_child_by_name(layout_np, cell.name);
			cell.read_post_process = microchip_tag_read_cb;
			cell.priv = (void *)(uintptr_t)tag_type;

			ret = nvmem_add_one_cell(nvmem, &cell);
			if (ret) {
				of_node_put(layout_np);
				return ret;
			}

			offset += raw_len;
			continue;
		}

next:
		offset += OTP_TAG_ENTRY_LENGTH;
	}

	of_node_put(layout_np);

	return 0;
}

static int microchip_add_cells(struct nvmem_layout *layout)
{
	int ret;

	ret = microchip_add_static_cells(layout);
	if (ret)
		return ret;

	ret = microchip_add_tag_cells(layout);
	if (ret)
		return ret;

	return ret;
}

static int microchip_probe(struct nvmem_layout *layout)
{
	layout->add_cells = microchip_add_cells;

	return nvmem_layout_register(layout);
}

static void microchip_remove(struct nvmem_layout *layout)
{
	nvmem_layout_unregister(layout);
}

static const struct of_device_id microchip_of_match_table[] = {
	{ .compatible = "microchip,otp-layout" },
	{},
};
MODULE_DEVICE_TABLE(of, microchip_of_match_table);

static struct nvmem_layout_driver microchip_layout = {
	.driver = {
		.name = "microchip-layout",
		.of_match_table = microchip_of_match_table,
	},
	.probe = microchip_probe,
	.remove = microchip_remove,
};
module_nvmem_layout_driver(microchip_layout);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Robert Marko <robert.marko@sartura.hr>");
MODULE_DESCRIPTION("NVMEM layout driver for Microchip LAN9xxx switches");
