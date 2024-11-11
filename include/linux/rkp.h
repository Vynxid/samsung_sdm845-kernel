#ifndef _RKP_H
#define _RKP_H

#ifndef __ASSEMBLY__

/* uH_RKP Command ID */
enum {
	RKP_START = 1,
	RKP_DEFERRED_START,
	RKP_WRITE_PGT1,
	RKP_WRITE_PGT2,
	RKP_WRITE_PGT3,
	RKP_EMULT_TTBR0,
	RKP_EMULT_TTBR1,
	RKP_EMULT_DORESUME,
	RKP_FREE_PGD,
	RKP_NEW_PGD,
	RKP_KASLR_MEM,
	RKP_FIMC_VERIFY, /* CFP cmds */
	RKP_JOPP_INIT,
	RKP_ROPP_INIT,
	RKP_ROPP_SAVE,
	RKP_ROPP_RELOAD,
	/* and KDT cmds */
	RKP_NOSHIP,
#ifdef CONFIG_RKP_TEST
	CMD_ID_TEST_GET_PAR = 0x81,
	CMD_ID_TEST_GET_RO = 0x83,
	CMD_ID_TEST_GET_VA_XN,
	CMD_ID_TEST_GET_VMM_INFO,
#endif
	RKP_MAX
};

#define RKP_PREFIX  UL(0x83800000)

#ifdef CONFIG_RKP_TEST
#define RKP_INIT_MAGIC		0x5afe0002
#else
#define RKP_INIT_MAGIC		0x5afe0001
#endif

#define RKP_VMM_BUFFER		0x600000
#define RKP_RO_BUFFER		UL(0x800000)

#define RKP_FIMC_FAIL		0x10
#define RKP_FIMC_SUCCESS	0xa5

/* For RKP mem reserves */
#define RKP_NUM_MEM		0x01

#define RKP_ROBUF_START		0x9FA08000
#define RKP_ROBUF_SIZE		(0x7f8000ULL)

#define RKP_PHYS_MAP_START	0xA0200000
#ifdef CONFIG_UH_RKP_8G
#define RKP_PHYS_MAP_SIZE	(8ULL << 20)
#define RKP_PGT_BITMAP_LEN	0x40000
#else
#define RKP_PHYS_MAP_SIZE	(6ULL << 20)
#define RKP_PGT_BITMAP_LEN	0x30000
#endif

#define RKP_RBUF_VA		(phys_to_virt(RKP_ROBUF_START))
#define RO_PAGES		(RKP_ROBUF_SIZE >> PAGE_SHIFT) // (RKP_ROBUF_SIZE/PAGE_SIZE)
#define CRED_JAR_RO		"cred_jar_ro"
#define TSEC_JAR		"tsec_jar"
#define VFSMNT_JAR		"vfsmnt_cache"

#ifdef CONFIG_KNOX_KAP
extern int boot_mode_security;
#endif

extern u8 rkp_pgt_bitmap[];
extern u8 rkp_map_bitmap[];
extern u8 __rkp_start_prot_page[];
extern u8 __rkp_end_prot_page[];
extern u8 rkp_def_init_done;
extern u8 rkp_started;
extern void *rkp_ro_alloc(void);
extern void rkp_ro_free(void *free_addr);
extern unsigned int is_rkp_ro_page(u64 addr);

struct rkp_init { //copy from uh (app/rkp/rkp.h)
	u32 magic;
	u64 vmalloc_start;
	u64 vmalloc_end;
	u64 init_mm_pgd;
	u64 id_map_pgd;
	u64 zero_pg_addr;
	u64 rkp_pgt_bitmap;
	u64 rkp_dbl_bitmap;
	u32 rkp_bitmap_size;
    u32 rkp_prot_page_size;
	u32 no_fimc_verify;
	u64 fimc_phys_addr;
	u64 _text;
	u64 _etext;
	u64 extra_memory_addr;
	u32 extra_memory_size;
	u64 physmap_addr; //not used. what is this for?
	u64 _srodata;
	u64 _erodata;
	u32 large_memory;
};

#ifdef CONFIG_RKP_KDP
typedef struct __attribute__((__packed__)) kdp_init_struct {
	u32 credSize;
	u32 sp_size;
	u32 pgd_mm;
	u32 uid_cred;
	u32 euid_cred;
	u32 gid_cred;
	u32 egid_cred;
	u32 bp_pgd_cred;
	u32 bp_task_cred;
	u32 type_cred;
	u32 security_cred;
	u32 usage_cred;
	u32 cred_task;
	u32 mm_task;
	u32 pid_task;
	u32 rp_task;
	u32 comm_task;
	u32 bp_cred_secptr;
	u64 verifiedbootstate;
} kdp_init_t;


#endif  /* CONFIG_RKP_KDP */

#ifdef CONFIG_RKP_NS_PROT
typedef struct ns_param {
	u32 ns_buff_size;
	u32 ns_size;
	u32 bp_offset;
	u32 sb_offset;
	u32 flag_offset;
	u32 data_offset;
}ns_param_t;

#define rkp_ns_fill_params(nsparam,buff_size,size,bp,sb,flag,data)	\
do {						\
	nsparam.ns_buff_size = (u64)buff_size;		\
	nsparam.ns_size  = (u64)size;		\
	nsparam.bp_offset = (u64)bp;		\
	nsparam.sb_offset = (u64)sb;		\
	nsparam.flag_offset = (u64)flag;		\
	nsparam.data_offset = (u64)data;		\
} while(0)
#endif
#define rkp_is_pg_protected(va)	rkp_is_protected(va, __pa(va), (u64 *)rkp_pgt_bitmap, 1)
#define rkp_is_pg_dbl_mapped(pa) rkp_is_protected((u64)__va(pa), pa, (u64 *)rkp_map_bitmap, 0)

#define RKP_PHYS_ADDR_MASK		((1ULL << 40)-1)

/*
 * The following all assume PHYS_OFFSET is fix addr
 */
#define	PHYS_PFN_OFFSET_MIN_DRAM	(0x80000ULL)
#ifdef CONFIG_UH_RKP_8G
#define	PHYS_PFN_OFFSET_MAX_DRAM	(0x280000ULL)
#else
#define	PHYS_PFN_OFFSET_MAX_DRAM	(0x200000ULL)
#endif

#define FIMC_LIB_OFFSET_VA		(VMALLOC_START + 0xF6000000)
#define FIMC_LIB_START_VA		(FIMC_LIB_OFFSET_VA + 0x04000000)
#define FIMC_VRA_LIB_SIZE		(0x80000)
#define FIMC_DDK_LIB_SIZE		(0x400000)
#define FIMC_RTA_LIB_SIZE		(0x300000)

#define FIMC_LIB_SIZE			(FIMC_VRA_LIB_SIZE + FIMC_DDK_LIB_SIZE + FIMC_RTA_LIB_SIZE)

static inline u64 rkp_get_sys_index(u64 pfn)
{
	if (pfn >= PHYS_PFN_OFFSET_MIN_DRAM
		&& pfn < PHYS_PFN_OFFSET_MAX_DRAM) {
		return ((pfn) - PHYS_PFN_OFFSET);
	}
	return (~0ULL);
}

static inline u8 rkp_is_protected(u64 va, u64 pa, u64 *base_addr, u64 type)
{
	u64 phys_addr = pa & (RKP_PHYS_ADDR_MASK);
	u64 *p = base_addr;
	u64 rindex;
	u8 val;
	u64 index = rkp_get_sys_index((phys_addr>>PAGE_SHIFT));

	if (index == (~0ULL))
		return 0;

	p += (index>>6);
	rindex = index % 64;
	val = (((*p) & (1ULL<<rindex))?1:0);
	return val;
}
#endif //__ASSEMBLY__
#endif //_RKP_H
