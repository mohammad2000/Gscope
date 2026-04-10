project('gscope', 'c',
    version: '0.1.0',
    license: 'Apache-2.0',
    default_options: [
        'c_std=c11',
        'warning_level=3',
        'b_pie=true',
        'b_lto=true',
    ],
    meson_version: '>= 0.60.0',
)

# ─── Compiler Configuration ─────────────────────────────────────────

cc = meson.get_compiler('c')

add_project_arguments(
    '-D_GNU_SOURCE',
    '-D_POSIX_C_SOURCE=200809L',
    '-Wno-unused-parameter',
    language: 'c',
)

# ─── Dependencies ───────────────────────────────────────────────────

thread_dep = dependency('threads')
rt_dep = cc.find_library('rt', required: false)
crypt_dep = cc.find_library('crypt', required: false)

# ─── Feature Detection ──────────────────────────────────────────────

config_h = configuration_data()

config_h.set10('HAVE_CLONE3',
    cc.has_header_symbol('sched.h', 'CLONE_INTO_CGROUP',
                         prefix: '#define _GNU_SOURCE'))

config_h.set10('HAVE_PIDFD_OPEN',
    cc.has_function('pidfd_open', prefix: '#include <sys/syscall.h>',
                    args: '-D_GNU_SOURCE'))

config_h.set10('HAVE_PIVOT_ROOT', true)

config_h.set10('HAVE_SECCOMP',
    cc.has_header('linux/seccomp.h'))

config_h.set10('HAVE_ACL',
    cc.has_header('sys/acl.h'))

configure_file(output: 'config.h', configuration: config_h)

# ─── Include Directories ────────────────────────────────────────────

inc = include_directories('include', 'src')

# ─── Library Sources ────────────────────────────────────────────────

lib_sources = files(
    'src/error.c',
    'src/log.c',
    'src/util.c',
    'src/scope.c',
    'src/cgroup/cgroup.c',
    'src/cgroup/cgroup_stats.c',
    'src/fs/rootfs.c',
    'src/fs/overlay.c',
    'src/fs/mount.c',
    'src/fs/pivot.c',
    'src/ns/namespace.c',
    'src/ns/userns.c',
    'src/ns/pidns.c',
    'src/proc/spawn.c',
    'src/proc/pty.c',
    'src/proc/pidfd.c',
    'src/proc/wait.c',
    'src/sec/seccomp.c',
    'src/sec/caps.c',
    'src/sec/priv.c',
    'src/net/netlink.c',
    'src/net/bridge.c',
    'src/net/veth.c',
    'src/net/addr.c',
    'src/net/route.c',
    'src/net/firewall.c',
    'src/net/ip_alloc.c',
    'src/user/user.c',
    'src/state.c',
    'src/template/cjson.c',
    'src/template/template.c',
    'src/template/template_vars.c',
    'src/template/template_exec.c',
    'src/template/template_pkg.c',
    'src/template/template_file.c',
    'src/template/template_verify.c',
)

# ─── Shared Library ─────────────────────────────────────────────────

# Version script (Linux only — macOS uses -exported_symbols_list)
if host_machine.system() == 'linux'
    version_script_args = ['-Wl,--version-script=' + meson.current_source_dir() / 'gscope.sym']
    version_script_deps = files('gscope.sym')
else
    version_script_args = []
    version_script_deps = []
endif

libgscope = shared_library('gscope',
    lib_sources,
    include_directories: inc,
    dependencies: [thread_dep, rt_dep, crypt_dep],
    version: meson.project_version(),
    soversion: '0',
    link_args: version_script_args,
    link_depends: version_script_deps,
    install: true,
)

# Static library for tests
libgscope_static = static_library('gscope_static',
    lib_sources,
    include_directories: inc,
    dependencies: [thread_dep, rt_dep, crypt_dep],
)

# ─── Dependency Object (for subprojects / CLI) ──────────────────────

gscope_dep = declare_dependency(
    include_directories: include_directories('include'),
    link_with: libgscope,
)

# ─── Install Headers ────────────────────────────────────────────────

install_headers(
    'include/gscope/gscope.h',
    'include/gscope/types.h',
    'include/gscope/error.h',
    'include/gscope/scope.h',
    'include/gscope/namespace.h',
    'include/gscope/cgroup.h',
    'include/gscope/network.h',
    'include/gscope/process.h',
    'include/gscope/rootfs.h',
    'include/gscope/security.h',
    'include/gscope/user.h',
    'include/gscope/template.h',
    subdir: 'gscope',
)

# ─── pkg-config ─────────────────────────────────────────────────────

pkg = import('pkgconfig')
pkg.generate(libgscope,
    name: 'gscope',
    description: 'Lightweight Linux scope isolation library',
    url: 'https://github.com/gritiva/gscope',
)

# ─── Tests ───────────────────────────────────────────────────────────

if get_option('tests')
    subdir('tests')
endif

# ─── CLI ─────────────────────────────────────────────────────────────

if get_option('cli')
    subdir('cli')
endif
