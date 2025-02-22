#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/init_task.h>
#include <linux/kernel.h>
#include <linux/lsm_hooks.h>
#include <linux/nsproxy.h>
#include <linux/path.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/version.h>
#include <linux/mount.h>

#include <linux/fs.h>
#include <linux/namei.h>

#ifdef CONFIG_KSU_SUSFS
#include <linux/susfs.h>
#endif // #ifdef CONFIG_KSU_SUSFS

#include "allowlist.h"
#include "core_hook.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "ksud.h"
#include "manager.h"
#include "selinux/selinux.h"
#include "throne_tracker.h"
#include "throne_tracker.h"
#include "kernel_compat.h"

#ifdef CONFIG_KSU_SUSFS
bool susfs_is_allow_su(void)
{
	if (ksu_is_manager()) {
		// we are manager, allow!
		return true;
	}
	return ksu_is_allow_uid(current_uid().val);
}

extern u32 susfs_zygote_sid;
extern bool susfs_is_mnt_devname_ksu(struct path *path);
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
extern bool susfs_is_log_enabled __read_mostly;
#endif
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
extern void susfs_run_try_umount_for_current_mnt_ns(void);
#endif // #ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
static bool susfs_is_umount_for_zygote_system_process_enabled = false;
#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT
extern bool susfs_is_auto_add_sus_bind_mount_enabled;
#endif // #ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT
extern bool susfs_is_auto_add_sus_ksu_default_mount_enabled;
#endif // #ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT
extern bool susfs_is_auto_add_try_umount_for_bind_mount_enabled;
#endif // #ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT

static inline void susfs_on_post_fs_data(void) {
	struct path path;
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
	if (!kern_path(DATA_ADB_UMOUNT_FOR_ZYGOTE_SYSTEM_PROCESS, 0, &path)) {
		susfs_is_umount_for_zygote_system_process_enabled = true;
		path_put(&path);
	}
	pr_info("susfs_is_umount_for_zygote_system_process_enabled: %d\n", susfs_is_umount_for_zygote_system_process_enabled);
#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT
	if (!kern_path(DATA_ADB_NO_AUTO_ADD_SUS_BIND_MOUNT, 0, &path)) {
		susfs_is_auto_add_sus_bind_mount_enabled = false;
		path_put(&path);
	}
	pr_info("susfs_is_auto_add_sus_bind_mount_enabled: %d\n", susfs_is_auto_add_sus_bind_mount_enabled);
#endif // #ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT
	if (!kern_path(DATA_ADB_NO_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT, 0, &path)) {
		susfs_is_auto_add_sus_ksu_default_mount_enabled = false;
		path_put(&path);
	}
	pr_info("susfs_is_auto_add_sus_ksu_default_mount_enabled: %d\n", susfs_is_auto_add_sus_ksu_default_mount_enabled);
#endif // #ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT
	if (!kern_path(DATA_ADB_NO_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT, 0, &path)) {
		susfs_is_auto_add_try_umount_for_bind_mount_enabled = false;
		path_put(&path);
	}
	pr_info("susfs_is_auto_add_try_umount_for_bind_mount_enabled: %d\n", susfs_is_auto_add_try_umount_for_bind_mount_enabled);
#endif // #ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT
}

#endif // #ifdef CONFIG_KSU_SUSFS

static bool ksu_module_mounted = false;

extern int ksu_handle_sepolicy(unsigned long arg3, void __user *arg4);

static bool ksu_su_compat_enabled = true;
extern void ksu_sucompat_init();
extern void ksu_sucompat_exit();

static inline bool is_allow_su()
{
	if (ksu_is_manager()) {
		// we are manager, allow!
		return true;
	}
	return ksu_is_allow_uid(current_uid().val);
}

static inline bool is_unsupported_uid(uid_t uid)
{
#define LAST_APPLICATION_UID 19999
	uid_t appid = uid % 100000;
	return appid > LAST_APPLICATION_UID;
}

static struct group_info root_groups = { .usage = ATOMIC_INIT(2) };

static void setup_groups(struct root_profile *profile, struct cred *cred)
{
	if (profile->groups_count > KSU_MAX_GROUPS) {
		pr_warn("Failed to setgroups, too large group: %d!\n",
			profile->uid);
		return;
	}

	if (profile->groups_count == 1 && profile->groups[0] == 0) {
		// setgroup to root and return early.
		if (cred->group_info)
			put_group_info(cred->group_info);
		cred->group_info = get_group_info(&root_groups);
		return;
	}

	u32 ngroups = profile->groups_count;
	struct group_info *group_info = groups_alloc(ngroups);
	if (!group_info) {
		pr_warn("Failed to setgroups, ENOMEM for: %d\n", profile->uid);
		return;
	}

	int i;
	for (i = 0; i < ngroups; i++) {
		gid_t gid = profile->groups[i];
		kgid_t kgid = make_kgid(current_user_ns(), gid);
		if (!gid_valid(kgid)) {
			pr_warn("Failed to setgroups, invalid gid: %d\n", gid);
			put_group_info(group_info);
			return;
		}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
		group_info->gid[i] = kgid;
#else
		GROUP_AT(group_info, i) = kgid;
#endif
	}

	groups_sort(group_info);
	set_groups(cred, group_info);
}

static void disable_seccomp()
{
	assert_spin_locked(&current->sighand->siglock);
	// disable seccomp
#if defined(CONFIG_GENERIC_ENTRY) &&                                           \
	LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	current_thread_info()->syscall_work &= ~SYSCALL_WORK_SECCOMP;
#else
	current_thread_info()->flags &= ~(TIF_SECCOMP | _TIF_SECCOMP);
#endif

#ifdef CONFIG_SECCOMP
	current->seccomp.mode = 0;
	current->seccomp.filter = NULL;
#else
#endif
}

void ksu_escape_to_root(void)
{
	struct cred *cred;

	rcu_read_lock();

	do {
		cred = (struct cred *)__task_cred((current));
		BUG_ON(!cred);
	} while (!get_cred_rcu(cred));

	if (cred->euid.val == 0) {
		pr_warn("Already root, don't escape!\n");
		rcu_read_unlock();
		return;
	}
	struct root_profile *profile = ksu_get_root_profile(cred->uid.val);

	cred->uid.val = profile->uid;
	cred->suid.val = profile->uid;
	cred->euid.val = profile->uid;
	cred->fsuid.val = profile->uid;

	cred->gid.val = profile->gid;
	cred->fsgid.val = profile->gid;
	cred->sgid.val = profile->gid;
	cred->egid.val = profile->gid;
	cred->securebits = 0;

	BUILD_BUG_ON(sizeof(profile->capabilities.effective) !=
		     sizeof(kernel_cap_t));

	// setup capabilities
	// we need CAP_DAC_READ_SEARCH becuase `/data/adb/ksud` is not accessible for non root process
	// we add it here but don't add it to cap_inhertiable, it would be dropped automaticly after exec!
	u64 cap_for_ksud =
		profile->capabilities.effective | CAP_DAC_READ_SEARCH;
	memcpy(&cred->cap_effective, &cap_for_ksud,
	       sizeof(cred->cap_effective));
	memcpy(&cred->cap_permitted, &profile->capabilities.effective,
	       sizeof(cred->cap_permitted));
	memcpy(&cred->cap_bset, &profile->capabilities.effective,
	       sizeof(cred->cap_bset));

	setup_groups(profile, cred);

	rcu_read_unlock();

	// Refer to kernel/seccomp.c: seccomp_set_mode_strict
	// When disabling Seccomp, ensure that current->sighand->siglock is held during the operation.
	spin_lock_irq(&current->sighand->siglock);
	disable_seccomp();
	spin_unlock_irq(&current->sighand->siglock);

	ksu_setup_selinux(profile->selinux_domain);
}

int ksu_handle_rename(struct dentry *old_dentry, struct dentry *new_dentry)
{
	if (!current->mm) {
		// skip kernel threads
		return 0;
	}

	if (current_uid().val != 1000) {
		// skip non system uid
		return 0;
	}

	if (!old_dentry || !new_dentry) {
		return 0;
	}

	// /data/system/packages.list.tmp -> /data/system/packages.list
	if (strcmp(new_dentry->d_iname, "packages.list")) {
		return 0;
	}

	char path[128];
	char *buf = dentry_path_raw(new_dentry, path, sizeof(path));
	if (IS_ERR(buf)) {
		pr_err("dentry_path_raw failed.\n");
		return 0;
	}

	if (!strstr(buf, "/system/packages.list")) {
		return 0;
	}
	pr_info("renameat: %s -> %s, new path: %s\n", old_dentry->d_iname,
		new_dentry->d_iname, buf);

	ksu_track_throne();

	return 0;
}

static void nuke_ext4_sysfs() {
	struct path path;
	int err = kern_path("/data/adb/modules", 0, &path);
	if (err) {
		pr_err("nuke path err: %d\n", err);
		return;
	}

	struct super_block* sb = path.dentry->d_inode->i_sb;
	const char* name = sb->s_type->name;
	if (strcmp(name, "ext4") != 0) {
		pr_info("nuke but module aren't mounted\n");
		return;
	}

	ext4_unregister_sysfs(sb);
}

int ksu_handle_prctl(int option, unsigned long arg2, unsigned long arg3,
		     unsigned long arg4, unsigned long arg5)
{
	// if success, we modify the arg5 as result!
	u32 *result = (u32 *)arg5;
	u32 reply_ok = KERNEL_SU_OPTION;

	if (KERNEL_SU_OPTION != option) {
		return 0;
	}

	// TODO: find it in throne tracker!
	uid_t current_uid_val = current_uid().val;
	uid_t manager_uid = ksu_get_manager_uid();
	if (current_uid_val != manager_uid &&
	    current_uid_val % 100000 == manager_uid) {
		ksu_set_manager_uid(current_uid_val);
	}

	bool from_root = 0 == current_uid().val;
	bool from_manager = ksu_is_manager();

	if (!from_root && !from_manager) {
		// only root or manager can access this interface
		return 0;
	}

#ifdef CONFIG_KSU_DEBUG
	pr_info("option: 0x%x, cmd: %ld\n", option, arg2);
#endif

	if (arg2 == CMD_BECOME_MANAGER) {
		if (from_manager) {
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("become_manager: prctl reply error\n");
			}
			return 0;
		}
		return 0;
	}

	if (arg2 == CMD_GRANT_ROOT) {
		if (is_allow_su()) {
			pr_info("allow root for: %d\n", current_uid().val);
			ksu_escape_to_root();
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("grant_root: prctl reply error\n");
			}
		}
		return 0;
	}

	// Both root manager and root processes should be allowed to get version
	if (arg2 == CMD_GET_VERSION) {
		u32 version = KERNEL_SU_VERSION;
		if (copy_to_user(arg3, &version, sizeof(version))) {
			pr_err("prctl reply error, cmd: %lu\n", arg2);
		}
		u32 version_flags = 0;
		version_flags |= 0x0;
		if (arg4 && copy_to_user(arg4, &version_flags, sizeof(version_flags))) {
			pr_err("prctl reply error, cmd: %lu\n", arg2);
		}
		return 0;
	}

	if (arg2 == CMD_REPORT_EVENT) {
		if (!from_root) {
			return 0;
		}
		switch (arg3) {
		case EVENT_POST_FS_DATA: {
			static bool post_fs_data_lock = false;
#ifdef CONFIG_KSU_SUSFS
			susfs_on_post_fs_data();
#endif
			if (!post_fs_data_lock) {
				post_fs_data_lock = true;
				pr_info("post-fs-data triggered\n");
				ksu_on_post_fs_data();
			}
			break;
		}
		case EVENT_BOOT_COMPLETED: {
			static bool boot_complete_lock = false;
			if (!boot_complete_lock) {
				boot_complete_lock = true;
				pr_info("boot_complete triggered\n");
			}
			break;
		}
		case EVENT_MODULE_MOUNTED: {
			ksu_module_mounted = true;
			pr_info("module mounted!\n");
			nuke_ext4_sysfs();
			break;
		}
		default:
			break;
		}
		return 0;
	}

	if (arg2 == CMD_SET_SEPOLICY) {
		if (!from_root) {
			return 0;
		}
		if (!ksu_handle_sepolicy(arg3, arg4)) {
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("sepolicy: prctl reply error\n");
			}
		}

		return 0;
	}

	if (arg2 == CMD_CHECK_SAFEMODE) {
		if (ksu_is_safe_mode()) {
			pr_warn("safemode enabled!\n");
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("safemode: prctl reply error\n");
			}
		}
		return 0;
	}

	if (arg2 == CMD_GET_ALLOW_LIST || arg2 == CMD_GET_DENY_LIST) {
		u32 array[128];
		u32 array_length;
		bool success = ksu_get_allow_list(array, &array_length,
						  arg2 == CMD_GET_ALLOW_LIST);
		if (success) {
			if (!copy_to_user(arg4, &array_length,
					  sizeof(array_length)) &&
			    !copy_to_user(arg3, array,
					  sizeof(u32) * array_length)) {
				if (copy_to_user(result, &reply_ok,
						 sizeof(reply_ok))) {
					pr_err("prctl reply error, cmd: %lu\n",
					       arg2);
				}
			} else {
				pr_err("prctl copy allowlist error\n");
			}
		}
		return 0;
	}

	if (arg2 == CMD_UID_GRANTED_ROOT || arg2 == CMD_UID_SHOULD_UMOUNT) {
		uid_t target_uid = (uid_t)arg3;
		bool allow = false;
		if (arg2 == CMD_UID_GRANTED_ROOT) {
			allow = ksu_is_allow_uid(target_uid);
		} else if (arg2 == CMD_UID_SHOULD_UMOUNT) {
			allow = ksu_uid_should_umount(target_uid);
		} else {
			pr_err("unknown cmd: %lu\n", arg2);
		}
		if (!copy_to_user(arg4, &allow, sizeof(allow))) {
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("prctl reply error, cmd: %lu\n", arg2);
			}
		} else {
			pr_err("prctl copy err, cmd: %lu\n", arg2);
		}
		return 0;
	}

#ifdef CONFIG_KSU_SUSFS
	if (current_uid_val == 0) {
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
		if (arg2 == CMD_SUSFS_ADD_SUS_PATH) {
			int error = 0;
			if (!ksu_access_ok((void __user*)arg3, sizeof(struct st_susfs_sus_path))) {
				pr_err("susfs: CMD_SUSFS_ADD_SUS_PATH -> arg3 is not accessible\n");
				return 0;
			}
			if (!ksu_access_ok((void __user*)arg5, sizeof(error))) {
				pr_err("susfs: CMD_SUSFS_ADD_SUS_PATH -> arg5 is not accessible\n");
				return 0;
			}
			error = susfs_add_sus_path((struct st_susfs_sus_path __user*)arg3);
			pr_info("susfs: CMD_SUSFS_ADD_SUS_PATH -> ret: %d\n", error);
			if (copy_to_user((void __user*)arg5, &error, sizeof(error)))
				pr_info("susfs: copy_to_user() failed\n");
			return 0;
		}
#endif //#ifdef CONFIG_KSU_SUSFS_SUS_PATH
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
		if (arg2 == CMD_SUSFS_ADD_SUS_MOUNT) {
			int error = 0;
			if (!ksu_access_ok((void __user*)arg3, sizeof(struct st_susfs_sus_mount))) {
				pr_err("susfs: CMD_SUSFS_ADD_SUS_MOUNT -> arg3 is not accessible\n");
				return 0;
			}
			if (!ksu_access_ok((void __user*)arg5, sizeof(error))) {
				pr_err("susfs: CMD_SUSFS_ADD_SUS_MOUNT -> arg5 is not accessible\n");
				return 0;
			}
			error = susfs_add_sus_mount((struct st_susfs_sus_mount __user*)arg3);
			pr_info("susfs: CMD_SUSFS_ADD_SUS_MOUNT -> ret: %d\n", error);
			if (copy_to_user((void __user*)arg5, &error, sizeof(error)))
				pr_info("susfs: copy_to_user() failed\n");
			return 0;
		}
#endif //#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
		if (arg2 == CMD_SUSFS_ADD_SUS_KSTAT) {
			int error = 0;
			if (!ksu_access_ok((void __user*)arg3, sizeof(struct st_susfs_sus_kstat))) {
				pr_err("susfs: CMD_SUSFS_ADD_SUS_KSTAT -> arg3 is not accessible\n");
				return 0;
			}
			if (!ksu_access_ok((void __user*)arg5, sizeof(error))) {
				pr_err("susfs: CMD_SUSFS_ADD_SUS_KSTAT -> arg5 is not accessible\n");
				return 0;
			}
			error = susfs_add_sus_kstat((struct st_susfs_sus_kstat __user*)arg3);
			pr_info("susfs: CMD_SUSFS_ADD_SUS_KSTAT -> ret: %d\n", error);
			if (copy_to_user((void __user*)arg5, &error, sizeof(error)))
				pr_info("susfs: copy_to_user() failed\n");
			return 0;
		}
		if (arg2 == CMD_SUSFS_UPDATE_SUS_KSTAT) {
			int error = 0;
			if (!ksu_access_ok((void __user*)arg3, sizeof(struct st_susfs_sus_kstat))) {
				pr_err("susfs: CMD_SUSFS_UPDATE_SUS_KSTAT -> arg3 is not accessible\n");
				return 0;
			}
			if (!ksu_access_ok((void __user*)arg5, sizeof(error))) {
				pr_err("susfs: CMD_SUSFS_UPDATE_SUS_KSTAT -> arg5 is not accessible\n");
				return 0;
			}
			error = susfs_update_sus_kstat((struct st_susfs_sus_kstat __user*)arg3);
			pr_info("susfs: CMD_SUSFS_UPDATE_SUS_KSTAT -> ret: %d\n", error);
			if (copy_to_user((void __user*)arg5, &error, sizeof(error)))
				pr_info("susfs: copy_to_user() failed\n");
			return 0;
		}
		if (arg2 == CMD_SUSFS_ADD_SUS_KSTAT_STATICALLY) {
			int error = 0;
			if (!ksu_access_ok((void __user*)arg3, sizeof(struct st_susfs_sus_kstat))) {
				pr_err("susfs: CMD_SUSFS_ADD_SUS_KSTAT_STATICALLY -> arg3 is not accessible\n");
				return 0;
			}
			if (!ksu_access_ok((void __user*)arg5, sizeof(error))) {
				pr_err("susfs: CMD_SUSFS_ADD_SUS_KSTAT_STATICALLY -> arg5 is not accessible\n");
				return 0;
			}
			error = susfs_add_sus_kstat((struct st_susfs_sus_kstat __user*)arg3);
			pr_info("susfs: CMD_SUSFS_ADD_SUS_KSTAT_STATICALLY -> ret: %d\n", error);
			if (copy_to_user((void __user*)arg5, &error, sizeof(error)))
				pr_info("susfs: copy_to_user() failed\n");
			return 0;
        }
#endif //#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
		if (arg2 == CMD_SUSFS_ADD_TRY_UMOUNT) {
			int error = 0;
			if (!ksu_access_ok((void __user*)arg3, sizeof(struct st_susfs_try_umount))) {
				pr_err("susfs: CMD_SUSFS_ADD_TRY_UMOUNT -> arg3 is not accessible\n");
				return 0;
			}
			if (!ksu_access_ok((void __user*)arg5, sizeof(error))) {
				pr_err("susfs: CMD_SUSFS_ADD_TRY_UMOUNT -> arg5 is not accessible\n");
				return 0;
			}
			error = susfs_add_try_umount((struct st_susfs_try_umount __user*)arg3);
			pr_info("susfs: CMD_SUSFS_ADD_TRY_UMOUNT -> ret: %d\n", error);
			if (copy_to_user((void __user*)arg5, &error, sizeof(error)))
				pr_info("susfs: copy_to_user() failed\n");
			return 0;
		}
		if (arg2 == CMD_SUSFS_RUN_UMOUNT_FOR_CURRENT_MNT_NS) {
			int error = 0;
			susfs_run_try_umount_for_current_mnt_ns();
			pr_info("susfs: CMD_SUSFS_RUN_UMOUNT_FOR_CURRENT_MNT_NS -> ret: %d\n", error);
		}
#endif //#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
		if (arg2 == CMD_SUSFS_SET_UNAME) {
			int error = 0;
			if (!ksu_access_ok((void __user*)arg3, sizeof(struct st_susfs_uname))) {
				pr_err("susfs: CMD_SUSFS_SET_UNAME -> arg3 is not accessible\n");
				return 0;
			}
			if (!ksu_access_ok((void __user*)arg5, sizeof(error))) {
				pr_err("susfs: CMD_SUSFS_SET_UNAME -> arg5 is not accessible\n");
				return 0;
			}
			error = susfs_set_uname((struct st_susfs_uname __user*)arg3);
			pr_info("susfs: CMD_SUSFS_SET_UNAME -> ret: %d\n", error);
			if (copy_to_user((void __user*)arg5, &error, sizeof(error)))
				pr_info("susfs: copy_to_user() failed\n");
			return 0;
		}
#endif //#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
		if (arg2 == CMD_SUSFS_ENABLE_LOG) {
			int error = 0;
			if (arg3 != 0 && arg3 != 1) {
				pr_err("susfs: CMD_SUSFS_ENABLE_LOG -> arg3 can only be 0 or 1\n");
				return 0;
			}
			susfs_set_log(arg3);
			if (copy_to_user((void __user*)arg5, &error, sizeof(error)))
				pr_info("susfs: copy_to_user() failed\n");
			return 0;
		}
#endif //#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
		if (arg2 == CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG) {
			int error = 0;
			if (!ksu_access_ok((void __user*)arg3, SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE)) {
				pr_err("susfs: CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG -> arg3 is not accessible\n");
				return 0;
			}
			if (!ksu_access_ok((void __user*)arg5, sizeof(error))) {
				pr_err("susfs: CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG -> arg5 is not accessible\n");
				return 0;
			}
			error = susfs_set_cmdline_or_bootconfig((char __user*)arg3);
			pr_info("susfs: CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG -> ret: %d\n", error);
			if (copy_to_user((void __user*)arg5, &error, sizeof(error)))
				pr_info("susfs: copy_to_user() failed\n");
			return 0;
		}
#endif //#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT
		if (arg2 == CMD_SUSFS_ADD_OPEN_REDIRECT) {
			int error = 0;
			if (!ksu_access_ok((void __user*)arg3, sizeof(struct st_susfs_open_redirect))) {
				pr_err("susfs: CMD_SUSFS_ADD_OPEN_REDIRECT -> arg3 is not accessible\n");
				return 0;
			}
			if (!ksu_access_ok((void __user*)arg5, sizeof(error))) {
				pr_err("susfs: CMD_SUSFS_ADD_OPEN_REDIRECT -> arg5 is not accessible\n");
				return 0;
			}
			error = susfs_add_open_redirect((struct st_susfs_open_redirect __user*)arg3);
			pr_info("susfs: CMD_SUSFS_ADD_OPEN_REDIRECT -> ret: %d\n", error);
			if (copy_to_user((void __user*)arg5, &error, sizeof(error)))
				pr_info("susfs: copy_to_user() failed\n");
			return 0;
		}
#endif //#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT
		if (arg2 == CMD_SUSFS_SHOW_VERSION) {
			int error = 0;
			int len_of_susfs_version = strlen(SUSFS_VERSION);
			char *susfs_version = SUSFS_VERSION;
			if (!ksu_access_ok((void __user*)arg3, len_of_susfs_version+1)) {
				pr_err("susfs: CMD_SUSFS_SHOW_VERSION -> arg3 is not accessible\n");
				return 0;
			}
			if (!ksu_access_ok((void __user*)arg5, sizeof(error))) {
				pr_err("susfs: CMD_SUSFS_SHOW_VERSION -> arg5 is not accessible\n");
				return 0;
			}
			error = copy_to_user((void __user*)arg3, (void*)susfs_version, len_of_susfs_version+1);
			pr_info("susfs: CMD_SUSFS_SHOW_VERSION -> ret: %d\n", error);
			if (copy_to_user((void __user*)arg5, &error, sizeof(error)))
				pr_info("susfs: copy_to_user() failed\n");
			return 0;
		}
		if (arg2 == CMD_SUSFS_SHOW_ENABLED_FEATURES) {
			int error = 0;
			u64 enabled_features = 0;
			if (!ksu_access_ok((void __user*)arg3, sizeof(u64))) {
				pr_err("susfs: CMD_SUSFS_SHOW_ENABLED_FEATURES -> arg3 is not accessible\n");
				return 0;
			}
			if (!ksu_access_ok((void __user*)arg5, sizeof(error))) {
				pr_err("susfs: CMD_SUSFS_SHOW_ENABLED_FEATURES -> arg5 is not accessible\n");
				return 0;
			}
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
			enabled_features |= (1 << 0);
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
			enabled_features |= (1 << 1);
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT
			enabled_features |= (1 << 2);
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT
			enabled_features |= (1 << 3);
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
			enabled_features |= (1 << 4);
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_OVERLAYFS
			enabled_features |= (1 << 5);
#endif
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
			enabled_features |= (1 << 6);
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT
			enabled_features |= (1 << 7);
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
			enabled_features |= (1 << 8);
#endif
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
			enabled_features |= (1 << 9);
#endif
#ifdef CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS
			enabled_features |= (1 << 10);
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
			enabled_features |= (1 << 11);
#endif
#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT
			enabled_features |= (1 << 12);
#endif
#ifdef CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT
			enabled_features |= (1 << 14);
#endif
			error = copy_to_user((void __user*)arg3, (void*)&enabled_features, sizeof(enabled_features));
			pr_info("susfs: CMD_SUSFS_SHOW_ENABLED_FEATURES -> ret: %d\n", error);
			if (copy_to_user((void __user*)arg5, &error, sizeof(error)))
				pr_info("susfs: copy_to_user() failed\n");
			return 0;
		}
		if (arg2 == CMD_SUSFS_SHOW_VARIANT) {
			int error = 0;
			int len_of_variant = strlen(SUSFS_VARIANT);
			char *susfs_variant = SUSFS_VARIANT;
			if (!ksu_access_ok((void __user*)arg3, len_of_variant+1)) {
				pr_err("susfs: CMD_SUSFS_SHOW_VARIANT -> arg3 is not accessible\n");
				return 0;
			}
			if (!ksu_access_ok((void __user*)arg5, sizeof(error))) {
				pr_err("susfs: CMD_SUSFS_SHOW_VARIANT -> arg5 is not accessible\n");
				return 0;
			}
			error = copy_to_user((void __user*)arg3, (void*)susfs_variant, len_of_variant+1);
			pr_info("susfs: CMD_SUSFS_SHOW_VARIANT -> ret: %d\n", error);
			if (copy_to_user((void __user*)arg5, &error, sizeof(error)))
				pr_info("susfs: copy_to_user() failed\n");
			return 0;
		}
	}
#endif //#ifdef CONFIG_KSU_SUSFS

	// all other cmds are for 'root manager'
	if (!from_manager) {
		return 0;
	}

	// we are already manager
	if (arg2 == CMD_GET_APP_PROFILE) {
		struct app_profile profile;
		if (copy_from_user(&profile, arg3, sizeof(profile))) {
			pr_err("copy profile failed\n");
			return 0;
		}

		bool success = ksu_get_app_profile(&profile);
		if (success) {
			if (copy_to_user(arg3, &profile, sizeof(profile))) {
				pr_err("copy profile failed\n");
				return 0;
			}
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("prctl reply error, cmd: %lu\n", arg2);
			}
		}
		return 0;
	}

	if (arg2 == CMD_SET_APP_PROFILE) {
		struct app_profile profile;
		if (copy_from_user(&profile, arg3, sizeof(profile))) {
			pr_err("copy profile failed\n");
			return 0;
		}

		// todo: validate the params
		if (ksu_set_app_profile(&profile, true)) {
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("prctl reply error, cmd: %lu\n", arg2);
			}
		}
		return 0;
	}

	if (arg2 == CMD_IS_SU_ENABLED) {
		if (copy_to_user(arg3, &ksu_su_compat_enabled,
				 sizeof(ksu_su_compat_enabled))) {
			pr_err("copy su compat failed\n");
			return 0;
		}
		if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
			pr_err("prctl reply error, cmd: %lu\n", arg2);
		}
		return 0;
	}
	if (arg2 == CMD_ENABLE_SU) {
		bool enabled = (arg3 != 0);
		if (enabled == ksu_su_compat_enabled) {
			pr_info("cmd enable su but no need to change.\n");
			return 0;
		}
		if (enabled) {
			ksu_sucompat_init();
		} else {
			ksu_sucompat_exit();
		}
		ksu_su_compat_enabled = enabled;
		if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
			pr_err("prctl reply error, cmd: %lu\n", arg2);
		}
		return 0;
	}
	return 0;
}

static bool is_appuid(kuid_t uid)
{
#define PER_USER_RANGE 100000
#define FIRST_APPLICATION_UID 10000
#define LAST_APPLICATION_UID 19999

	uid_t appid = uid.val % PER_USER_RANGE;
	return appid >= FIRST_APPLICATION_UID && appid <= LAST_APPLICATION_UID;
}

static bool should_umount(struct path *path)
{
	if (!path) {
		return false;
	}

	if (current->nsproxy->mnt_ns == init_nsproxy.mnt_ns) {
		pr_info("ignore global mnt namespace process: %d\n",
			current_uid().val);
		return false;
	}

#ifdef CONFIG_KSU_SUSFS
	return susfs_is_mnt_devname_ksu(path);
#else
	if (path->mnt && path->mnt->mnt_sb && path->mnt->mnt_sb->s_type) {
		const char *fstype = path->mnt->mnt_sb->s_type->name;
		return strcmp(fstype, "overlay") == 0;
	}
	return false;
#endif
}

static void ksu_umount_mnt(struct path *path, int flags)
{
	int err = path_umount(path, flags);
	if (err) {
		pr_info("umount %s failed: %d\n", path->dentry->d_iname, err);
	}
}

#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
void ksu_try_umount(const char *mnt, bool check_mnt, int flags, uid_t uid)
#else
static void ksu_try_umount(const char *mnt, bool check_mnt, int flags)
#endif
{
	struct path path;
	int err = kern_path(mnt, 0, &path);
	if (err) {
		return;
	}

	if (path.dentry != path.mnt->mnt_root) {
		// it is not root mountpoint, maybe umounted by others already.
		return;
	}

	// we are only interest in some specific mounts
	if (check_mnt && !should_umount(&path)) {
		return;
	}

#if defined(CONFIG_KSU_SUSFS_TRY_UMOUNT) && defined(CONFIG_KSU_SUSFS_ENABLE_LOG)
	if (susfs_is_log_enabled) {
		pr_info("susfs: umounting '%s' for uid: %d\n", mnt, uid);
	}
#endif

	ksu_umount_mnt(&path, flags);
}

#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
void susfs_try_umount_all(uid_t uid) {
	susfs_try_umount(uid);
	/* For Legacy KSU only */
	ksu_try_umount("/system", true, 0, uid);
	ksu_try_umount("/system_ext", true, 0, uid);
	ksu_try_umount("/vendor", true, 0, uid);
	ksu_try_umount("/product", true, 0, uid);
	ksu_try_umount("/odm", true, 0, uid);
	// - For '/data/adb/modules' we pass 'false' here because it is a loop device that we can't determine whether 
	//   its dev_name is KSU or not, and it is safe to just umount it if it is really a mountpoint
	ksu_try_umount("/data/adb/modules", false, MNT_DETACH, uid);
	/* For both Legacy KSU and Magic Mount KSU */
	ksu_try_umount("/debug_ramdisk", true, MNT_DETACH, uid);
	ksu_try_umount("/system/etc/hosts", false, MNT_DETACH, uid);
	ksu_try_umount("/apex/com.android.art/bin/dex2oat64", false, MNT_DETACH, uid);
	ksu_try_umount("/apex/com.android.art/bin/dex2oat32", false, MNT_DETACH, uid);
}
#endif

int ksu_handle_setuid(struct cred *new, const struct cred *old)
{
	// this hook is used for umounting overlayfs for some uid, if there isn't any module mounted, just ignore it!
	if (!ksu_module_mounted) {
		return 0;
	}

	if (!new || !old) {
		return 0;
	}

	kuid_t new_uid = new->uid;
	kuid_t old_uid = old->uid;

	if (0 != old_uid.val) {
		// old process is not root, ignore it.
		return 0;
	}

#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
	// check if current process is zygote
	bool is_zygote_child = susfs_is_sid_equal(old->security, susfs_zygote_sid);
	if (likely(is_zygote_child)) {
		// if spawned process is non user app process
		if (unlikely(new_uid.val < 10000 && new_uid.val >= 1000)) {
			// umount for the system process if path DATA_ADB_UMOUNT_FOR_ZYGOTE_SYSTEM_PROCESS exists
			if (susfs_is_umount_for_zygote_system_process_enabled) {
				goto out_ksu_try_umount;
			}
		}
	}
#endif

	if (!is_appuid(new_uid) || is_unsupported_uid(new_uid.val)) {
		// pr_info("handle setuid ignore non application or isolated uid: %d\n", new_uid.val);
		return 0;
	}

	if (ksu_is_allow_uid(new_uid.val)) {
		// pr_info("handle setuid ignore allowed application: %d\n", new_uid.val);
		return 0;
	}
#ifdef CONFIG_KSU_SUSFS
	else {
		task_lock(current);
		current->susfs_task_state |= TASK_STRUCT_NON_ROOT_USER_APP_PROC;
		task_unlock(current);
	}
#endif

#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
out_ksu_try_umount:
#endif
	if (!ksu_uid_should_umount(new_uid.val)) {
		return 0;
	} else {
#ifdef CONFIG_KSU_DEBUG
		pr_info("uid: %d should not umount!\n", current_uid().val);
#endif
	}

#ifndef CONFIG_KSU_SUSFS_SUS_MOUNT
	// check old process's selinux context, if it is not zygote, ignore it!
	// because some su apps may setuid to untrusted_app but they are in global mount namespace
	// when we umount for such process, that is a disaster!
	bool is_zygote_child = ksu_is_zygote(old->security);
#endif
	if (!is_zygote_child) {
		pr_info("handle umount ignore non zygote child: %d\n",
			current->pid);
		return 0;
	}
#ifdef CONFIG_KSU_DEBUG
	// umount the target mnt
	pr_info("handle umount for uid: %d, pid: %d\n", new_uid.val,
		current->pid);
#endif

#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
	// susfs come first, and lastly umount by ksu, make sure umount in reversed order
	susfs_try_umount_all(new_uid.val);
#else
	// fixme: use `collect_mounts` and `iterate_mount` to iterate all mountpoint and
	// filter the mountpoint whose target is `/data/adb`
	ksu_try_umount("/system", true, 0);
	ksu_try_umount("/vendor", true, 0);
	ksu_try_umount("/product", true, 0);
	ksu_try_umount("/system_ext", true, 0);
	ksu_try_umount("/data/adb/modules", false, MNT_DETACH);

	// try umount ksu temp path
	ksu_try_umount("/debug_ramdisk", false, MNT_DETACH);

	// try umount /system/etc/hosts (hosts module)
	ksu_try_umount("/system/etc/hosts", false, MNT_DETACH);
	
	// try umount lsposed dex2oat
	ksu_try_umount("/apex/com.android.art/bin/dex2oat64", false, MNT_DETACH);
	ksu_try_umount("/apex/com.android.art/bin/dex2oat32", false, MNT_DETACH);
	
#endif
	return 0;
}

static int ksu_task_prctl(int option, unsigned long arg2, unsigned long arg3,
			  unsigned long arg4, unsigned long arg5)
{
	ksu_handle_prctl(option, arg2, arg3, arg4, arg5);
	return -ENOSYS;
}
// kernel 4.4 and 4.9
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0) || defined(CONFIG_KSU_ALLOWLIST_WORKAROUND)
static int ksu_key_permission(key_ref_t key_ref, const struct cred *cred,
			      unsigned perm)
{
	if (init_session_keyring != NULL) {
		return 0;
	}
	if (strcmp(current->comm, "init")) {
		// we are only interested in `init` process
		return 0;
	}
	init_session_keyring = cred->session_keyring;
	pr_info("kernel_compat: got init_session_keyring\n");
	return 0;
}
#endif
static int ksu_inode_rename(struct inode *old_inode, struct dentry *old_dentry,
			    struct inode *new_inode, struct dentry *new_dentry)
{
	return ksu_handle_rename(old_dentry, new_dentry);
}

static int ksu_task_fix_setuid(struct cred *new, const struct cred *old,
			       int flags)
{
	return ksu_handle_setuid(new, old);
}

static struct security_hook_list ksu_hooks[] = {
	LSM_HOOK_INIT(task_prctl, ksu_task_prctl),
	LSM_HOOK_INIT(inode_rename, ksu_inode_rename),
	LSM_HOOK_INIT(task_fix_setuid, ksu_task_fix_setuid),
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0) || defined(CONFIG_KSU_ALLOWLIST_WORKAROUND)
	LSM_HOOK_INIT(key_permission, ksu_key_permission)
#endif
};

void __init ksu_lsm_hook_init(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	security_add_hooks(ksu_hooks, ARRAY_SIZE(ksu_hooks), "ksu");
#else
	// https://elixir.bootlin.com/linux/v4.10.17/source/include/linux/lsm_hooks.h#L1892
	security_add_hooks(ksu_hooks, ARRAY_SIZE(ksu_hooks));
#endif
}

void __init ksu_core_init(void)
{
	ksu_lsm_hook_init();
}
