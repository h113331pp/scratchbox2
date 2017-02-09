-- Copyright (C) 2007 Lauri Leukkunen <lle@rahina.org>
-- Licensed under MIT license.
--
-- "simple" mode, to be used for software development & building
-- (as the name says, this is the simple solution; See/use the "devel"
-- mode when a more full-featured environment is needed)

-- Rule file interface version, mandatory.
--
rule_file_interface_version = "105"
----------------------------------

tools = tools_root
if (not tools) then
	tools = "/"
end

qemu_binary_name = basename(sbox_cputransparency_cmd)

simple_rules_usr = {
		{path = "/usr/bin/sb2-show",
		 use_orig_path = true, readonly = true},
		{dir = "/usr/lib/libsb2", use_orig_path = true,
		 readonly = true},

		-- Qemu only:
		{binary_name = qemu_binary_name,
		 prefix = "/usr/lib", map_to = target_root},
		{binary_name = qemu_binary_name,
		 prefix = "/usr/local/lib", map_to = target_root},

		-- perl
		{prefix = "/usr/share/perl5", use_orig_path = true, readonly = true},
		{prefix = "/usr/share/perl/5.22", use_orig_path = true, readonly = true},
		{prefix = "/etc/perl", use_orig_path = true, readonly = true},
		{prefix = "/usr/local/lib/i386-linux-gnu/perl/5.22.1", use_orig_path = true, readonly = true},
		{prefix = "/usr/local/share/perl/5.22.1", use_orig_path = true, readonly = true},
		{prefix = "/usr/lib/i386-linux-gnu/perl5/5.22", use_orig_path = true, readonly = true},
		{prefix = "/usr/lib/i386-linux-gnu/perl/5.22", use_orig_path = true, readonly = true},
		{prefix = "/usr/local/lib/site_perl", use_orig_path = true, readonly = true},
		{prefix = "/usr/lib/i386-linux-gnu/perl-base", use_orig_path = true, readonly = true},

		-- python
		{prefix = "/usr/share/python2.7", use_orig_path = true, readonly = true},
		{prefix = "/usr/lib/python2.7", use_orig_path = true, readonly = true},
		{prefix = "/usr/share/pyshared", use_orig_path = true, readonly = true},

		-- gdk-pixbuf-csource
		{path = "/usr/bin/gdk-pixbuf-csource", map_to = target_root},

		-- pkg-config
		{path = "/usr/bin/pkg-config", map_to = target_root},

		-- for libraspberrypi0, libraspberrypi-doc, libraspberrypi-dev, libraspberrypi-bin
		{prefix = "/opt/", map_to = target_root},

		-- qt
		{path = "/usr/bin/qmake", map_to = target_root},
		{path = "/usr/bin/moc-qt4", map_to = target_root},
		{path = "/usr/lib/arm-linux-gnueabihf/qt-default/qtchooser/default.conf", map_to = target_root},

		-- Defaults:
		{prefix = "/usr/lib/perl", map_to = tools},
		{prefix = "/usr/lib/gcc", map_to = tools},
		{prefix = "/usr/lib", map_to = target_root},
		{prefix = "/usr/include", map_to = target_root},

		{prefix = "/usr/lib/git-core", use_orig_path = true, readonly = true},

		{dir = "/usr", map_to = tools},
		{dir = "/var/lib/dpkg", map_to = target_root},
}

fs_mapping_rules = {
		-- -----------------------------------------------
		-- 1. The session directory
		{dir = session_dir, use_orig_path = true},

		-- -----------------------------------------------
		-- 2. Development environment special destinations:

		{prefix = "/sb2/wrappers",
		 replace_by = sbox_dir.."/share/scratchbox2/wrappers",
		 readonly = true},

		{prefix = "/sb2/scripts",
		 replace_by = sbox_dir.."/share/scratchbox2/scripts",
		 readonly = true},

		{prefix = sbox_user_home_dir, use_orig_path = true},

		{prefix = sbox_dir .. "/share/scratchbox2",
		 use_orig_path = true},

		{dir = "/usr", rules = simple_rules_usr},

		-- -----------------------------------------------
		-- 99. Other rules.
		{prefix = "/home/user", map_to = target_root},
		{prefix = "/home", use_orig_path = true},
		{prefix = "/host_usr", map_to = target_root},
		{prefix = "/tmp", use_orig_path = true},
		{prefix = "/dev", use_orig_path = true},
		{prefix = "/proc", custom_map_funct = sb2_procfs_mapper,
		 virtual_path = true},
		{prefix = "/sys", use_orig_path = true},

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

		{prefix = "/lib", map_to = target_root},
		--

		{prefix = tools, use_orig_path = true},
		{path = "/", use_orig_path = true},
		{prefix = "/", map_to = tools}
}

