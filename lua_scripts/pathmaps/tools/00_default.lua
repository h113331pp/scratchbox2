-- Copyright (C) 2008 Lauri Leukkunen <lle@rahina.org>
-- Copyright (C) 2008 Nokia Corporation.
-- Licensed under MIT license.

-- "tools" mapping mode: Almost everything maps to tools_root.

-- Rule file interface version, mandatory.
--
rule_file_interface_version = "24"
----------------------------------

tools = tools_root
if (not tools) then
	tools = "/"
end

-- Don't map the working directory where sb2 was started, unless
-- that happens to be the root directory.
if sbox_workdir == "/" then
	-- FIXME. There should be a way to skip a rule...
	unmapped_workdir = "/XXXXXX" 
else
	unmapped_workdir = sbox_workdir
end

-- If the permission token exists and contains "root", tools_root directories
-- will be available in R/W mode. Otherwise it will be "mounted" R/O.
local tools_root_is_readonly
local fakeroot_ld_preload = ""
if sb.get_session_perm() == "root" then
	tools_root_is_readonly = false
	fakeroot_ld_preload = ":"..host_ld_preload_fakeroot
else
	tools_root_is_readonly = true
end

-- disable the gcc toolchain tricks. gcc & friends will be available, if
-- those have been installed to tools_root
enable_cross_gcc_toolchain = false

-- This mode can also be used to redirect /var/lib/dpkg/status to another
-- location (our dpkg-checkbuilddeps wrapper needs that)
local var_lib_dpkg_status_location = os.getenv("SBOX_TOOLS_MODE_VAR_LIB_DPKG_STATUS_LOCATION")
if var_lib_dpkg_status_location == nil or var_lib_dpkg_status_location == "" then
	-- Use the default location
	var_lib_dpkg_status_location = tools_root .. "/var/lib/dpkg/status"
end

mapall_chain = {
	next_chain = nil,
	binary = nil,
	rules = {
		{dir = session_dir, use_orig_path = true},

		{path = sbox_cputransparency_method, use_orig_path = true,
		 readonly = true},

		{path = "/usr/bin/sb2-show", use_orig_path = true,
		 readonly = true},

		-- tools_root should not be mapped twice.
		{prefix = tools, use_orig_path = true, readonly = true},

		-- ldconfig is static binary, and needs to be wrapped
		{prefix = "/sb2/wrappers",
		 replace_by = session_dir .. "/wrappers." .. active_mapmode,
		 readonly = true},

		--
		{prefix = "/var/run", map_to = session_dir},

		--
		{dir = "/tmp", map_to = session_dir},

		--
		{prefix = "/dev", use_orig_path = true},
		{dir = "/proc", custom_map_funct = sb2_procfs_mapper,
		 virtual_path = true},
		{prefix = "/sys", use_orig_path = true},

		{prefix = sbox_user_home_dir .. "/.scratchbox2",
		 use_orig_path = true},
		{prefix = sbox_dir .. "/share/scratchbox2",
		 use_orig_path = true},

		--
		-- Following 3 rules are needed because package
		-- "resolvconf" makes resolv.conf to be symlink that
		-- points to /etc/resolvconf/run/resolv.conf and
		-- we want them all to come from host.
		--
		{prefix = "/var/run/resolvconf", force_orig_path = true,
		 readonly = true},
		{prefix = "/etc/resolvconf", force_orig_path = true,
		 readonly = true},
		{prefix = "/etc/resolv.conf", force_orig_path = true,
		 readonly = true},

		--
		{path = "/etc/passwd",
		 use_orig_path = true, readonly = true},

		-- -----------------------------------------------
		-- home directories = not mapped, R/W access
		{prefix = "/home", use_orig_path = true},

		-- -----------------------------------------------

		-- "policy-rc.d" checks if scratchbox-version exists, 
		-- to detect if it is running inside scratchbox..
		{prefix = "/scratchbox/etc/scratchbox-version",
		 replace_by = "/usr/share/scratchbox2/version",
		 log_level = "warning",
		 readonly = true, virtual_path = true},

		-- -----------------------------------------------

		{path = "/var/lib/dpkg/status", replace_by = var_lib_dpkg_status_location,
		 readonly = tools_root_is_readonly},

		-- The default is to map everything to tools_root,
		-- except that we don't map the directory tree where
		-- sb2 was started.
		{prefix = unmapped_workdir, use_orig_path = true},

		{path = "/", use_orig_path = true},
		{prefix = "/", map_to = tools_root,
		 readonly = tools_root_is_readonly}
	}
}

export_chains = {
	mapall_chain
}

-- Exec policy rules.

default_exec_policy = {
	name = "Default",
	native_app_ld_preload_prefix = host_ld_preload..fakeroot_ld_preload,
}

-- For binaries from tools_root:
-- we have "tools' native" and "host's native" binaries, that would look
-- identical (and valid!) to the kernel. But they may need to use different
-- loaders and dynamic libraries! The solution is that we use the location
-- (as determined by the mapping engine) to decide the execution policy.

tools_mode_tools_ld_so = nil		-- default = not needed
tools_mode_tools_ld_library_path = nil	-- default = not needed
tools_mode_tools_ld_library_path_prefix = ""
tools_mode_tools_ld_library_path_suffix = nil

if (conf_tools_sb2_installed) then
	if (conf_tools_ld_so ~= nil) then
		-- use dynamic libraries from tools,
		-- when executing native binaries!
		tools_mode_tools_ld_so = conf_tools_ld_so

		-- FIXME: This exec policy should process (map components of)
		-- the current value of LD_LIBRARY_PATH, and add the results
		-- to tools_mode_tools_ld_library_path just before exec.
		-- This has not been done yet.
	end
	tools_mode_tools_ld_library_path_prefix = conf_tools_ld_so_library_path
else
	tools_mode_tools_ld_library_path_prefix =
		host_ld_library_path_libfakeroot ..
		host_ld_library_path_prefix ..
		host_ld_library_path_libsb2
	tools_mode_tools_ld_library_path_suffix =
		host_ld_library_path_suffix
end

local exec_policy_tools = {
	name = "Tools_root",
	native_app_ld_so = tools_mode_tools_ld_so,
	native_app_ld_so_supports_argv0 = conf_tools_ld_so_supports_argv0,
	native_app_ld_library_path = tools_mode_tools_ld_library_path,

	native_app_ld_library_path_prefix = tools_mode_tools_ld_library_path_prefix,
	native_app_ld_library_path_suffix = tools_mode_tools_ld_library_path_suffix,

	native_app_locale_path = conf_tools_locale_path,
	native_app_gconv_path = conf_tools_gconv_path,
	native_app_message_catalog_prefix = conf_tools_message_catalog_prefix,
}

-- Note that the real path (mapped path) is used when looking up rules!
all_exec_policies_chain = {
	next_chain = nil,
	binary = nil,
	rules = {
		-- Tools binaries:
		{prefix = tools_root, exec_policy = exec_policy_tools},

		-- DEFAULT RULE (must exist):
		{prefix = "/", exec_policy = default_exec_policy}
	}
}

exec_policy_chains = {
	all_exec_policies_chain
}

-- This table lists all exec policies - this is used when the current
-- process wants to locate the currently active policy
all_exec_policies = {
	exec_policy_tools,
	default_exec_policy,
}

