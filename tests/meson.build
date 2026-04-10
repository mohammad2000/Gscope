test_deps = [thread_dep]

test_inc = include_directories('../include', '../src')

test_error = executable('test_error',
    'test_error.c',
    include_directories: test_inc,
    link_with: libgscope_static,
    dependencies: test_deps,
)
test('error', test_error)

test_cgroup = executable('test_cgroup',
    'test_cgroup.c',
    include_directories: test_inc,
    link_with: libgscope_static,
    dependencies: test_deps,
)
test('cgroup', test_cgroup)

test_rootfs = executable('test_rootfs',
    'test_rootfs.c',
    include_directories: test_inc,
    link_with: libgscope_static,
    dependencies: test_deps,
)
test('rootfs', test_rootfs)

test_namespace = executable('test_namespace',
    'test_namespace.c',
    include_directories: test_inc,
    link_with: libgscope_static,
    dependencies: test_deps,
)
test('namespace', test_namespace)

test_spawn = executable('test_spawn',
    'test_spawn.c',
    include_directories: test_inc,
    link_with: libgscope_static,
    dependencies: test_deps,
)
test('spawn', test_spawn)

test_security = executable('test_security',
    'test_security.c',
    include_directories: test_inc,
    link_with: libgscope_static,
    dependencies: test_deps,
)
test('security', test_security)

test_network = executable('test_network',
    'test_network.c',
    include_directories: test_inc,
    link_with: libgscope_static,
    dependencies: test_deps,
)
test('network', test_network)

test_user = executable('test_user',
    'test_user.c',
    include_directories: test_inc,
    link_with: libgscope_static,
    dependencies: test_deps,
)
test('user', test_user)
