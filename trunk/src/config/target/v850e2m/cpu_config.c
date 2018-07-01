#include "cpu.h"
#include "bus.h"
#include "std_cpu_ops.h"
#include <stdio.h>
#include "cpu_common/cpu_ops.h"
#include "cpu_dec/op_parse.h"
#include "cpu_exec/op_exec.h"
#include "mpu_types.h"

CpuType virtual_cpu;

void cpu_init(void)
{
	CoreIdType i;
	for (i = 0; i < cpu_config_get_core_id_num(); i++) {
		virtual_cpu.cores[i].core.core_id = i;
		cpu_reset(i);
	}
	return;
}
typedef struct {
	CpuMemoryAccessType access_type;
	uint32 addr;
	uint32 size;
} CpuMemoryCheckType;

/*
 * FALSE: permission OK
 * TRUE:  permission NG
 */
static bool dmp_object_filter(const void *p, const void *arg)
{
	TargetCoreMpuDataConfigType *config = (TargetCoreMpuDataConfigType*)p;
	CpuMemoryCheckType *check_arg = (CpuMemoryCheckType*)arg;

	if (config->common.enable == FALSE) {
		return FALSE;
	}
	if (config->common.is_mask_method == FALSE) {
		//TODO upper lower
	}
	else {
		//TODO mask method
	}
	return TRUE;
}

static bool cpu_has_permission_dmp(CoreIdType core_id, CpuMemoryAccessType access_type, uint32 addr, uint32 size)
{
	ObjectContainerType *container = virtual_cpu.cores[core_id].core.mpu.data_configs.region_permissions;
	void *obj;
	CpuMemoryCheckType arg;

	arg.access_type = access_type;
	arg.addr = addr;
	arg.size = size;

	obj = object_container_find_first2(container, dmp_object_filter, &arg);
	if (obj != NULL) {
		return FALSE;
	}
	else {
		return TRUE;
	}
}

static bool cpu_has_permission_imp(CoreIdType core_id, CpuMemoryAccessType access_type, uint32 addr, uint32 size)
{
	return TRUE;
}
bool cpu_has_permission(CoreIdType core_id, MpuAddressRegionEnumType region_type, CpuMemoryAccessType access_type, uint32 addr, uint32 size)
{
	uint32 psw = cpu_get_psw(&virtual_cpu.cores[core_id].core.reg.sys);
	bool permission = FALSE;

	switch (region_type) {
	case GLOBAL_MEMORY:
		if (IS_TRUSTED_DMP(psw)) {
			permission = TRUE;
		}
		else {
			permission = cpu_has_permission_dmp(core_id, access_type, addr, size);
		}

		break;
	case READONLY_MEMORY:
		if (IS_TRUSTED_IMP(psw)) {
			permission = TRUE;
		}
		else {
			permission = cpu_has_permission_imp(core_id, access_type, addr, size);
		}
		break;
	case DEVICE:
		permission = IS_TRUSTED_PP(psw);
		break;
	default:
		break;
	}
	return permission;
}

static void private_cpu_mpu_set_common_obj(TargetCoreMpuConfigType *config, uint32 au, uint32 al)
{
	config->al = (al & 0xFFFFFF0);
	config->au = ( (au & 0xFFFFFF0) | 0x0F );

	if ((al & 0x04) != 0x00) {
		config->is_mask_method = TRUE;
	}
	else {
		config->is_mask_method = FALSE;
	}

	if ((al & 0x01) != 0x00) {
		config->enable = TRUE;
	}
	else {
		config->enable = FALSE;
	}
	return;
}

void private_cpu_mpu_construct_containers(TargetCoreType *cpu)
{
	int i;
	uint32 *setting_sysreg;

	setting_sysreg = cpu_get_mpu_settign_sysreg(&cpu->reg.sys);

	if (cpu->mpu.exec_configs.region_permissions != NULL) {
		object_container_delete(cpu->mpu.exec_configs.region_permissions);
		cpu->mpu.exec_configs.region_permissions = NULL;
	}
	if (cpu->mpu.data_configs.region_permissions != NULL) {
		object_container_delete(cpu->mpu.data_configs.region_permissions);
		cpu->mpu.data_configs.region_permissions = NULL;
	}
	cpu->mpu.exec_configs.region_permissions = object_container_create(sizeof(TargetCoreMpuExecConfigType), TARGET_CORE_MPU_CONFIG_EXEC_MAXNUM);
	cpu->mpu.data_configs.region_permissions = object_container_create(sizeof(TargetCoreMpuDataConfigType), TARGET_CORE_MPU_CONFIG_DATA_MAXNUM);

	/*
	 * exec
	 */
	ObjectContainerType	*container;

	container = cpu->mpu.exec_configs.region_permissions;
	for (i = 0; i < TARGET_CORE_MPU_CONFIG_EXEC_MAXNUM; i++) {
		TargetCoreMpuExecConfigType *obj = (TargetCoreMpuExecConfigType *)object_container_create_element(container);
		uint32 al = setting_sysreg[SYS_REG_MPU_IPA0L + (i * 2)];
		uint32 au = setting_sysreg[SYS_REG_MPU_IPA0U + (i * 2)];

		private_cpu_mpu_set_common_obj(&obj->common, au, al);
		if ((au & 0x02) != 0x00) {
			obj->enable_read = TRUE;
		}
		else {
			obj->enable_read = FALSE;
		}

		if ((al & 0x01) != 0x00) {
			obj->enable_exec = TRUE;
		}
		else {
			obj->enable_exec = FALSE;
		}
	}

	/*
	 * data
	 */
	for (i = 0; i < TARGET_CORE_MPU_CONFIG_DATA_MAXNUM; i++) {
		TargetCoreMpuDataConfigType *obj = (TargetCoreMpuDataConfigType *)object_container_create_element(container);
		uint32 al = setting_sysreg[SYS_REG_MPU_DPA0L + (i * 2)];
		uint32 au = setting_sysreg[SYS_REG_MPU_DPA0U + (i * 2)];

		private_cpu_mpu_set_common_obj(&obj->common, au, al);
		if ((au & 0x02) != 0x00) {
			obj->enable_read = TRUE;
		}
		else {
			obj->enable_read = FALSE;
		}

		if ((al & 0x04) != 0x00) {
			obj->enable_write = TRUE;
		}
		else {
			obj->enable_write = FALSE;
		}
	}

	return;
}

static void private_cpu_mpu_init(TargetCoreType *cpu)
{
	uint32 *setting_sysreg;

	cpu->mpu.exception_error_code = CpuExceptionError_None;

	//mpu register initial values
	setting_sysreg = cpu_get_mpu_settign_sysreg(&cpu->reg.sys);
	//IPA0L-IPA4L 			0000 0002H
	setting_sysreg[SYS_REG_MPU_IPA0L] = 0x00000002;
	setting_sysreg[SYS_REG_MPU_IPA1L] = 0x00000002;
	setting_sysreg[SYS_REG_MPU_IPA2L] = 0x00000002;
	setting_sysreg[SYS_REG_MPU_IPA3L] = 0x00000002;
	setting_sysreg[SYS_REG_MPU_IPA4L] = 0x00000002;

	//IPA0U-IPA4U 			0000 0000H
	setting_sysreg[SYS_REG_MPU_IPA0U] = 0x00000000;
	setting_sysreg[SYS_REG_MPU_IPA1U] = 0x00000000;
	setting_sysreg[SYS_REG_MPU_IPA2U] = 0x00000000;
	setting_sysreg[SYS_REG_MPU_IPA3U] = 0x00000000;
	setting_sysreg[SYS_REG_MPU_IPA4U] = 0x00000000;

	//DPA0L 				0000 0002H
	setting_sysreg[SYS_REG_MPU_DPA0L] = 0x00000006;
	//DPA1L-DPA4L 			0000 0002H
	setting_sysreg[SYS_REG_MPU_DPA1L] = 0x00000002;
	setting_sysreg[SYS_REG_MPU_DPA2L] = 0x00000002;
	setting_sysreg[SYS_REG_MPU_DPA3L] = 0x00000002;
	setting_sysreg[SYS_REG_MPU_DPA4L] = 0x00000002;
	//DPA5L 				0000 0006H
	setting_sysreg[SYS_REG_MPU_DPA5L] = 0x00000006;

	//DPA0U 				0000 0006H
	setting_sysreg[SYS_REG_MPU_DPA0U] = 0x00000006;
	//DPA1U-DPA4U 			0000 0000H
	setting_sysreg[SYS_REG_MPU_DPA1U] = 0x00000000;
	setting_sysreg[SYS_REG_MPU_DPA2U] = 0x00000000;
	setting_sysreg[SYS_REG_MPU_DPA3U] = 0x00000000;
	setting_sysreg[SYS_REG_MPU_DPA4U] = 0x00000000;
	//DPA5U 				0000 0000H
	setting_sysreg[SYS_REG_MPU_DPA5U] = 0x00000000;

	private_cpu_mpu_construct_containers(cpu);
	return;
}


static void private_cpu_reset(TargetCoreType *cpu)
{
	uint32 *sysreg;
	cpu->reg.pc = 0x00;
	cpu->reg.r[0] = 0;

	cpu->reg.sys.current_grp = SYS_GRP_CPU;
	cpu->reg.sys.current_bnk = SYS_GRP_CPU_BNK_0;
	for (int regId = 0; regId < CPU_GREG_NUM; regId++) {
		sysreg = cpu_get_sysreg(&cpu->reg.sys, regId);
		*sysreg = 0;
	}
	sys_get_cpu_base(&cpu->reg)->r[SYS_REG_PSW] = 0x20;

	cpu->is_halt = FALSE;

	/*
	 * MPU
	 */
	private_cpu_mpu_init(cpu);
	return;
}

void cpu_reset(CoreIdType core_id)
{
	private_cpu_reset(&virtual_cpu.cores[core_id].core);
	return;
}
bool cpu_is_halt(CoreIdType core_id)
{
	return virtual_cpu.cores[core_id].core.is_halt;
}
void cpu_set_current_core(CoreIdType core_id)
{
	virtual_cpu.current_core = &virtual_cpu.cores[core_id];
	return;
}

Std_ReturnType cpu_supply_clock(CoreIdType core_id)
{
	OperationCodeType optype;
	int ret;
	Std_ReturnType err;
	uint32 inx;
	CachedOperationCodeType *cached_code;

	if (virtual_cpu.cores[core_id].core.is_halt == TRUE) {
		return STD_E_OK;
	}

	cached_code = virtual_cpu_get_cached_code(virtual_cpu.cores[core_id].core.reg.pc);
	inx = virtual_cpu.cores[core_id].core.reg.pc - cached_code->code_start_addr;
	if (cached_code->codes[inx].op_exec == NULL) {
		/*
		 * 命令取得する
		 */
		err = bus_get_pointer(core_id,
				virtual_cpu.cores[core_id].core.reg.pc,
				(uint8**)&(virtual_cpu.cores[core_id].core.current_code));
		if (err != STD_E_OK) {
			return err;
		}

		/*
		 * デコード
		 */
		ret = op_parse(virtual_cpu.cores[core_id].core.current_code,
				&cached_code->codes[inx].decoded_code, &optype);
		if (ret < 0) {
			printf("Decode Error\n");
			return STD_E_DECODE;
		}
		if (op_exec_table[optype.code_id].exec == NULL) {
			printf("Not supported code(%d fmt=%d) Error code[0]=0x%x code[1]=0x%x type_id=0x%x\n",
					optype.code_id, optype.format_id,
					virtual_cpu.cores[core_id].core.current_code[0],
					virtual_cpu.cores[core_id].core.current_code[1],
					virtual_cpu.cores[core_id].core.decoded_code->type_id);
			return STD_E_EXEC;
		}

		virtual_cpu.cores[core_id].core.decoded_code = &cached_code->codes[inx].decoded_code;
		/*
		 * 命令実行
		 */
		ret = op_exec_table[optype.code_id].exec(&virtual_cpu.cores[core_id].core);
		if (ret < 0) {
			printf("Exec Error code[0]=0x%x code[1]=0x%x type_id=0x%x\n",
					virtual_cpu.cores[core_id].core.current_code[0],
					virtual_cpu.cores[core_id].core.current_code[1],
					virtual_cpu.cores[core_id].core.decoded_code->type_id);
			return STD_E_EXEC;
		}
		cached_code->codes[inx].op_exec = op_exec_table[optype.code_id].exec;
	}
	else {
		virtual_cpu.cores[core_id].core.decoded_code = &cached_code->codes[inx].decoded_code;
		ret = cached_code->codes[inx].op_exec(&virtual_cpu.cores[core_id].core);
		if (ret < 0) {
			printf("Exec Error code[0]=0x%x code[1]=0x%x type_id=0x%x\n",
					virtual_cpu.cores[core_id].core.current_code[0],
					virtual_cpu.cores[core_id].core.current_code[1],
					virtual_cpu.cores[core_id].core.decoded_code->type_id);
			return STD_E_EXEC;
		}
		virtual_cpu.cores[core_id].core.reg.r[0] = 0U;

	}
	return STD_E_OK;
}

void cpu_illegal_opcode_trap(CoreIdType core_id)
{
	uint32 eicc;
	uint32 ecr;

	eicc = 0x60;
	sys_get_cpu_base(&virtual_cpu.cores[core_id].core.reg)->r[SYS_REG_EIPC] = virtual_cpu.cores[core_id].core.reg.pc - 4;
	sys_get_cpu_base(&virtual_cpu.cores[core_id].core.reg)->r[SYS_REG_EIPSW] = sys_get_cpu_base(&virtual_cpu.cores[core_id].core.reg)->r[SYS_REG_PSW];

	ecr = sys_get_cpu_base(&virtual_cpu.cores[core_id].core.reg)->r[SYS_REG_ECR];
	ecr = ecr & 0x00FF;
	ecr |= (eicc << 16);
	sys_get_cpu_base(&virtual_cpu.cores[core_id].core.reg)->r[SYS_REG_ECR] = ecr;
	CPU_SET_NP(&virtual_cpu.cores[core_id].core.reg);
	CPU_SET_EP(&virtual_cpu.cores[core_id].core.reg);
	CPU_SET_ID(&virtual_cpu.cores[core_id].core.reg);
	virtual_cpu.cores[core_id].core.reg.pc = 0x60;

	return;
}


static Std_ReturnType cpu_get_data32(MpuAddressRegionType *region, CoreIdType core_id, uint32 addr, uint32 *data);
static Std_ReturnType cpu_put_data32(MpuAddressRegionType *region, CoreIdType core_id, uint32 addr, uint32 data);

MpuAddressRegionOperationType cpu_register_operation = {
		.get_data8 = NULL,
		.get_data16 = NULL,
		.get_data32 = cpu_get_data32,
		.put_data8 = NULL,
		.put_data16 = NULL,
		.put_data32 = cpu_put_data32,
};
static uint32 *get_cpu_register_addr(MpuAddressRegionType *region, TargetCoreType *core, uint32 addr)
{
	uint32 inx = (addr - CPU_CONFIG_DEBUG_REGISTER_ADDR) / sizeof(uint32);

	//printf("get_cpu_register_addr:inx=%u\n", inx);
	if (inx >= 0 && inx <= 31) {
		return (uint32*)&core->reg.r[inx];
	}
	else if (addr == CPU_CONFIG_ADDR_PEID) {
		inx = (addr - CPU_CONFIG_DEBUG_REGISTER_ADDR) * core->core_id;
		return (uint32*)&region->data[inx];
	}
	else if ((addr >= CPU_CONFIG_ADDR_MEV_0) && (addr <= CPU_CONFIG_ADDR_MEV_7)) {
		inx = (addr - CPU_CONFIG_DEBUG_REGISTER_ADDR);
		return (uint32*)&region->data[inx];
	}
	else if ((addr >= CPU_CONFIG_ADDR_MIR_0) && (addr <= CPU_CONFIG_ADDR_MIR_1)) {
		inx = (addr - CPU_CONFIG_DEBUG_REGISTER_ADDR);
		return (uint32*)&region->data[inx];
	}
	return NULL;
}
static Std_ReturnType cpu_get_data32(MpuAddressRegionType *region, CoreIdType core_id, uint32 addr, uint32 *data)
{
	uint32 *registerp = get_cpu_register_addr(region, &virtual_cpu.current_core->core, addr);
	if (registerp == NULL) {
		return STD_E_SEGV;
	}
	else if (addr == CPU_CONFIG_ADDR_PEID) {
		*registerp = (core_id + 1);
	}
	*data = *registerp;
	return STD_E_OK;
}

static Std_ReturnType cpu_put_data32(MpuAddressRegionType *region, CoreIdType core_id, uint32 addr, uint32 data)
{
	uint32 *registerp = get_cpu_register_addr(region, &virtual_cpu.current_core->core, addr);
	if (registerp == NULL) {
		return STD_E_SEGV;
	}
	else if (addr == CPU_CONFIG_ADDR_PEID) {
		return STD_E_SEGV;
	}
	else if ((addr == CPU_CONFIG_ADDR_MIR_0)) {
		intc_cpu_trigger_interrupt(core_id, CPU_CONFIG_ADDR_MIR_0_INTNO);
		return STD_E_OK;
	}
	else if ((addr == CPU_CONFIG_ADDR_MIR_1)) {
		intc_cpu_trigger_interrupt(core_id, CPU_CONFIG_ADDR_MIR_1_INTNO);
		return STD_E_OK;
	}
	*registerp = data;
	return STD_E_OK;
}


uint32 cpu_get_pc(const TargetCoreType *core)
{
	return core->reg.pc;
}
uint32 cpu_get_ep(const TargetCoreType *core)
{
	return core->reg.r[30];
}
uint32 cpu_get_current_core_id(void)
{
	return ((const TargetCoreType *)virtual_cpu.current_core)->core_id;
}
uint32 cpu_get_current_core_pc(void)
{
	return cpu_get_pc((const TargetCoreType *)virtual_cpu.current_core);
}

uint32 cpu_get_current_core_register(uint32 inx)
{
	return ((TargetCoreType *)virtual_cpu.current_core)->reg.r[inx];
}

uint32 cpu_get_sp(const TargetCoreType *core)
{
	return core->reg.r[3];
}
uint32 cpu_get_current_core_sp(void)
{
	return cpu_get_sp((const TargetCoreType *)virtual_cpu.current_core);
}
uint32 cpu_get_current_core_ep(void)
{
	return cpu_get_ep((const TargetCoreType *)virtual_cpu.current_core);
}


uint32 cpu_get_return_addr(const TargetCoreType *core)
{
	return core->reg.r[31];
}
CoreIdType cpu_get_core_id(const TargetCoreType *core)
{
	return core->core_id;
}

