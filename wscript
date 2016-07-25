#! /usr/bin/env python
# encoding: utf-8

import os
import Options

# the following two variables are used by the target "waf dist"
VERSION='0.0.1'
APPNAME='fastproxy'

# these variables are mandatory ('/' are converted automatically)
top = '.'
out = 'build'

def set_options(opt):
	opt.tool_options('compiler_cxx')
	opt.tool_options('python')

	opt.add_option('--build-type', action='store', default='debug,release', help='build the selected variants')
	opt.add_option('--boost-path', action='store', help="'boost' library location root path")
	opt.add_option('--udns-path', action='store', help="'udns' library location root path")
	opt.add_option('--ldns-path', action='store', help="'ldns' library location root path")
	opt.add_option('--unbound-path', action='store', help="'unbound' library location root path")

def configure(conf):
	print Options.options.boost_path
	if Options.options.boost_path:
		conf.env.append_value('CXXFLAGS', ['-I', os.path.join(Options.options.boost_path, 'include')])
		conf.env.append_value('LIBPATH', [os.path.join(Options.options.boost_path, 'lib')])

	if Options.options.udns_path:
		conf.env.append_value('CXXFLAGS', ['-I', Options.options.udns_path])
		conf.env.append_value('LIBPATH', [Options.options.udns_path])

	if Options.options.unbound_path:
		conf.env.append_value('CXXFLAGS', ['-I', os.path.join(Options.options.unbound_path, 'include')])
		conf.env.append_value('LIBPATH', [os.path.join(Options.options.unbound_path, 'lib')])

	if Options.options.ldns_path:
		conf.env.append_value('CXXFLAGS',['-I', os.path.join(Options.options.ldns_path, 'include')])
		conf.env.append_value('LIBPATH', [os.path.join(Options.options.ldns_path,'lib')])

	conf.check_tool('compiler_cxx')
	conf.check_tool('boost')
	conf.check_boost(version='1.61.0')
	conf.check_cxx(fragment='#include <udns.h>\nint main(){return 0;}\n', lib='udns', mandatory=True)
	conf.check_cxx(fragment='#include <ldns/ldns.h>\nint main(){return 0;}\n', lib='ldns', mandatory=True)
	conf.check_cxx(fragment='#include <unbound.h>\nint main(){return 0;}\n', lib='unbound', mandatory=True)

	conf.sub_config('src')

	# create a debug and release builds (variants)
	dbg = conf.env.copy()
	rel = conf.env.copy()

	dbg.set_variant('debug')
	conf.set_env_name('debug', dbg)
	conf.setenv('debug')
	conf.env.append_value('CXXFLAGS', ['-D_REENTRANT', '-DDBG_ENABLED', '-Wall', '-O0', '-ggdb3', '-ftemplate-depth-128'])

	rel.set_variant('release')
	conf.set_env_name('release', rel)
	conf.setenv('release')
	conf.env.append_value('CXXFLAGS', ['-O2', '-DNOTRACE', '-ggdb3'])


def build(bld):

	bld.add_subdirs('src')
	bld.install_as('/etc/zabbix/bin/zbx_fastproxy.py', 'scripts/zbx_fastproxy.py', chmod=755)
	bld.install_as('/etc/fastproxy.conf', 'conf/fastproxy.conf')
	for i in '500 502 503 504'.split():
		bld.install_as('/etc/fastproxy/errors/{0}.http'.format(i), 'conf/errors/{0}.http'.format(i))

	# enable the debug or the release variant, depending on the one wanted
	for obj in bld.all_task_gen[:]:

		# task generator instances (bld.new_task_gen...) should be created in the same order
		# to avoid unnecessary rebuilds (remember that cloning task generators is cheap, but copying task instances are expensive)
		debug_obj = obj.clone('debug')
		release_obj = obj.clone('release')

		# stupid reminder: do not make clones for the same variant (default -> default)

		# disable the original task generator for the default variant (do not use it)
		obj.posted = 1

		# disable the unwanted variant(s)
		kind = Options.options.build_type
		if kind.find('debug') < 0:
			debug_obj.posted = 1
		if kind.find('release') < 0:
			release_obj.posted = 1
