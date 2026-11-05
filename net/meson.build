system_ss.add(files(
  'announce.c',
  'checksum.c',
  'dgram.c',
  'dump.c',
  'eth.c',
  'filter-buffer.c',
  'filter-mirror.c',
  'filter.c',
  'hub.c',
  'net.c',
  'queue.c',
  'socket.c',
  'stream.c',
  'stream_data.c',
  'util.c',
))
if have_hmp
endif

if get_option('replication').allowed() or \
    get_option('colo_proxy').allowed()
else
  system_ss.add(files('colo-stubs.c'))
endif

if get_option('colo_proxy').allowed()
endif

system_ss.add(when: 'CONFIG_TCG', if_true: files('filter-replay.c'))

if have_l2tpv3
endif
if enable_passt
endif
system_ss.add(when: slirp, if_true: files('slirp.c'))
if have_netmap
endif


if have_vhost_net_user
endif
stub_ss.add(files('vhost-user-stub.c'))

if host_os == 'windows'
elif host_os == 'linux'
  system_ss.add(files('tap.c', 'tap-linux.c'))
elif host_os in bsd_oses
  system_ss.add(files('tap.c', ))
elif host_os == 'sunos'
  system_ss.add(files('tap.c', ))
else
  system_ss.add(files('tap.c', ))
endif
if have_vhost_net_vdpa
endif
stub_ss.add(files('vhost-vdpa-stub.c'))

vmnet_files = files(
  
  
  
  
)
system_ss.add(when: vmnet, if_true: vmnet_files)
subdir('can')
