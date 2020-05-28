#include "serial_fifo.h"
#include "assert.h"
#include "mpu_ops.h"
#include "cpuemu_ops.h"
#include "device.h"

static AthrillSerialFifoType athrill_serial_fifo[SERIAL_FIFO_MAX_CHANNEL_NUM];
static uint32 serial_fifo_base_addr = 0x0;

static char serial_fifo_param_buffer[256];
void athrill_device_init_serial_fifo(void)
{
	uint32 i;
	uint32 buffer_size;
	Std_ReturnType ret;

	ret = cpuemu_get_devcfg_value_hex("DEVICE_CONFIG_SERIAL_FILFO_BASE_ADDR", &serial_fifo_base_addr);
	if (ret != STD_E_OK) {
		return;
	}
	printf("DEVICE_CONFIG_SERIAL_FILFO_BASE_ADDR=0x%x\n", serial_fifo_base_addr);
	for (i = 0; i < SERIAL_FIFO_MAX_CHANNEL_NUM; i++) {
		memset(serial_fifo_param_buffer, 0, sizeof(serial_fifo_param_buffer));
		snprintf(serial_fifo_param_buffer, sizeof(serial_fifo_param_buffer), "DEVICE_CONFIG_SERIAL_FILFO_%d_SIZE", i);
		ret = cpuemu_get_devcfg_value(serial_fifo_param_buffer, &buffer_size);
		if (ret == STD_E_OK) {
			printf("%s=%u\n", serial_fifo_param_buffer, buffer_size);
			ret = comm_fifo_buffer_create(buffer_size, &athrill_serial_fifo[i].rd);
			ASSERT(ret == STD_E_OK);
			ret = comm_fifo_buffer_create(SERIAL_FIFO_RD_BUFFER_LEN, &athrill_serial_fifo[i].rd_dev_buffer);
			ASSERT(ret == STD_E_OK);

			ret = comm_fifo_buffer_create(buffer_size, &athrill_serial_fifo[i].wr);
			ASSERT(ret == STD_E_OK);
			ret = comm_fifo_buffer_create(SERIAL_FIFO_WR_BUFFER_LEN, &athrill_serial_fifo[i].wr_dev_buffer);
			ASSERT(ret == STD_E_OK);

			snprintf(serial_fifo_param_buffer, sizeof(serial_fifo_param_buffer), "DEVICE_CONFIG_SERIAL_FILFO_%d_RD_INTNO", i);
			ret = cpuemu_get_devcfg_value(serial_fifo_param_buffer, &athrill_serial_fifo[i].rd_intno);
			ASSERT(ret == STD_E_OK);
			printf("%s=%u\n", serial_fifo_param_buffer, athrill_serial_fifo[i].rd_intno);
			snprintf(serial_fifo_param_buffer, sizeof(serial_fifo_param_buffer), "DEVICE_CONFIG_SERIAL_FILFO_%d_WR_INTNO", i);
			ret = cpuemu_get_devcfg_value(serial_fifo_param_buffer, &athrill_serial_fifo[i].wr_intno);
			ASSERT(ret == STD_E_OK);
			printf("%s=%u\n", serial_fifo_param_buffer, athrill_serial_fifo[i].wr_intno);
			snprintf(serial_fifo_param_buffer, sizeof(serial_fifo_param_buffer), "DEVICE_CONFIG_SERIAL_FILFO_%d_RD_INTOFF", i);
			ret = cpuemu_get_devcfg_value(serial_fifo_param_buffer, &athrill_serial_fifo[i].rd_intoff);
			ASSERT(ret == STD_E_OK);
			printf("%s=%u\n", serial_fifo_param_buffer, athrill_serial_fifo[i].rd_intoff);
			snprintf(serial_fifo_param_buffer, sizeof(serial_fifo_param_buffer), "DEVICE_CONFIG_SERIAL_FILFO_%d_WR_INTOFF", i);
			ret = cpuemu_get_devcfg_value(serial_fifo_param_buffer, &athrill_serial_fifo[i].wr_intoff);
			ASSERT(ret == STD_E_OK);
			printf("%s=%u\n", serial_fifo_param_buffer, athrill_serial_fifo[i].wr_intoff);
		}
		else {
			athrill_serial_fifo[i].rd.data = NULL;
			athrill_serial_fifo[i].wr.data = NULL;
		}
	}
	return;
}

static void do_serial_fifo_cpu_read(uint32 channel)
{
	Std_ReturnType err;
	uint8 data;
	uint8 cmd;
	uint32 res;

	/*
	 * status
	 */
	if (COMM_FIFO_IS_EMPTY(&athrill_serial_fifo[channel].rd)) {
		//no data found
		mpu_put_data8(0U, SERIAL_FIFO_READ_STATUS_ADDR(serial_fifo_base_addr, channel), SERIAL_FIFO_READ_STATUS_NO_DATA);
	}
	else {
		//found data
		mpu_put_data8(0U, SERIAL_FIFO_READ_STATUS_ADDR(serial_fifo_base_addr, channel), SERIAL_FIFO_READ_STATUS_DATA_IN);
		if (athrill_serial_fifo[channel].rd.count >= athrill_serial_fifo[channel].rd_intoff) {
			device_raise_int(athrill_serial_fifo[channel].rd_intno);
		}
	}
	/*
	 * check command and move data
	 */
	mpu_get_data8(0U, SERIAL_FIFO_READ_CMD_ADDR(serial_fifo_base_addr, channel), &cmd);
	if (cmd == SERIAL_FIFO_READ_CMD_MOVE) {
		err = comm_fifo_buffer_get(&athrill_serial_fifo[channel].rd, (char*)&data, 1, &res);
		if (err == STD_E_OK) {
			mpu_put_data8(0U, SERIAL_FIFO_READ_PTR_ADDR(serial_fifo_base_addr, channel), data);
		}
		mpu_put_data8(0U, SERIAL_FIFO_READ_CMD_ADDR(serial_fifo_base_addr, channel), SERIAL_FIFO_READ_CMD_NONE);
	}

	return;
}
static void do_serial_fifo_cpu_write_tx(uint32 channel)
{
	Std_ReturnType err;
	uint32 i;
	uint8 data;
	uint32 res;

	for (i = 0; i < athrill_serial_fifo[channel].wr_dev_buffer.count; i++) {
		if (COMM_FIFO_IS_FULL(&athrill_serial_fifo[channel].wr)) {
			break;
		}
		err = comm_fifo_buffer_get(&athrill_serial_fifo[channel].wr_dev_buffer, (char*)&data, 1, &res);
		if (err == STD_E_OK) {
			(void)comm_fifo_buffer_add(&athrill_serial_fifo[channel].wr, (const char*)&data, 1, &res);
		}
		else {
			break;
		}
	}
	if (athrill_serial_fifo[channel].wr_dev_buffer.count <= athrill_serial_fifo[channel].wr_intoff) {
		device_raise_int(athrill_serial_fifo[channel].wr_intno);
	}
	if (athrill_serial_fifo[channel].wr_dev_buffer.count == 0) {
		mpu_put_data8(0U, SERIAL_FIFO_WRITE_CMD_ADDR(serial_fifo_base_addr, channel), SERIAL_FIFO_WRITE_CMD_NONE);
	}
	return;
}

static void do_serial_fifo_cpu_write(uint32 channel)
{
	uint8 data;
	uint8 cmd;
	uint32 res;
	/*
	 * status
	 */
	if (!COMM_FIFO_IS_FULL(&athrill_serial_fifo[channel].wr_dev_buffer)) {
		mpu_put_data8(0U, SERIAL_FIFO_WRITE_STATUS_ADDR(serial_fifo_base_addr, channel), SERIAL_FIFO_WRITE_STATUS_CAN_DATA);
	}
	else {
		mpu_put_data8(0U, SERIAL_FIFO_WRITE_STATUS_ADDR(serial_fifo_base_addr, channel), SERIAL_FIFO_WRITE_STATUS_DATA_FULL);
	}
	/*
	 * check command and move data
	 */
	mpu_get_data8(0U, SERIAL_FIFO_WRITE_CMD_ADDR(serial_fifo_base_addr, channel), &cmd);
	if (cmd == SERIAL_FIFO_WRITE_CMD_MOVE) {
		mpu_get_data8(0U, SERIAL_FIFO_WRITE_PTR_ADDR(serial_fifo_base_addr, channel), &data);
		(void)comm_fifo_buffer_add(&athrill_serial_fifo[channel].wr_dev_buffer, (const char*)&data, 1, &res);
		mpu_put_data8(0U, SERIAL_FIFO_WRITE_CMD_ADDR(serial_fifo_base_addr, channel), SERIAL_FIFO_WRITE_CMD_NONE);
	}
	else if (cmd == SERIAL_FIFO_WRITE_CMD_TX) {
		do_serial_fifo_cpu_write_tx(channel);
	}
	return;
}

void athrill_device_supply_clock_serial_fifo(DeviceClockType *dev_clock)
{
	uint32 i;
	if (serial_fifo_base_addr == 0x0) {
		return;
	}
	for (i = 0; i < SERIAL_FIFO_MAX_CHANNEL_NUM; i++) {
		if (athrill_serial_fifo[i].rd.data == NULL) {
			continue;
		}
		do_serial_fifo_cpu_read(i);
		do_serial_fifo_cpu_write(i);
	}
	return;
}

void athrill_device_get_serial_fifo_buffer(uint32 channel, AthrillSerialFifoType **serial_fifop)
{
	*serial_fifop = NULL;
	if (serial_fifo_base_addr == 0x0) {
		return;
	}
	if (channel >= SERIAL_FIFO_MAX_CHANNEL_NUM) {
		return;
	}
	if (athrill_serial_fifo[channel].rd.data == NULL) {
		return;
	}
	*serial_fifop = &athrill_serial_fifo[channel];
	return;
}


